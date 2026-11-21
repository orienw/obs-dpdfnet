# SPDX-License-Identifier: GPL-2.0-or-later

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot
$TestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("dpdfnet-update-" + [guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Path (Join-Path $TestRoot "scripts"),
        (Join-Path $TestRoot "models"),
        (Join-Path $TestRoot "third_party\onnxruntime") | Out-Null
    foreach ($Name in @("update-windows.ps1", "dependency-versions.ps1")) {
        Copy-Item -LiteralPath (Join-Path $RepoRoot "scripts\$Name") -Destination (Join-Path $TestRoot "scripts\$Name")
    }
    Copy-Item -LiteralPath (Join-Path $RepoRoot "VERSION") -Destination $TestRoot
    . (Join-Path $TestRoot "scripts\dependency-versions.ps1")
    Set-Content -LiteralPath (Join-Path $TestRoot "third_party\onnxruntime\VERSION_NUMBER") $DpdfnetDefaultOnnxRuntimeVersion
    $ModelRecords = @(foreach ($Name in $DpdfnetDefaultModelNames) {
        $Path = Join-Path $TestRoot "models\$Name.onnx"
        Set-Content -LiteralPath $Path "model $Name"
        [PSCustomObject]@{
            rfilename = "onnx/$Name.onnx"
            lfs = @{ sha256 = (Get-FileHash -LiteralPath $Path).Hash }
            size = (Get-Item -LiteralPath $Path).Length
        }
    })

    function Invoke-RestMethod {
        param([string]$Uri)
        if ($Uri -eq "https://api.github.com/repos/microsoft/onnxruntime/releases/tags/v$DpdfnetDefaultOnnxRuntimeVersion") {
            return [PSCustomObject]@{
                tag_name = "v$DpdfnetDefaultOnnxRuntimeVersion"
                assets = @([PSCustomObject]@{ name = "onnxruntime-win-x64-$DpdfnetDefaultOnnxRuntimeVersion.zip" })
            }
        }
        if ($Uri -eq "https://api.github.com/repos/microsoft/onnxruntime/releases/latest") {
            return [PSCustomObject]@{
                tag_name = "v$UnpinnedVersion"
                assets = @([PSCustomObject]@{ name = "$FixtureBase.zip"; digest = "sha256:$FixtureHash"; browser_download_url = "https://github.com/microsoft/onnxruntime/releases/download/v$UnpinnedVersion/$FixtureBase.zip" })
            }
        }
        if ($Uri -eq "https://huggingface.co/api/models/Ceva-IP/DPDFNet?blobs=true") {
            return [PSCustomObject]@{ sha = "test-revision"; siblings = $ModelRecords }
        }
        throw "Unexpected download request: $Uri"
    }

    # Replace the compiler and installer with their directory contracts.
    Set-Content -LiteralPath (Join-Path $TestRoot "scripts\build-windows-msvc.ps1") @'
param([string]$ObsVersion, [string]$OnnxRuntimeVersion, [string]$OnnxRuntimeSha256, [string]$ModelName, [string]$BuildDir = "")
if (!$BuildDir) { $BuildDir = Join-Path (Split-Path $PSScriptRoot) "build\msvc" }
$Output = Join-Path $BuildDir "Release"
New-Item -ItemType Directory -Force -Path $Output | Out-Null
Set-Content -LiteralPath (Join-Path $Output "obs-dpdfnet.dll") "new build"
'@
    Set-Content -LiteralPath (Join-Path $TestRoot "scripts\install-windows.ps1") @'
param([string]$BuildDir)
$Built = Get-Content -Raw -LiteralPath (Join-Path $BuildDir "Release\obs-dpdfnet.dll")
Set-Content -LiteralPath (Join-Path (Split-Path $PSScriptRoot) "installed.txt") $Built.Trim()
'@

    foreach ($Custom in @($false, $true)) {
        $BuildDir = Join-Path $TestRoot $(if ($Custom) { "custom build" } else { "build\msvc" })
        New-Item -ItemType Directory -Force -Path (Join-Path $BuildDir "Release") | Out-Null
        Set-Content -LiteralPath (Join-Path $BuildDir "Release\obs-dpdfnet.dll") "old build"
        $Arguments = @{ Build = $true; Install = $true }
        if ($Custom) { $Arguments.BuildDir = $BuildDir }
        & (Join-Path $TestRoot "scripts\update-windows.ps1") @Arguments
        $Installed = (Get-Content -Raw -LiteralPath (Join-Path $TestRoot "installed.txt")).Trim()
        if ($Installed -cne "new build") {
            throw "Build and install used different directories: $BuildDir"
        }
        Write-Host "PASS build and install share the $(if ($Custom) { 'custom' } else { 'default' }) directory"
    }

    # Run the real updater and builder through runtime extraction. Stop when the
    # builder moves on to OBS, before it needs the compiler or an OBS install.
    Copy-Item (Join-Path $RepoRoot "scripts\build-windows-msvc.ps1") (Join-Path $TestRoot "scripts") -Force
    $UnpinnedVersion = "0.0.123"
    $FixtureBase = "onnxruntime-win-x64-$UnpinnedVersion"
    $FixtureDir = New-Item -ItemType Directory -Path (Join-Path $TestRoot $FixtureBase)
    Set-Content (Join-Path $FixtureDir "VERSION_NUMBER") $UnpinnedVersion
    $FixtureZip = Join-Path $TestRoot "$FixtureBase.zip"
    Compress-Archive -LiteralPath $FixtureDir.FullName -DestinationPath $FixtureZip
    $FixtureHash = (Get-FileHash -LiteralPath $FixtureZip).Hash
    $Requests = [System.Collections.Generic.List[string]]::new()
    function Invoke-WebRequest {
        param([string]$Uri, [string]$OutFile)
        $Requests.Add($Uri)
        if ($Uri -eq "https://github.com/obsproject/obs-studio/archive/refs/tags/$DpdfnetMinimumObsVersion.zip") {
            throw "Reached OBS download"
        }
        if ($Uri -eq "https://github.com/microsoft/onnxruntime/releases/download/v$UnpinnedVersion/$FixtureBase.zip") {
            Copy-Item -LiteralPath $FixtureZip -Destination $OutFile
            return
        }
        throw "Unexpected download request: $Uri"
    }
    foreach ($ExpectedRequests in @(2, 1)) {
        $Requests.Clear()
        try {
            & (Join-Path $TestRoot "scripts\update-windows.ps1") -OnnxRuntimeVersion latest -Build
            throw "Expected the build to reach the OBS download"
        } catch {
            if ($_.Exception.Message -ne "Reached OBS download") { throw }
        }
        if ($Requests.Count -ne $ExpectedRequests) {
            throw "Unexpected requests while building the verified runtime: $Requests"
        }
    }
    Write-Host "PASS latest builds reuse the verified archive on fresh and cached updates"
} finally {
    if (Test-Path -LiteralPath $TestRoot) {
        Remove-Item -LiteralPath $TestRoot -Recurse -Force
    }
}
