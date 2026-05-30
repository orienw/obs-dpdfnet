# SPDX-License-Identifier: GPL-2.0-or-later
#
# End-to-end Windows release: build -> stage -> zip -> checksum -> notes ->
# tag -> push -> GitHub release.
#
# Stages everything locally by default (dry run). Pass -Publish to actually
# tag, push, and create the GitHub release. The version you pass is the single
# source of truth: it is compiled into the DLL, names the zip, and becomes the
# tag, so those three can never drift.
#
#   .\scripts\release-windows.ps1 -Version 0.2.1                 # dry run
#   .\scripts\release-windows.ps1 -Version 0.2.1 -Publish        # ship it
#   .\scripts\release-windows.ps1 -Version 0.2.1 -SkipBuild -Publish

param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ObsVersion = "32.1.2",
    [string]$OnnxRuntimeVersion = "1.26.0",
    [string]$Repo = "orienw/obs-dpdfnet",
    [switch]$PreRelease,
    [switch]$Draft,
    [switch]$SkipBuild,
    [switch]$Publish
)

$ErrorActionPreference = "Stop"

if ($Version -notmatch '^\d+\.\d+\.\d+(-[A-Za-z0-9.]+)?$') {
    throw "Version '$Version' must look like 0.2.1 or 0.3.0-rc1."
}

# 0.x or a -suffix is a pre-release unless this is a clean 1.0.0+ tag.
$IsPreRelease = $PreRelease -or ($Version -match '^0\.') -or ($Version -match '-')

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Configuration = "Release"
$BuildDir = Join-Path $Root "build\msvc"
$OutDir = Join-Path $Root "build"
$Staging = Join-Path $OutDir "release-staging"
$PluginStage = Join-Path $Staging "obs-dpdfnet"
$ZipName = "obs-dpdfnet-$Version-windows-x64.zip"
$ZipPath = Join-Path $OutDir $ZipName
$NotesPath = Join-Path $OutDir "release-notes-v$Version.md"
$Tag = "v$Version"

# 1. Build with the version baked in (skip to reuse an existing build).
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build-windows-msvc.ps1") `
        -PluginVersion $Version `
        -ObsVersion $ObsVersion `
        -OnnxRuntimeVersion $OnnxRuntimeVersion
}

$PluginDll = Join-Path $BuildDir "$Configuration\obs-dpdfnet.dll"
if (!(Test-Path $PluginDll)) {
    throw "Plugin DLL not found at $PluginDll. Run without -SkipBuild."
}

# 2. Stage the install layout with the existing installer, into a clean dir.
if (Test-Path $Staging) { Remove-Item -Recurse -Force $Staging }
New-Item -ItemType Directory -Force -Path $PluginStage | Out-Null
& (Join-Path $PSScriptRoot "install-windows.ps1") `
    -BuildDir $BuildDir `
    -Configuration $Configuration `
    -PluginRoot $PluginStage

# ONNX Runtime's own MIT LICENSE alongside its ThirdPartyNotices.
$OrtLicense = Join-Path $Root "third_party\onnxruntime\LICENSE"
if (Test-Path $OrtLicense) {
    Copy-Item $OrtLicense -Destination (Join-Path $PluginStage "data\ONNXRuntime-LICENSE.txt") -Force
}

# 3. INSTALL.txt at the zip root.
$InstallTxt = @"
obs-dpdfnet $Version - Windows x64
DPDFNet local speech-enhancement audio filter for OBS Studio.

REQUIREMENTS
- OBS Studio $ObsVersion (x64), Windows 10/11 64-bit
- OBS audio sample rate set to 48 kHz (Settings -> Audio -> Sample Rate)

INSTALL
1. Close OBS Studio.
2. Copy the "obs-dpdfnet" folder from this zip into:
       %ProgramData%\obs-studio\plugins\
   The final path should look like:
       %ProgramData%\obs-studio\plugins\obs-dpdfnet\bin\64bit\obs-dpdfnet.dll
3. Start OBS, then add the filter:
       Audio Mixer -> mic gear -> Filters -> + -> DPDFNet Noise Suppression

NOTES
- This binary is unsigned. Windows SmartScreen or Defender may warn on first run.
- Built against OBS Studio $ObsVersion and ONNX Runtime $OnnxRuntimeVersion.
- Single-channel speech enhancer. Use it on a microphone source, not on desktop
  audio or music. Pick the mic input channel explicitly on stereo sources.

LICENSING
- Plugin code: GPL-2.0-or-later. See data\LICENSE.
- Bundled DPDFNet models: Apache-2.0. ONNX Runtime: MIT. KissFFT: BSD-3-Clause.
- Details: data\THIRD_PARTY.md, data\LICENSES\, data\ThirdPartyNotices.txt,
  data\ONNXRuntime-LICENSE.txt.

Source code: https://github.com/$Repo (release tag $Tag)
"@
Set-Content -Encoding ASCII -Path (Join-Path $Staging "INSTALL.txt") -Value $InstallTxt

# 4. Zip.
if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path $PluginStage, (Join-Path $Staging "INSTALL.txt") -DestinationPath $ZipPath

# 5. Checksum.
$Sha = (Get-FileHash -Algorithm SHA256 -Path $ZipPath).Hash.ToLowerInvariant()
"$Sha  $ZipName" | Set-Content -Encoding ASCII "$ZipPath.sha256"

# 6. Release notes.
$Kind = if ($IsPreRelease) { "Early pre-release" } else { "Release" }
$Notes = @"
$Kind of **obs-dpdfnet**, a native OBS audio filter for local DPDFNet speech enhancement. Audio is processed locally; the plugin makes no network requests at runtime.

## Install (Windows x64)

1. Close OBS Studio.
2. Extract ``$ZipName`` and copy the ``obs-dpdfnet`` folder into ``%ProgramData%\obs-studio\plugins\``.
3. Start OBS and add: **Audio Mixer -> mic gear -> Filters -> + -> DPDFNet Noise Suppression**.

