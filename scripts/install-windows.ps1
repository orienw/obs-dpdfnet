# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [Parameter(Mandatory = $false)]
    [string]$BuildDir = ".\build",

    [Parameter(Mandatory = $false)]
    [string]$Configuration = "Release",

    [Parameter(Mandatory = $false)]
    [string]$PluginRoot = "$env:ProgramData\obs-studio\plugins\obs-dpdfnet"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Resolve-Path $BuildDir
$BinDir = Join-Path $PluginRoot "bin\64bit"
$DataDir = Join-Path $PluginRoot "data"

# The MSVC script and CMake's Visual Studio generator put the plugin in
# <BuildDir>\<Configuration>; single-config CMake generators put it in
# <BuildDir>. Anything else must be unambiguous.
$Candidates = @(
    (Join-Path $BuildPath "$Configuration\obs-dpdfnet.dll"),
    (Join-Path $BuildPath "obs-dpdfnet.dll")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
if ($Candidates.Count -eq 0) {
    $Candidates = @(Get-ChildItem -Path $BuildPath -Recurse -Filter "obs-dpdfnet.dll" |
        Where-Object { $_.DirectoryName -match "\\$Configuration$" } |
        ForEach-Object { $_.FullName })
    if ($Candidates.Count -gt 1) {
        throw "Found several $Configuration builds of obs-dpdfnet.dll under $BuildPath. Pass -BuildDir for the one to install:`n$($Candidates -join "`n")"
    }
}
if ($Candidates.Count -eq 0) {
    throw "Could not find obs-dpdfnet.dll under $BuildPath. Build the $Configuration configuration first."
}
$Dll = Get-Item -LiteralPath @($Candidates)[0]
$PluginBuildDir = $Dll.DirectoryName

# ONNX Runtime comes from the plugin's own build directory, under the name
# that build links: the MSVC script renames it onnxruntime_dpdfnet.dll, a
# CMake build keeps onnxruntime.dll. The other name is removed, so an
# earlier install cannot leave a mismatched copy behind.
$RenamedOrtDll = Join-Path $PluginBuildDir "onnxruntime_dpdfnet.dll"
$OrtDll = Join-Path $PluginBuildDir "onnxruntime.dll"
if (Test-Path -LiteralPath $RenamedOrtDll -PathType Leaf) {
    $SelectedOrtDll = $RenamedOrtDll
    $ObsoleteOrtName = "onnxruntime.dll"
} elseif (Test-Path -LiteralPath $OrtDll -PathType Leaf) {
    $SelectedOrtDll = $OrtDll
    $ObsoleteOrtName = "onnxruntime_dpdfnet.dll"
} else {
    throw "ONNX Runtime was not found next to $($Dll.FullName). Rebuild; the plugin does not load without it."
}

# ONNX Runtime's license and third-party notices go wherever its DLLs go.
$OrtRoot = Join-Path $Root "third_party\onnxruntime"
$Notices = @(
    @{ Source = "LICENSE"; Name = "ONNXRuntime-LICENSE.txt" },
    @{ Source = "ThirdPartyNotices.txt"; Name = "ThirdPartyNotices.txt" }
)
foreach ($Notice in $Notices) {
    $NoticePath = Join-Path $OrtRoot $Notice.Source
    if (!(Test-Path -LiteralPath $NoticePath -PathType Leaf)) {
        throw "ONNX Runtime's $($Notice.Source) is missing from $OrtRoot. Run scripts\bootstrap-windows.ps1."
    }
}

# Check every required source before changing an existing installation.
foreach ($RelativePath in @(
    "LICENSE", "THIRD_PARTY.md", "data\locale\en-US.ini",
    "models\dpdfnet8_48khz_hr.onnx", "models\dpdfnet2_48khz_hr.onnx"
)) {
    if (!(Test-Path -LiteralPath (Join-Path $Root $RelativePath) -PathType Leaf)) {
        throw "Required install file is missing: $RelativePath"
    }
}
if (!(Test-Path -LiteralPath (Join-Path $Root "LICENSES") -PathType Container)) {
    throw "Required install directory is missing: LICENSES"
}
$Models = @(Get-ChildItem -LiteralPath (Join-Path $Root "models") -Filter "*.onnx" -File)

New-Item -ItemType Directory -Force -Path $BinDir, $DataDir | Out-Null
Copy-Item $Dll.FullName -Destination $BinDir -Force
Copy-Item -LiteralPath $SelectedOrtDll -Destination $BinDir -Force
Remove-Item (Join-Path $BinDir $ObsoleteOrtName) -Force -ErrorAction SilentlyContinue

$OrtProvidersDll = Join-Path $PluginBuildDir "onnxruntime_providers_shared.dll"
if (Test-Path -LiteralPath $OrtProvidersDll -PathType Leaf) {
    Copy-Item $OrtProvidersDll -Destination $BinDir -Force
}

Copy-Item (Join-Path $Root "data\*") -Destination $DataDir -Recurse -Force
Copy-Item (Join-Path $Root "LICENSE") -Destination $DataDir -Force
Copy-Item (Join-Path $Root "THIRD_PARTY.md") -Destination $DataDir -Force
Copy-Item (Join-Path $Root "LICENSES") -Destination $DataDir -Recurse -Force
foreach ($Notice in $Notices) {
    Copy-Item -LiteralPath (Join-Path $OrtRoot $Notice.Source) `
        -Destination (Join-Path $DataDir $Notice.Name) -Force
}

$ModelDir = Join-Path $DataDir "models"
New-Item -ItemType Directory -Force -Path $ModelDir | Out-Null
$Models | Copy-Item -Destination $ModelDir -Force

$ModelManifest = Join-Path $Root "models\manifest.json"
if (Test-Path $ModelManifest) {
    Copy-Item $ModelManifest -Destination $ModelDir -Force
}

Write-Host "Installed obs-dpdfnet to $PluginRoot"
Write-Host "Restart OBS and add DPDFNet Noise Suppression as an audio filter."
