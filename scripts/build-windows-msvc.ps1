# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$ObsVersion = "",
    [string]$OnnxRuntimeVersion = "",
    [string]$ModelName = "",
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$Configuration = "Release",
    [string]$PluginVersion = ""
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "dependency-versions.ps1")

if ([string]::IsNullOrWhiteSpace($ObsVersion)) { $ObsVersion = $DpdfnetDefaultObsVersion }
if ([string]::IsNullOrWhiteSpace($OnnxRuntimeVersion)) { $OnnxRuntimeVersion = $DpdfnetDefaultOnnxRuntimeVersion }
if ([string]::IsNullOrWhiteSpace($ModelName)) { $ModelName = $DpdfnetDefaultModelName }
if ([string]::IsNullOrWhiteSpace($PluginVersion)) { $PluginVersion = $DpdfnetDefaultPluginVersion }

foreach ($VersionValue in @($ObsVersion, $OnnxRuntimeVersion)) {
    if ($VersionValue -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') {
        throw "Dependency version '$VersionValue' contains unsupported characters."
    }
}
if ($PluginVersion -notmatch '^\d+\.\d+\.\d+(-[A-Za-z0-9.]+)?$') {
    throw "Plugin version '$PluginVersion' must look like 1.0.0 or 1.0.0-rc1."
}
if ($ModelName -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,62}[A-Za-z0-9_-])?$' -or
    @($ModelName -split '\.', 2)[0] -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
    throw "Model name '$ModelName' must be a simple file name without a trailing period."
}
if ($Configuration -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,30}[A-Za-z0-9_-])?$') {
    throw "Configuration '$Configuration' must be a simple directory name."
}

$KnownOnnxRuntimeHashes = $DpdfnetKnownOnnxRuntimeHashes
$KnownObsArchiveHashes = $DpdfnetKnownObsArchiveHashes

$KissArchiveSha256 = "9c2e19cc34ed910dcb509fd8ab561a523b923b6578703ace8c8f37f5a286bb25"
$SimdeCommit = "f3e8262173b7089db9a9d57a9ecef8dd07ad9c97"
$SimdeArchiveSha256 = "3d95ef8de11ed9aea4e75fc5c1f7b60f1d01b9dfd67b48c42fbffbb3baa64589"

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (!(Test-Path $Path)) {
        return $null
    }

    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

function Assert-NoReparsePoints {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$Recursive
    )

    if (!(Test-Path -LiteralPath $Path)) {
        return
    }

    $Pending = [System.Collections.Generic.Stack[string]]::new()
    $Pending.Push($Path)
    while ($Pending.Count -gt 0) {
        $Item = Get-Item -Force -LiteralPath $Pending.Pop()
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to replace or remove a path containing a reparse point: $($Item.FullName)"
        }
        if ($Recursive -and $Item.PSIsContainer) {
            foreach ($Child in Get-ChildItem -Force -LiteralPath $Item.FullName) {
                $Pending.Push($Child.FullName)
            }
        }
    }
}