## Requirements

- OBS Studio $ObsVersion (x64), Windows 10/11 64-bit
- OBS audio sample rate set to **48 kHz**

## Notes

- Windows is the only tested path.
- The binary is **unsigned**; Windows SmartScreen or Defender may warn on first run.
- Built against **OBS Studio $ObsVersion** and **ONNX Runtime $OnnxRuntimeVersion**.

## Verify your download

SHA-256 of ``$ZipName``:

``````
$Sha
``````

## Licensing

Plugin code is **GPL-2.0-or-later**; the corresponding source is this release's tag (``$Tag``). Bundled DPDFNet models are Apache-2.0, ONNX Runtime is MIT, KissFFT is BSD-3-Clause. See ``THIRD_PARTY.md``, ``LICENSES/``, and the bundled ONNX Runtime notices.
"@
Set-Content -Encoding ASCII -Path $NotesPath -Value $Notes

Write-Host ""
Write-Host "Staged $ZipName"
Write-Host "  zip:    $ZipPath"
Write-Host "  sha256: $Sha"
Write-Host "  notes:  $NotesPath"
Write-Host "  tag:    $Tag (prerelease=$IsPreRelease)"

if (-not $Publish) {
    Write-Host ""
    Write-Host "Dry run. Re-run with -Publish to tag, push, and create the GitHub release."
    return
}

# 7. Publish preconditions: clean tree, HEAD pushed, tag free.
if (git -C $Root status --porcelain) {
    throw "Working tree is not clean. Commit or stash before publishing."
}
$Branch = (git -C $Root rev-parse --abbrev-ref HEAD).Trim()
git -C $Root fetch origin $Branch 2>$null | Out-Null
$Head = (git -C $Root rev-parse HEAD).Trim()
$RemoteHead = (git -C $Root rev-parse "origin/$Branch").Trim()
if ($Head -ne $RemoteHead) {
    throw "HEAD ($Head) is not pushed to origin/$Branch ($RemoteHead). Push first."
}
if (git -C $Root tag -l $Tag) {
    throw "Tag $Tag already exists. Bump the version or delete the tag."
}

# 8. Tag + push.
git -C $Root tag -a $Tag -m "obs-dpdfnet $Version"
git -C $Root push origin $Tag

# 9. GitHub release.
$GhArgs = @(
    "release", "create", $Tag,
    "-R", $Repo,
    "--title", "obs-dpdfnet $Version",
    "--notes-file", $NotesPath
)
if ($IsPreRelease) { $GhArgs += "--prerelease" }
if ($Draft) { $GhArgs += "--draft" }
$GhArgs += $ZipPath
$GhArgs += "$ZipPath.sha256"

& gh @GhArgs
Write-Host ""
Write-Host "Released $Tag"
