# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$OnnxRuntimeVersion = "",
    [string]$DefaultModelName = "",
    [string[]]$ModelNames = @(),
    [string]$ObsVersion = "",
    [string]$BuildDir = "",
    [switch]$Force,
    [switch]$Build,
    [switch]$Install
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

. (Join-Path $PSScriptRoot "dependency-versions.ps1")

if ([string]::IsNullOrWhiteSpace($OnnxRuntimeVersion)) { $OnnxRuntimeVersion = $DpdfnetDefaultOnnxRuntimeVersion }
if ([string]::IsNullOrWhiteSpace($DefaultModelName)) { $DefaultModelName = $DpdfnetDefaultModelName }
if ($ModelNames.Count -eq 0) { $ModelNames = $DpdfnetDefaultModelNames }
if ([string]::IsNullOrWhiteSpace($ObsVersion)) { $ObsVersion = $DpdfnetMinimumObsVersion }

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
    # A pinned version already identifies the archive and its digest. Only
    # latest and unpinned versions need the GitHub release API to resolve them.
    $version = $OnnxRuntimeVersion -replace "^v", ""
    $expectedHash = $DpdfnetKnownOnnxRuntimeHashes[$version]
    if (!$expectedHash) {
        $release = Get-OnnxRuntimeRelease -Version $OnnxRuntimeVersion
        $version = $release.tag_name -replace "^v", ""
        $asset = @($release.assets | Where-Object { $_.name -eq "onnxruntime-win-x64-$version.zip" })[0]
        if (!$asset) {
            throw "Could not find ONNX Runtime asset onnxruntime-win-x64-$version.zip"
        }
        $expectedHash = $DpdfnetKnownOnnxRuntimeHashes[$version]
        if ($asset.digest -match '^sha256:([0-9a-fA-F]{64})$') {
            $digestHash = $Matches[1].ToLowerInvariant()
            if ($expectedHash -and $expectedHash -ne $digestHash) {
                throw "GitHub's digest for $($asset.name) does not match the hash pinned in dependency-versions.ps1."
            }
            $expectedHash = $digestHash
        }
    }

    $assetName = "onnxruntime-win-x64-$version.zip"
    $zipPath = Join-Path $ThirdParty $assetName
    $ortRoot = Join-Path $ThirdParty "onnxruntime"
    $ortVersionFile = Join-Path $ortRoot "VERSION_NUMBER"
    $currentVersion = $null
    if (Test-Path $ortVersionFile) {
        $currentVersion = (Get-Content -Raw $ortVersionFile).Trim()
    }

    $runtime = @{ Version = $version; Sha256 = $expectedHash }
    $needsUpdate = $Force -or ($currentVersion -ne $version)
    if (!$expectedHash -and ($needsUpdate -or $Build)) {
        throw "ONNX Runtime $version has no pinned hash and GitHub publishes no digest for $assetName. Add its SHA-256 to scripts/dependency-versions.ps1."
    }
    if (!$needsUpdate) {
        Write-Host "ONNX Runtime $version is current."
        return $runtime
    }

    Write-Host "Updating ONNX Runtime to $version"
    $zipHash = Get-Sha256 -Path $zipPath
    if ($Force -or !(Test-Path $zipPath) -or $zipHash -ne $expectedHash) {
        & (Join-Path $PSScriptRoot "download-archive.ps1") `
            -Uri "https://github.com/microsoft/onnxruntime/releases/download/v$version/$assetName" `
            -OutFile $zipPath -FallbackUri (Get-DpdfnetOnnxRuntimeMirrorUri $version)
        $zipHash = Get-Sha256 -Path $zipPath
    }

    if ($zipHash -ne $expectedHash) {
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
    return $runtime
}

function Update-DpdfnetModels {
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $Models "manifest.json") | ConvertFrom-Json
    $upstream = $null
    foreach ($modelName in $ModelNames) {
        $bundled = $DpdfnetDefaultModelNames -contains $modelName
        if ($bundled) {
            $entry = @($manifest.models | Where-Object { $_.name -eq $modelName })
            if ($entry.Count -ne 1) {
                throw "Missing bundled digest for $modelName in models/manifest.json"
            }
            $expectedHash = $entry[0].sha256
        } else {
            # Explicit additional models still come from Ceva's model repository.
            if (!$upstream) {
                $upstream = Invoke-RestMethod -Uri "https://huggingface.co/api/models/Ceva-IP/DPDFNet?blobs=true"
            }
            $repoFile = "onnx/$modelName.onnx"
            $sibling = @($upstream.siblings | Where-Object { $_.rfilename -eq $repoFile })[0]
            if (!$sibling) { throw "Could not find $repoFile in Ceva-IP/DPDFNet" }
            $expectedHash = $sibling.lfs.sha256
            $uri = "https://huggingface.co/Ceva-IP/DPDFNet/resolve/$($upstream.sha)/$repoFile"
        }

        if ($expectedHash -notmatch '^[a-fA-F0-9]{64}$') { throw "Missing SHA-256 for $modelName" }
        $modelPath = Join-Path $Models "$modelName.onnx"
        if (!$Force -and (Get-Sha256 $modelPath) -eq $expectedHash) {
            Write-Host "$modelName is current."
            continue
        }

        Write-Host "Downloading $modelName"
        if ($bundled) {
            $ref = "v$DpdfnetDefaultPluginVersion"
            if ((Test-Path -LiteralPath (Join-Path $Root ".git")) -and (Get-Command git -ErrorAction SilentlyContinue)) {
                $ref = git -C $Root rev-parse HEAD
                if ($LASTEXITCODE -ne 0) { throw "Could not identify the model source commit" }
            }
            $uri = "https://raw.githubusercontent.com/orienw/obs-dpdfnet/$ref/models/$modelName.onnx"
        }
        $tmpPath = "$modelPath.$([guid]::NewGuid()).download"
        try {
            Invoke-WebRequest -Uri $uri -OutFile $tmpPath
            $actualHash = Get-Sha256 $tmpPath
            if (!$actualHash -or $actualHash -ne $expectedHash) {
                throw "Model hash mismatch for $modelName. Expected $expectedHash, got $actualHash"
            }
            Move-Item -Force -LiteralPath $tmpPath -Destination $modelPath
        } finally {
            if (Test-Path -LiteralPath $tmpPath) { Remove-Item -LiteralPath $tmpPath -Force }
        }
    }
}

$runtime = Update-OnnxRuntime
Update-DpdfnetModels

if ($Build) {
    & (Join-Path $PSScriptRoot "build-windows-msvc.ps1") `
        -ObsVersion $ObsVersion `
        -OnnxRuntimeVersion $runtime.Version `
        -OnnxRuntimeSha256 $runtime.Sha256 `
        -ModelName $DefaultModelName `
        -BuildDir $BuildDir
}

if ($Install) {
    & (Join-Path $PSScriptRoot "install-windows.ps1") -BuildDir $BuildDir
}

Write-Host "Update complete."
Write-Host "ONNX Runtime: $($runtime.Version)"
Write-Host "Default model: $DefaultModelName"
