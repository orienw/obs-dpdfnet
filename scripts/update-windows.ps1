# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$OnnxRuntimeVersion = "latest",
    [string]$DefaultModelName = "dpdfnet8_48khz_hr",
    [string[]]$ModelNames = @("dpdfnet8_48khz_hr", "dpdfnet2_48khz_hr"),
    [string]$ObsVersion = "32.1.2",
    [string]$BuildDir = "",
    [switch]$Force,
    [switch]$Build,
    [switch]$Install
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$ThirdParty = Join-Path $Root "third_party"
$Models = Join-Path $Root "models"
New-Item -ItemType Directory -Force -Path $ThirdParty, $Models | Out-Null

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $Root "build\msvc"
}

if ($ModelNames -notcontains $DefaultModelName) {
    $ModelNames = @($DefaultModelName) + $ModelNames
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (!(Test-Path $Path)) {
        return $null
    }

    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

function Get-OnnxRuntimeRelease {
    param([Parameter(Mandatory = $true)][string]$Version)

    if ($Version -eq "latest") {
        return Invoke-RestMethod -Uri "https://api.github.com/repos/microsoft/onnxruntime/releases/latest"
    }

    $tag = $Version
    if (!$tag.StartsWith("v")) {
        $tag = "v$tag"
    }

    return Invoke-RestMethod -Uri "https://api.github.com/repos/microsoft/onnxruntime/releases/tags/$tag"
}

function Update-OnnxRuntime {
    $release = Get-OnnxRuntimeRelease -Version $OnnxRuntimeVersion
    $version = $release.tag_name -replace "^v", ""
    $assetName = "onnxruntime-win-x64-$version.zip"
    $asset = @($release.assets | Where-Object { $_.name -eq $assetName })[0]
    if (!$asset) {
        throw "Could not find ONNX Runtime asset $assetName"
    }

    $zipPath = Join-Path $ThirdParty $assetName
    $ortRoot = Join-Path $ThirdParty "onnxruntime"
    $ortVersionFile = Join-Path $ortRoot "VERSION_NUMBER"
    $currentVersion = $null
    if (Test-Path $ortVersionFile) {
        $currentVersion = (Get-Content -Raw $ortVersionFile).Trim()
    }

    $expectedHash = $null
    if ($asset.digest -and $asset.digest.StartsWith("sha256:")) {
        $expectedHash = $asset.digest.Substring("sha256:".Length).ToLowerInvariant()
    }

    $needsUpdate = $Force -or !(Test-Path $ortRoot) -or ($currentVersion -ne $version)
    if (!$needsUpdate) {
        Write-Host "ONNX Runtime $version is current."
        return $version
    }

    Write-Host "Updating ONNX Runtime to $version"
    $zipHash = Get-Sha256 -Path $zipPath
    if ($Force -or !(Test-Path $zipPath) -or ($expectedHash -and $zipHash -ne $expectedHash)) {
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath
        $zipHash = Get-Sha256 -Path $zipPath
    }

    if ($expectedHash -and $zipHash -ne $expectedHash) {
        throw "ONNX Runtime hash mismatch for $assetName. Expected $expectedHash, got $zipHash"
    }

    $expandedDir = Join-Path $ThirdParty "onnxruntime-win-x64-$version"
    if (Test-Path $expandedDir) {
        Remove-Item -Recurse -Force $expandedDir
    }

    Expand-Archive -Path $zipPath -DestinationPath $ThirdParty -Force

    if (Test-Path $ortRoot) {
        Remove-Item -Recurse -Force $ortRoot
    }

    Rename-Item -Path $expandedDir -NewName "onnxruntime"
    return $version
}

function Update-DpdfnetModels {
    $repo = Invoke-RestMethod -Uri "https://huggingface.co/api/models/Ceva-IP/DPDFNet?blobs=true"
    $records = @()

    foreach ($modelName in $ModelNames) {
        $repoFile = "onnx/$modelName.onnx"
        $sibling = @($repo.siblings | Where-Object { $_.rfilename -eq $repoFile })[0]
        if (!$sibling) {
            throw "Could not find $repoFile in Ceva-IP/DPDFNet"
        }

        $expectedHash = $sibling.lfs.sha256.ToLowerInvariant()
        $modelPath = Join-Path $Models "$modelName.onnx"
        $currentHash = Get-Sha256 -Path $modelPath

        if ($Force -or !(Test-Path $modelPath) -or ($currentHash -ne $expectedHash)) {
            Write-Host "Downloading $modelName"
            $tmpPath = "$modelPath.download"
            Invoke-WebRequest `
                -Uri "https://huggingface.co/Ceva-IP/DPDFNet/resolve/$($repo.sha)/$repoFile" `
                -OutFile $tmpPath

            $downloadHash = Get-Sha256 -Path $tmpPath
            if ($downloadHash -ne $expectedHash) {
                Remove-Item -Force $tmpPath -ErrorAction SilentlyContinue
                throw "Model hash mismatch for $modelName. Expected $expectedHash, got $downloadHash"
            }

            Move-Item -Force $tmpPath $modelPath
        } else {
            Write-Host "$modelName is current."
        }

        $records += [PSCustomObject]@{
            name = $modelName
            file = $repoFile
            sha256 = $expectedHash
            size = $sibling.size
        }
    }

    [PSCustomObject]@{
        updatedAt = (Get-Date).ToUniversalTime().ToString("o")
        source = "Ceva-IP/DPDFNet"
        revision = $repo.sha
        defaultModel = $DefaultModelName
        models = $records
    } | ConvertTo-Json -Depth 5 |
        Set-Content -Encoding ASCII -Path (Join-Path $Models "manifest.json")
}

$resolvedOnnxRuntimeVersion = Update-OnnxRuntime
Update-DpdfnetModels

if ($Build) {
    & (Join-Path $PSScriptRoot "build-windows-msvc.ps1") `
        -ObsVersion $ObsVersion `
        -OnnxRuntimeVersion $resolvedOnnxRuntimeVersion `
        -ModelName $DefaultModelName
}

if ($Install) {
    & (Join-Path $PSScriptRoot "install-windows.ps1") -BuildDir $BuildDir
}

Write-Host "Update complete."
Write-Host "ONNX Runtime: $resolvedOnnxRuntimeVersion"
Write-Host "Default model: $DefaultModelName"