function Invoke-DownloadZip {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$ZipPath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ExpectedDirectory,
        [Parameter(Mandatory = $true)][string]$ArchiveRootName,
        [string]$ExpectedSha256 = ""
    )

    Assert-NoReparsePoints -Path $Destination
    Assert-NoReparsePoints -Path (Split-Path $ZipPath)
    Assert-NoReparsePoints -Path $ZipPath
    Assert-NoReparsePoints -Path $ExpectedDirectory -Recursive
    New-Item -ItemType Directory -Force -Path (Split-Path $ZipPath), $Destination | Out-Null

    if ((Test-Path $ZipPath) -and $ExpectedSha256) {
        $currentHash = Get-Sha256 -Path $ZipPath
        if ($currentHash -ne $ExpectedSha256) {
            Write-Host "Discarding cached archive with mismatched hash: $ZipPath"
            Remove-Item -Force $ZipPath
        }
    }
    if ((Test-Path $ZipPath) -and !$ExpectedSha256) {
        # An unpinned archive is never trusted from the mutable local cache.
        # Development builds may use one, but each invocation downloads it
        # again and release-grade gates reject it.
        Remove-Item -Force $ZipPath
    }

    if (!(Test-Path $ZipPath)) {
        Write-Host "Downloading $Uri"
        $null = Invoke-WebRequest -Uri $Uri -OutFile $ZipPath
    }

    $ArchiveSha256 = Get-Sha256 -Path $ZipPath
    if ($ExpectedSha256) {
        if ($ArchiveSha256 -ne $ExpectedSha256) {
            Remove-Item -Force $ZipPath -ErrorAction SilentlyContinue
            throw "Hash mismatch for $ZipPath. Expected $ExpectedSha256, got $ArchiveSha256"
        }
    }

    $ExtractionDirectory = Join-Path $Destination (".extract-" + [guid]::NewGuid().ToString("N"))
    $ReplacementBackup = "$($ExpectedDirectory).replace-" + [guid]::NewGuid().ToString("N")
    Assert-NoReparsePoints -Path $ExtractionDirectory -Recursive
    Assert-NoReparsePoints -Path $ReplacementBackup -Recursive
    New-Item -ItemType Directory -Path $ExtractionDirectory | Out-Null
    try {
        $null = Expand-Archive -Path $ZipPath -DestinationPath $ExtractionDirectory -Force
        Assert-NoReparsePoints -Path $ExtractionDirectory -Recursive
        $ExtractedRoot = Join-Path $ExtractionDirectory $ArchiveRootName
        if (!(Test-Path $ExtractedRoot -PathType Container)) {
            throw "Archive $ZipPath did not contain expected root '$ArchiveRootName'."
        }

        if (Test-Path $ExpectedDirectory) {
            $null = Move-Item -Path $ExpectedDirectory -Destination $ReplacementBackup
        }
        try {
            $null = Move-Item -Path $ExtractedRoot -Destination $ExpectedDirectory
        } catch {
            if ((Test-Path $ReplacementBackup) -and !(Test-Path $ExpectedDirectory)) {
                $null = Move-Item -Path $ReplacementBackup -Destination $ExpectedDirectory
            }
            throw
        }
        if (Test-Path $ReplacementBackup) {
            Assert-NoReparsePoints -Path $ReplacementBackup -Recursive
            Remove-Item -Recurse -Force $ReplacementBackup
        }
    } finally {
        if (Test-Path $ExtractionDirectory) {
            Assert-NoReparsePoints -Path $ExtractionDirectory -Recursive
            Remove-Item -Recurse -Force $ExtractionDirectory
        }
    }

    return $ArchiveSha256
}

