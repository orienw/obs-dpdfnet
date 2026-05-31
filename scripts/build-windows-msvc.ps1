# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$ObsVersion = "32.1.2",
    [string]$OnnxRuntimeVersion = "1.26.0",
    [string]$ModelName = "dpdfnet8_48khz_hr",
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$Configuration = "Release",
    [string]$PluginVersion = "0.3.0"
)

$ErrorActionPreference = "Stop"

$KnownOnnxRuntimeHashes = @{
    "1.26.0" = "6ebe99b5564bf4d029b6e93eac9ff423682b6212eade769e9ca3f685eaf500b4"
}

$KnownObsArchiveHashes = @{
    "32.1.2" = "21cba22292985cf0da967d5c618999b40eaa32b73d2ab8b06154b5ea1b3d3798"
}

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

function Invoke-DownloadZip {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$ZipPath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ExpectedDirectory,
        [string]$ExpectedSha256 = ""
    )

    if (Test-Path $ExpectedDirectory) {
        return
    }

    New-Item -ItemType Directory -Force -Path (Split-Path $ZipPath), $Destination | Out-Null

    if ((Test-Path $ZipPath) -and $ExpectedSha256) {
        $currentHash = Get-Sha256 -Path $ZipPath
        if ($currentHash -ne $ExpectedSha256) {
            Write-Host "Discarding cached archive with mismatched hash: $ZipPath"
            Remove-Item -Force $ZipPath
        }
    }

    if (!(Test-Path $ZipPath)) {
        Write-Host "Downloading $Uri"
        Invoke-WebRequest -Uri $Uri -OutFile $ZipPath
    }

    if ($ExpectedSha256) {
        $downloadHash = Get-Sha256 -Path $ZipPath
        if ($downloadHash -ne $ExpectedSha256) {
            Remove-Item -Force $ZipPath -ErrorAction SilentlyContinue
            throw "Hash mismatch for $ZipPath. Expected $ExpectedSha256, got $downloadHash"
        }
    }

    Expand-Archive -Path $ZipPath -DestinationPath $Destination -Force
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

$onnxBase = "onnxruntime-win-x64-$OnnxRuntimeVersion"
$onnxRoot = Join-Path $ThirdParty "onnxruntime"
$onnxZip = Join-Path $ThirdParty "$onnxBase.zip"
$onnxExpectedHash = $KnownOnnxRuntimeHashes[$OnnxRuntimeVersion]
if (!$onnxExpectedHash) {
    Write-Warning "No pinned ONNX Runtime hash is known for version $OnnxRuntimeVersion."
}
$onnxVersionFile = Join-Path $onnxRoot "VERSION_NUMBER"
if ((Test-Path $onnxRoot) -and
    (!(Test-Path $onnxVersionFile) -or
     ((Get-Content -Raw $onnxVersionFile).Trim() -ne $OnnxRuntimeVersion))) {
    Write-Host "Replacing ONNX Runtime with $OnnxRuntimeVersion"
    Remove-Item -Recurse -Force $onnxRoot
}
Invoke-DownloadZip `
    -Uri "https://github.com/microsoft/onnxruntime/releases/download/v$OnnxRuntimeVersion/$onnxBase.zip" `
    -ZipPath $onnxZip `
    -Destination $ThirdParty `
    -ExpectedDirectory $onnxRoot `
    -ExpectedSha256 $onnxExpectedHash
if (!(Test-Path $onnxRoot)) {
    Rename-Item -Path (Join-Path $ThirdParty $onnxBase) -NewName "onnxruntime"
}

$obsSourceRoot = Join-Path $ThirdParty "obs-studio-$ObsVersion"
$obsZip = Join-Path $ThirdParty "obs-studio-$ObsVersion.zip"
$obsExpectedHash = $KnownObsArchiveHashes[$ObsVersion]
if (!$obsExpectedHash) {
    Write-Warning "No pinned OBS Studio source hash is known for version $ObsVersion."
}
Invoke-DownloadZip `
    -Uri "https://github.com/obsproject/obs-studio/archive/refs/tags/$ObsVersion.zip" `
    -ZipPath $obsZip `
    -Destination $ThirdParty `
    -ExpectedDirectory $obsSourceRoot `
    -ExpectedSha256 $obsExpectedHash

$simdeRoot = Join-Path $ThirdParty "simde-$SimdeCommit"
$simdeZip = Join-Path $ThirdParty "simde-$SimdeCommit.zip"
Invoke-DownloadZip `
    -Uri "https://github.com/simd-everywhere/simde/archive/$SimdeCommit.zip" `
    -ZipPath $simdeZip `
    -Destination $ThirdParty `
    -ExpectedDirectory $simdeRoot `
    -ExpectedSha256 $SimdeArchiveSha256

$kissRoot = Join-Path $ThirdParty "kissfft-131.1.0"
$kissZip = Join-Path $ThirdParty "kissfft-131.1.0.zip"
Invoke-DownloadZip `
    -Uri "https://github.com/mborgerding/kissfft/archive/refs/tags/131.1.0.zip" `
    -ZipPath $kissZip `
    -Destination $ThirdParty `
    -ExpectedDirectory $kissRoot `
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

Remove-Item (Join-Path $OutputDir "onnxruntime.dll") -Force -ErrorAction SilentlyContinue
Copy-Item $onnxDll -Destination (Join-Path $OutputDir $onnxRenamedDll) -Force
Copy-Item (Join-Path $onnxRoot "lib\onnxruntime_providers_shared.dll") -Destination $OutputDir -Force

Write-Host "Built $pluginDll"
