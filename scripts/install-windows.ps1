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

New-Item -ItemType Directory -Force -Path $BinDir, $DataDir | Out-Null

$Dll = Get-ChildItem -Path $BuildPath -Recurse -Filter "obs-dpdfnet.dll" |
    Where-Object { $_.FullName -match "\\$Configuration\\" -or $_.DirectoryName -match "\\$Configuration$" } |
    Select-Object -First 1

if (!$Dll) {
    throw "Could not find obs-dpdfnet.dll under $BuildPath. Build the Release configuration first."
}

Copy-Item $Dll.FullName -Destination $BinDir -Force

function Get-BuildArtifact {
    param([Parameter(Mandatory = $true)][string]$Filter)

    Get-ChildItem -Path $BuildPath -Recurse -Filter $Filter |
        Where-Object { $_.FullName -match "\\$Configuration\\" -or $_.DirectoryName -match "\\$Configuration$" } |
        Select-Object -First 1
}

$RenamedOrtDll = Get-BuildArtifact -Filter "onnxruntime_dpdfnet.dll"
$OrtDll = Get-BuildArtifact -Filter "onnxruntime.dll"
$ThirdPartyOrtDll = Join-Path $Root "third_party\onnxruntime\lib\onnxruntime.dll"

Remove-Item (Join-Path $BinDir "onnxruntime.dll") -Force -ErrorAction SilentlyContinue

if ($RenamedOrtDll) {
    Copy-Item $RenamedOrtDll.FullName -Destination $BinDir -Force
} elseif ($OrtDll) {
    Copy-Item $OrtDll.FullName -Destination $BinDir -Force
} elseif (Test-Path $ThirdPartyOrtDll) {
    Copy-Item $ThirdPartyOrtDll -Destination $BinDir -Force
} else {
    Write-Warning "ONNX Runtime DLL was not found under $BuildPath or third_party\onnxruntime. The plugin will not load without it."
}

$OrtProvidersDll = Get-BuildArtifact -Filter "onnxruntime_providers_shared.dll"
$ThirdPartyOrtProvidersDll = Join-Path $Root "third_party\onnxruntime\lib\onnxruntime_providers_shared.dll"
if ($OrtProvidersDll) {
    Copy-Item $OrtProvidersDll.FullName -Destination $BinDir -Force
} elseif (Test-Path $ThirdPartyOrtProvidersDll) {
    Copy-Item $ThirdPartyOrtProvidersDll -Destination $BinDir -Force
}

Copy-Item (Join-Path $Root "data\*") -Destination $DataDir -Recurse -Force
Copy-Item (Join-Path $Root "LICENSE") -Destination $DataDir -Force
Copy-Item (Join-Path $Root "THIRD_PARTY.md") -Destination $DataDir -Force
Copy-Item (Join-Path $Root "LICENSES") -Destination $DataDir -Recurse -Force

$OrtNotices = Join-Path $Root "third_party\onnxruntime\ThirdPartyNotices.txt"
if (Test-Path $OrtNotices) {
    Copy-Item $OrtNotices -Destination $DataDir -Force
}

$ModelDir = Join-Path $DataDir "models"
New-Item -ItemType Directory -Force -Path $ModelDir | Out-Null
Copy-Item (Join-Path $Root "models\*.onnx") -Destination $ModelDir -Force

$ModelManifest = Join-Path $Root "models\manifest.json"
if (Test-Path $ModelManifest) {
    Copy-Item $ModelManifest -Destination $ModelDir -Force
}

Write-Host "Installed obs-dpdfnet to $PluginRoot"
Write-Host "Restart OBS and add DPDFNet Noise Suppression as an audio filter."