function Import-VcVars64 {
    $vcvars = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio" -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1

    if (!$vcvars) {
        throw "Could not find vcvars64.bat. Install Visual Studio Build Tools with the C++ toolchain."
    }

    Write-Host "Using $($vcvars.FullName)"
    $envLines = cmd.exe /s /c "`"$($vcvars.FullName)`" >nul && set"
    foreach ($line in $envLines) {
        if ($line -match "^(.*?)=(.*)$") {
            Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
        }
    }
}

function New-ImportLibrary {
    param(
        [Parameter(Mandatory = $true)][string]$DllPath,
        [Parameter(Mandatory = $true)][string]$LibraryName,
        [Parameter(Mandatory = $true)][string]$OutLib,
        [Parameter(Mandatory = $true)][string]$DefPath
    )

    $exports = & dumpbin.exe /exports $DllPath
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $DllPath"
    }

    $names = foreach ($line in $exports) {
        if ($line -match "^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_]*)") {
            $matches[1]
        }
    }

    $names = $names | Sort-Object -Unique
    if (!$names) {
        throw "Parsed no exports from $DllPath"
    }

    @("LIBRARY $LibraryName", "EXPORTS") + $names | Set-Content -Encoding ASCII $DefPath
    & lib.exe /nologo /machine:x64 /def:$DefPath /out:$OutLib | Write-Host
    if ($LASTEXITCODE -ne 0) {
        throw "lib.exe failed while creating $OutLib"
    }
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $Root "build\msvc"
$OutputDir = Join-Path $BuildDir $Configuration
$ThirdParty = Join-Path $Root "third_party"
$GeneratedObs = Join-Path $BuildDir "generated-obs"
$Models = Join-Path $Root "models"

New-Item -ItemType Directory -Force -Path $BuildDir, $OutputDir, $ThirdParty, $GeneratedObs, $Models | Out-Null

# Remove provenance and test status before touching build outputs. A failed or
# interrupted build must never leave a valid stamp beside partially replaced
# artifacts.
$BuildProvenanceFile = Join-Path $OutputDir "build-provenance.json"
$SourceCommitFile = Join-Path $OutputDir "source-commit.txt"
$TestsPassedFile = Join-Path $OutputDir "tests-passed.txt"
Remove-Item -LiteralPath $BuildProvenanceFile, $SourceCommitFile, $TestsPassedFile `
    -Force -ErrorAction SilentlyContinue

$onnxBase = "onnxruntime-win-x64-$OnnxRuntimeVersion"
$onnxRoot = Join-Path $ThirdParty "onnxruntime"
$onnxZip = Join-Path $ThirdParty "$onnxBase.zip"
$onnxExpectedHash = $KnownOnnxRuntimeHashes[$OnnxRuntimeVersion]
if (!$onnxExpectedHash) {
    Write-Warning "No pinned ONNX Runtime hash is known for version $OnnxRuntimeVersion."
}
$OnnxRuntimeArchiveSha256 = Invoke-DownloadZip `
    -Uri "https://github.com/microsoft/onnxruntime/releases/download/v$OnnxRuntimeVersion/$onnxBase.zip" `
    -ZipPath $onnxZip `
    -Destination $ThirdParty `
    -ExpectedDirectory $onnxRoot `
    -ArchiveRootName $onnxBase `
    -ExpectedSha256 $onnxExpectedHash
$onnxVersionFile = Join-Path $onnxRoot "VERSION_NUMBER"
if (!(Test-Path $onnxVersionFile -PathType Leaf) -or
    (Get-Content -Raw $onnxVersionFile).Trim() -cne $OnnxRuntimeVersion) {
    throw "Extracted ONNX Runtime does not identify itself as $OnnxRuntimeVersion."
}

$obsSourceRoot = Join-Path $ThirdParty "obs-studio-$ObsVersion"
$obsZip = Join-Path $ThirdParty "obs-studio-$ObsVersion.zip"
$obsExpectedHash = $KnownObsArchiveHashes[$ObsVersion]
if (!$obsExpectedHash) {
    Write-Warning "No pinned OBS Studio source hash is known for version $ObsVersion."
}
$ObsSourceArchiveSha256 = Invoke-DownloadZip `
    -Uri "https://github.com/obsproject/obs-studio/archive/refs/tags/$ObsVersion.zip" `
    -ZipPath $obsZip `
    -Destination $ThirdParty `
    -ExpectedDirectory $obsSourceRoot `
    -ArchiveRootName "obs-studio-$ObsVersion" `
    -ExpectedSha256 $obsExpectedHash

$simdeRoot = Join-Path $ThirdParty "simde-$SimdeCommit"
$simdeZip = Join-Path $ThirdParty "simde-$SimdeCommit.zip"
$null = Invoke-DownloadZip `
    -Uri "https://github.com/simd-everywhere/simde/archive/$SimdeCommit.zip" `
    -ZipPath $simdeZip `
    -Destination $ThirdParty `
    -ExpectedDirectory $simdeRoot `
    -ArchiveRootName "simde-$SimdeCommit" `
    -ExpectedSha256 $SimdeArchiveSha256

$kissRoot = Join-Path $ThirdParty "kissfft-131.1.0"
$kissZip = Join-Path $ThirdParty "kissfft-131.1.0.zip"
$null = Invoke-DownloadZip `
    -Uri "https://github.com/mborgerding/kissfft/archive/refs/tags/131.1.0.zip" `
    -ZipPath $kissZip `
    -Destination $ThirdParty `
    -ExpectedDirectory $kissRoot `
    -ArchiveRootName "kissfft-131.1.0" `
    -ExpectedSha256 $KissArchiveSha256

$modelPath = Join-Path $Models "$ModelName.onnx"
if (!(Test-Path $modelPath)) {
    & (Join-Path $PSScriptRoot "update-windows.ps1") `
        -OnnxRuntimeVersion $OnnxRuntimeVersion `
        -DefaultModelName $ModelName `
        -ModelNames @($ModelName)
}

Set-Content -Encoding ASCII -Path (Join-Path $GeneratedObs "obsconfig.h") -Value @"
#pragma once
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
"@

Set-Content -Encoding ASCII -Path (Join-Path $GeneratedObs "plugin-version.h") -Value @"
#pragma once
#define PLUGIN_NAME "obs-dpdfnet"
#define PLUGIN_VERSION "$PluginVersion"
"@

Import-VcVars64

$obsDll = Join-Path $ObsInstallDir "bin\64bit\obs.dll"
if (!(Test-Path $obsDll)) {
    throw "Could not find installed OBS DLL at $obsDll"
}
$ObsRuntimeProductVersion = (Get-Item -LiteralPath $obsDll).VersionInfo.ProductVersion.Trim()
if ($ObsRuntimeProductVersion -cne $ObsVersion) {
    throw "Installed OBS runtime is version '$ObsRuntimeProductVersion', but this build requests OBS $ObsVersion."
}
$ObsRuntimeSha256 = Get-Sha256 -Path $obsDll

$obsLib = Join-Path $BuildDir "obs.lib"
New-ImportLibrary `
    -DllPath $obsDll `
    -LibraryName "obs.dll" `
    -OutLib $obsLib `
    -DefPath (Join-Path $BuildDir "obs.def")

$onnxDll = Join-Path $onnxRoot "lib\onnxruntime.dll"
$onnxRenamedDll = "onnxruntime_dpdfnet.dll"
$onnxRenamedLib = Join-Path $BuildDir "onnxruntime_dpdfnet.lib"
New-ImportLibrary `
    -DllPath $onnxDll `
    -LibraryName $onnxRenamedDll `
    -OutLib $onnxRenamedLib `
    -DefPath (Join-Path $BuildDir "onnxruntime_dpdfnet.def")

$pluginDll = Join-Path $OutputDir "obs-dpdfnet.dll"
$pluginLib = Join-Path $OutputDir "obs-dpdfnet.lib"
$sources = @(
    (Join-Path $Root "src\dpdfnet-filter.cpp"),
    (Join-Path $Root "src\dpdfnet-model.cpp"),
    (Join-Path $Root "src\dpdfnet-processor.cpp"),
    (Join-Path $Root "src\dpdfnet-settings.cpp"),
    (Join-Path $Root "src\plugin-main.cpp"),
    (Join-Path $Root "src\stft.cpp"),
    (Join-Path $kissRoot "kiss_fft.c"),
    (Join-Path $kissRoot "kiss_fftr.c")
)

$includeArgs = @(
    "/I$GeneratedObs",
    "/I$(Join-Path $obsSourceRoot 'libobs')",
    "/I$(Join-Path $onnxRoot 'include')",
    "/I$simdeRoot",
    "/I$kissRoot"
)

$defines = @(
    "/Dkiss_fft_scalar=float",
    "/DNOMINMAX",
    "/DWIN32",
    "/D_WINDOWS"
)

$compileArgs = @(
    "/nologo",
    "/std:c++17",
    "/EHsc",
    "/W4",
    "/permissive-",
    "/FI$(Join-Path $GeneratedObs 'plugin-version.h')",
    "/MD",
    "/O2",
    "/Zi",
    "/Fd:$OutputDir\obs-dpdfnet.pdb",
    "/LD",
    "/Fe:$pluginDll",
    "/Fo:$OutputDir\"
) + $defines + $includeArgs + $sources + @(
    "/link",
    "/NOLOGO",
    "/DEBUG:FULL",
    "/MAP:$OutputDir\obs-dpdfnet.map",
    "/IMPLIB:$pluginLib",
    $obsLib,
    $onnxRenamedLib
)

& cl.exe @compileArgs
if ($LASTEXITCODE -ne 0) {
    throw "cl.exe failed"
}

function Invoke-NativeExecutableBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Sources,
        [Parameter(Mandatory = $true)][string[]]$Libraries,
        [string[]]$AdditionalCompileArguments = @()
    )

    $Executable = Join-Path $OutputDir "$Name.exe"
    $ObjectDir = Join-Path $BuildDir "obj-$Name"
    New-Item -ItemType Directory -Force -Path $ObjectDir | Out-Null
    Get-ChildItem $ObjectDir -File -ErrorAction SilentlyContinue | Remove-Item -Force

    $Arguments = @(
        "/nologo",
        "/std:c++17",
        "/EHsc",
        "/W4",
        "/permissive-",
        "/MD",
        "/O2",
        "/Zi",
        "/Fd:$ObjectDir\$Name.pdb",
        "/Fe:$Executable",
        "/Fo:$ObjectDir\"
    ) + $defines + $includeArgs + $AdditionalCompileArguments + $Sources + @(
        "/link",
        "/NOLOGO",
        "/DEBUG:FULL",
        "/PDB:$OutputDir\$Name.pdb"
    ) + $Libraries

    & cl.exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe failed while building $Name"
    }
}

$ModelSources = @(
    (Join-Path $Root "src\dpdfnet-model.cpp")
)
$DspSources = @(
    (Join-Path $Root "src\dpdfnet-model.cpp"),
    (Join-Path $Root "src\dpdfnet-processor.cpp"),
    (Join-Path $Root "src\stft.cpp"),
    (Join-Path $kissRoot "kiss_fft.c"),
    (Join-Path $kissRoot "kiss_fftr.c")
)

Invoke-NativeExecutableBuild `
    -Name "dpdfnet-model-smoke" `
    -Sources (@((Join-Path $Root "tools\model-smoke.cpp")) + $ModelSources + @(
        (Join-Path $Root "src\stft.cpp"),
        (Join-Path $kissRoot "kiss_fft.c"),
        (Join-Path $kissRoot "kiss_fftr.c")
    )) `
    -Libraries @($onnxRenamedLib)

Invoke-NativeExecutableBuild `
    -Name "dpdfnet-model-contract-test" `
    -Sources (@((Join-Path $Root "tests\model-contract-test.cpp")) + $ModelSources) `
    -Libraries @($onnxRenamedLib)

Invoke-NativeExecutableBuild `
    -Name "dpdfnet-onnxruntime-version" `
    -Sources @((Join-Path $Root "tools\onnxruntime-version.cpp")) `
    -Libraries @($onnxRenamedLib)

Invoke-NativeExecutableBuild `
    -Name "obs-dpdfnet-tests" `
    -Sources (@(
        (Join-Path $Root "tests\dpdfnet-tests.cpp"),
        (Join-Path $Root "src\dpdfnet-settings.cpp")
    ) + $DspSources) `
    -Libraries @($obsLib, $onnxRenamedLib)

Invoke-NativeExecutableBuild `
    -Name "obs-dpdfnet-filter-tests" `
    -Sources (@(
        (Join-Path $Root "tests\dpdfnet-filter-tests.cpp"),
        (Join-Path $Root "src\dpdfnet-filter.cpp"),
        (Join-Path $Root "src\dpdfnet-settings.cpp")
    ) + $DspSources) `
    -Libraries @($obsLib, $onnxRenamedLib) `
    -AdditionalCompileArguments @(
        "/FI$(Join-Path $GeneratedObs 'plugin-version.h')"
    )

Invoke-NativeExecutableBuild `
    -Name "dpdfnet-stream-dump" `
    -Sources (@((Join-Path $Root "tools\stream-dump.cpp")) + $DspSources) `
    -Libraries @($obsLib, $onnxRenamedLib)

Invoke-NativeExecutableBuild `
    -Name "dpdfnet-quality-benchmark" `
    -Sources (@((Join-Path $Root "tools\quality-benchmark.cpp")) + $DspSources) `
    -Libraries @($obsLib, $onnxRenamedLib)

Invoke-NativeExecutableBuild `
    -Name "dpdfnet-processor-benchmark" `
    -Sources (@((Join-Path $Root "tools\processor-benchmark.cpp")) + $DspSources) `
    -Libraries @($obsLib, $onnxRenamedLib)

Remove-Item (Join-Path $OutputDir "onnxruntime.dll") -Force -ErrorAction SilentlyContinue
Copy-Item $onnxDll -Destination (Join-Path $OutputDir $onnxRenamedDll) -Force
Copy-Item (Join-Path $onnxRoot "lib\onnxruntime_providers_shared.dll") -Destination $OutputDir -Force

$OnnxRuntimeVersionExecutable = Join-Path $OutputDir "dpdfnet-onnxruntime-version.exe"
$OnnxRuntimeVersionOutput = & $OnnxRuntimeVersionExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Could not query the version reported by onnxruntime_dpdfnet.dll."
}
$OnnxRuntimeReportedVersion = ($OnnxRuntimeVersionOutput -join "").Trim()
if ($OnnxRuntimeReportedVersion -cne $OnnxRuntimeVersion) {
    throw "onnxruntime_dpdfnet.dll reports version '$OnnxRuntimeReportedVersion', but this build requests $OnnxRuntimeVersion."
}

# Bind release metadata to the exact outputs produced by this invocation.
# source-commit.txt remains for older local reporting scripts, but release and
# test gates require the complete, hash-bound JSON manifest below.
$Git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($Git) {
    # Capture before selecting: an inline `| Select-Object -First 1` stops the
    # pipeline early and clobbers git's exit code with -1.
    $GitOutput = & $Git.Source -C $Root rev-parse HEAD
    if ($LASTEXITCODE -eq 0 -and $GitOutput) {
        $GitSha = @($GitOutput)[0].Trim().ToLowerInvariant()
        $GitStatus = & $Git.Source -C $Root status --porcelain
        if ($LASTEXITCODE -eq 0) {
            $GitDirty = [bool]$GitStatus
            $ArtifactNames = @(
                "obs-dpdfnet.dll",
                "dpdfnet-model-smoke.exe",
                "dpdfnet-model-contract-test.exe",
                "dpdfnet-onnxruntime-version.exe",
                "obs-dpdfnet-tests.exe",
                "obs-dpdfnet-filter-tests.exe",
                "dpdfnet-stream-dump.exe",
                "dpdfnet-quality-benchmark.exe",
                "dpdfnet-processor-benchmark.exe",
                $onnxRenamedDll,
                "onnxruntime_providers_shared.dll"
            )
            $Artifacts = foreach ($ArtifactName in $ArtifactNames) {
                $ArtifactPath = Join-Path $OutputDir $ArtifactName
                if (!(Test-Path $ArtifactPath -PathType Leaf)) {
                    throw "Expected build artifact is missing: $ArtifactPath"
                }
                [ordered]@{
                    path = $ArtifactName
                    sha256 = Get-Sha256 -Path $ArtifactPath
                }
            }

            $BuildProvenance = [ordered]@{
                schemaVersion = 2
                sourceCommit = $GitSha
                sourceDirty = $GitDirty
                pluginVersion = $PluginVersion
                obsVersion = $ObsVersion
                obsSourceArchiveSha256 = $ObsSourceArchiveSha256
                obsRuntimeProductVersion = $ObsRuntimeProductVersion
                obsRuntimeSha256 = $ObsRuntimeSha256
                onnxRuntimeVersion = $OnnxRuntimeVersion
                onnxRuntimeReportedVersion = $OnnxRuntimeReportedVersion
                onnxRuntimeArchiveSha256 = $OnnxRuntimeArchiveSha256
                configuration = $Configuration
                architecture = "x64"
                artifacts = @($Artifacts)
            }
            $BuildProvenance |
                ConvertTo-Json -Depth 4 |
                Set-Content -Encoding ASCII -Path $BuildProvenanceFile

            $LegacyCommit = if ($GitDirty) { "$GitSha-dirty" } else { $GitSha }
            Set-Content -Encoding ASCII -Path $SourceCommitFile -Value $LegacyCommit
        }
    }
}
if (!(Test-Path $BuildProvenanceFile)) {
    Write-Warning "Could not record complete build provenance; testing and release staging will reject this build."
}

Write-Host "Built $pluginDll"
Write-Host "Built Windows test and quality executables in $OutputDir"
