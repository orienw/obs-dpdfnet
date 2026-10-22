# SPDX-License-Identifier: GPL-2.0-or-later

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot
$TestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("dpdfnet-install-" + [guid]::NewGuid().ToString("N"))
$SourceRoot = Join-Path $TestRoot "source"
$BuildDir = Join-Path $SourceRoot "build"
$PluginRoot = Join-Path $TestRoot "installed"
$Installer = Join-Path $SourceRoot "scripts\install-windows.ps1"

function Write-TestFile {
    param([string]$Path, [string]$Content)
    New-Item -ItemType Directory -Force -Path (Split-Path $Path) | Out-Null
    Set-Content -LiteralPath $Path -Value $Content -Encoding ASCII
}

function Get-InstallSnapshot {
    @(Get-ChildItem -LiteralPath $PluginRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
        "$($_.FullName.Substring($PluginRoot.Length))|$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
    }) -join "`n"
}

try {
    Write-TestFile $Installer ""
    Copy-Item -LiteralPath (Join-Path $RepoRoot "scripts\install-windows.ps1") -Destination $Installer -Force
    $SourceFiles = @(
        "build\Release\obs-dpdfnet.dll",
        "build\Release\onnxruntime_dpdfnet.dll",
        "build\Release\onnxruntime_providers_shared.dll",
        "data\locale\en-US.ini",
        "LICENSE",
        "THIRD_PARTY.md",
        "LICENSES\MIT-ONNXRuntime.txt",
        "third_party\onnxruntime\LICENSE",
        "third_party\onnxruntime\ThirdPartyNotices.txt",
        "models\dpdfnet8_48khz_hr.onnx",
        "models\dpdfnet2_48khz_hr.onnx",
        "models\manifest.json"
    )
    foreach ($RelativePath in $SourceFiles) {
        Write-TestFile (Join-Path $SourceRoot $RelativePath) "new $RelativePath"
    }
    foreach ($RelativePath in @(
        "bin\64bit\obs-dpdfnet.dll",
        "bin\64bit\onnxruntime.dll",
        "data\locale\en-US.ini",
        "data\models\custom\personal.onnx"
    )) {
        Write-TestFile (Join-Path $PluginRoot $RelativePath) "old $RelativePath"
    }
    $Before = Get-InstallSnapshot
    $MissingAssets = @(
        @{ Path = "build\Release\obs-dpdfnet.dll"; Error = "Could not find obs-dpdfnet.dll" },
        @{ Path = "build\Release\onnxruntime_dpdfnet.dll"; Error = "ONNX Runtime was not found" },
        @{ Path = "third_party\onnxruntime\LICENSE"; Error = "ONNX Runtime's LICENSE is missing" },
        @{ Path = "third_party\onnxruntime\ThirdPartyNotices.txt"; Error = "ONNX Runtime's ThirdPartyNotices.txt is missing" },
        @{ Path = "data\locale\en-US.ini"; Error = "Required install file is missing" },
        @{ Path = "LICENSE"; Error = "Required install file is missing" },
        @{ Path = "THIRD_PARTY.md"; Error = "Required install file is missing" },
        @{ Path = "LICENSES"; Error = "Required install directory is missing" },
        @{ Path = "models\dpdfnet8_48khz_hr.onnx"; Error = "Required install file is missing" },
        @{ Path = "models\dpdfnet2_48khz_hr.onnx"; Error = "Required install file is missing" }
    )
    foreach ($Case in $MissingAssets) {
        $Path = Join-Path $SourceRoot $Case.Path
        Move-Item -LiteralPath $Path -Destination "$Path.missing"
        try {
            $Failure = $null
            try {
                & $Installer -BuildDir $BuildDir -PluginRoot $PluginRoot
            } catch {
                $Failure = $_.Exception.Message
            }
            if (!$Failure -or !$Failure.Contains($Case.Error)) {
                throw "Missing $($Case.Path) did not produce the expected error: $Failure"
            }
            if ((Get-InstallSnapshot) -cne $Before) {
                throw "Missing $($Case.Path) changed the existing installation."
            }
            Write-Host "PASS missing $($Case.Path) preserves the installation"
        } finally {
            Move-Item -LiteralPath "$Path.missing" -Destination $Path
        }
    }

    & $Installer -BuildDir $BuildDir -PluginRoot $PluginRoot
    foreach ($RelativePath in @(
        "bin\64bit\obs-dpdfnet.dll",
        "bin\64bit\onnxruntime_dpdfnet.dll",
        "bin\64bit\onnxruntime_providers_shared.dll",
        "data\locale\en-US.ini",
        "data\ONNXRuntime-LICENSE.txt",
        "data\ThirdPartyNotices.txt",
        "data\models\dpdfnet8_48khz_hr.onnx",
        "data\models\dpdfnet2_48khz_hr.onnx",
        "data\models\manifest.json"
    )) {
        if (!(Test-Path -LiteralPath (Join-Path $PluginRoot $RelativePath) -PathType Leaf)) {
            throw "Successful install omitted $RelativePath."
        }
    }
    if (Test-Path -LiteralPath (Join-Path $PluginRoot "bin\64bit\onnxruntime.dll")) {
        throw "Successful install left the obsolete runtime."
    }
    $CustomModel = Get-Content -Raw -LiteralPath (Join-Path $PluginRoot "data\models\custom\personal.onnx")
    if ($CustomModel.Trim() -cne "old data\models\custom\personal.onnx") {
        throw "Successful install changed the custom model."
    }
    Write-Host "PASS complete install replaces the runtime and preserves custom models"

    Move-Item -LiteralPath (Join-Path $SourceRoot "build\Release\onnxruntime_dpdfnet.dll") `
        -Destination (Join-Path $SourceRoot "build\Release\onnxruntime.dll")
    & $Installer -BuildDir $BuildDir -PluginRoot $PluginRoot
    if (!(Test-Path -LiteralPath (Join-Path $PluginRoot "bin\64bit\onnxruntime.dll")) -or
        (Test-Path -LiteralPath (Join-Path $PluginRoot "bin\64bit\onnxruntime_dpdfnet.dll"))) {
        throw "CMake install did not replace the private runtime with the stock runtime."
    }
    Write-Host "PASS CMake install selects the stock runtime"
} finally {
    if (Test-Path -LiteralPath $TestRoot) {
        Remove-Item -LiteralPath $TestRoot -Recurse -Force
    }
}
