# SPDX-License-Identifier: GPL-2.0-or-later
#
# Windows release artifact staging: build -> stage -> zip -> checksum -> notes.
#
# Run this from Windows PowerShell to produce the release zip and notes. Publish
# from WSL with scripts/publish-release-wsl.sh so git/gh use the WSL GitHub auth
# that is already configured for this checkout.
#
#   .\scripts\release-windows.ps1 -Version 1.0.0-rc2
#   .\scripts\release-windows.ps1 -Version 1.0.0-rc2 -SkipBuild
#   ./scripts/publish-release-wsl.sh 1.0.0-rc2

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ObsVersion = "",
    [string]$OnnxRuntimeVersion = "",
    [string]$Repo = "orienw/obs-dpdfnet",
    [string[]]$Changelog = @(),
    [string]$SourceCommit = "",
    [switch]$PreRelease,
    [switch]$SkipBuild,
    [switch]$Draft,
    [switch]$Publish
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "dependency-versions.ps1")

function Assert-NoReparsePoints {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Pending = [System.Collections.Generic.Stack[string]]::new()
    $Pending.Push($Path)
    while ($Pending.Count -gt 0) {
        $Item = Get-Item -Force -LiteralPath $Pending.Pop()
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Release staging paths may not contain a symbolic link or junction: $($Item.FullName)"
        }
        if ($Item.PSIsContainer) {
            foreach ($Child in Get-ChildItem -Force -LiteralPath $Item.FullName) {
                $Pending.Push($Child.FullName)
            }
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ObsVersion)) { $ObsVersion = $DpdfnetDefaultObsVersion }
if ([string]::IsNullOrWhiteSpace($OnnxRuntimeVersion)) { $OnnxRuntimeVersion = $DpdfnetDefaultOnnxRuntimeVersion }

if ($Version -notmatch '^\d+\.\d+\.\d+(-[A-Za-z0-9.]+)?$') {
    throw "Version '$Version' must look like 1.0.0 or 1.0.0-rc2."
}

$PinnedObsArchiveHash = $DpdfnetKnownObsArchiveHashes[$ObsVersion]
$PinnedOrtArchiveHash = $DpdfnetKnownOnnxRuntimeHashes[$OnnxRuntimeVersion]
if (!$PinnedObsArchiveHash -or !$PinnedOrtArchiveHash) {
    throw "Release staging requires pinned OBS and ONNX Runtime versions."
}

if ($Publish -or $Draft) {
    throw "Publishing moved to WSL. First run this script without -Publish/-Draft, then run: ./scripts/publish-release-wsl.sh $Version"
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
$CommitPath = Join-Path $OutDir "release-commit-v$Version.txt"
$Tag = "v$Version"

if (Test-Path -LiteralPath $OutDir) {
    $OutDirItem = Get-Item -Force -LiteralPath $OutDir
    if (!$OutDirItem.PSIsContainer) {
        throw "Release output path is not a directory: $OutDir"
    }
    if (($OutDirItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Release output path may not be a symbolic link or junction: $OutDir"
    }
}

# Purge this version's prior outputs up front so a failed rerun can never
# leave a stale zip/checksum pair alongside a fresh commit stamp.
foreach ($StaleOutput in @($ZipPath, "$ZipPath.sha256", $NotesPath, $CommitPath)) {
    if (Test-Path $StaleOutput) { Remove-Item -Force $StaleOutput }
}

# 1. Build with the version baked in (skip to reuse an existing build).
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build-windows-msvc.ps1") `
        -PluginVersion $Version `
        -ObsVersion $ObsVersion `
        -OnnxRuntimeVersion $OnnxRuntimeVersion `
        -Configuration $Configuration
}

# The behavioral gate always runs, including with -SkipBuild. It rejects stale
# test binaries and a dirty source tree before any files enter release staging.
& (Join-Path $PSScriptRoot "test-windows.ps1") `
    -ObsInstallDir "C:\Program Files\obs-studio" `
    -Configuration $Configuration `
    -RequireCleanProvenance

$PluginDll = Join-Path $BuildDir "$Configuration\obs-dpdfnet.dll"
if (!(Test-Path $PluginDll)) {
    throw "Plugin DLL not found at $PluginDll. Run without -SkipBuild."
}

# A skipped build may only be reused for the exact release inputs that
# produced it. The test gate has already verified every recorded artifact
# hash; these checks prevent relabeling those binaries at packaging time.
$BuildProvenancePath = Join-Path $BuildDir "$Configuration\build-provenance.json"
if (!(Test-Path $BuildProvenancePath -PathType Leaf)) {
    throw "No complete build provenance at $BuildProvenancePath. Re-run without -SkipBuild."
}
try {
    $BuildProvenance = Get-Content -Raw -LiteralPath $BuildProvenancePath | ConvertFrom-Json
} catch {
    throw "Build provenance is not valid JSON: $BuildProvenancePath"
}
if ($BuildProvenance.schemaVersion -ne 2) {
    throw "Unsupported build provenance schema '$($BuildProvenance.schemaVersion)'."
}
if ($BuildProvenance.obsSourceArchiveSha256 -cne $PinnedObsArchiveHash -or
    $BuildProvenance.onnxRuntimeArchiveSha256 -cne $PinnedOrtArchiveHash) {
    throw "Build provenance dependency archive hashes do not match the pinned release inputs."
}
$ExpectedBuildMetadata = [ordered]@{
    pluginVersion = $Version
    obsVersion = $ObsVersion
    onnxRuntimeVersion = $OnnxRuntimeVersion
    onnxRuntimeReportedVersion = $OnnxRuntimeVersion
    configuration = $Configuration
    architecture = "x64"
}
foreach ($Property in $ExpectedBuildMetadata.Keys) {
    if ([string]$BuildProvenance.$Property -cne [string]$ExpectedBuildMetadata[$Property]) {
        throw "Build provenance records $Property '$($BuildProvenance.$Property)', but this release requests '$($ExpectedBuildMetadata[$Property])'. Rebuild with matching inputs."
    }
}
if ($BuildProvenance.sourceDirty -isnot [bool] -or $BuildProvenance.sourceDirty) {
    throw "The staged build was not produced from a clean working tree. Commit and rebuild before releasing."
}
if ($BuildProvenance.sourceCommit -notmatch '^[0-9a-fA-F]{40,64}$') {
    throw "Build provenance contains an invalid source commit."
}
$PluginArtifact = @($BuildProvenance.artifacts | Where-Object { $_.path -ceq "obs-dpdfnet.dll" })
if ($PluginArtifact.Count -ne 1 -or $PluginArtifact[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') {
    throw "Build provenance does not contain exactly one valid plugin DLL hash."
}

# 2. Stage the install layout with the existing installer, into a clean dir.
if (Test-Path -LiteralPath $Staging) {
    $StagingItem = Get-Item -Force -LiteralPath $Staging
    if (!$StagingItem.PSIsContainer) {
        throw "Release staging path is not a directory: $Staging"
    }
    Assert-NoReparsePoints -Path $Staging
    Remove-Item -Recurse -Force -LiteralPath $Staging
}
New-Item -ItemType Directory -Force -Path $PluginStage | Out-Null
& (Join-Path $PSScriptRoot "install-windows.ps1") `
    -BuildDir $BuildDir `
    -Configuration $Configuration `
    -PluginRoot $PluginStage

$StagedArtifacts = @(
    @{ Name = "obs-dpdfnet.dll"; Path = (Join-Path $PluginStage "bin\64bit\obs-dpdfnet.dll") },
    @{ Name = "onnxruntime_dpdfnet.dll"; Path = (Join-Path $PluginStage "bin\64bit\onnxruntime_dpdfnet.dll") },
    @{ Name = "onnxruntime_providers_shared.dll"; Path = (Join-Path $PluginStage "bin\64bit\onnxruntime_providers_shared.dll") }
)
foreach ($Artifact in $StagedArtifacts) {
    $ManifestEntry = @($BuildProvenance.artifacts | Where-Object { $_.path -ceq $Artifact.Name })
    if ($ManifestEntry.Count -ne 1 -or !(Test-Path $Artifact.Path -PathType Leaf)) {
        throw "The staged release is missing the verified build artifact $($Artifact.Name)."
    }
    $StagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Artifact.Path).Hash.ToLowerInvariant()
    if ($StagedHash -cne $ManifestEntry[0].sha256.ToLowerInvariant()) {
        throw "The staged $($Artifact.Name) does not match the verified build artifact."
    }
}
Copy-Item -LiteralPath $BuildProvenancePath `
    -Destination (Join-Path $PluginStage "data\build-provenance.json") -Force

# ONNX Runtime's own MIT LICENSE alongside its ThirdPartyNotices.
$OrtLicense = Join-Path $Root "third_party\onnxruntime\LICENSE"
if (Test-Path $OrtLicense) {
    Copy-Item $OrtLicense -Destination (Join-Path $PluginStage "data\ONNXRuntime-LICENSE.txt") -Force
}

# The commit stamp comes from the hash-bound build manifest, never from HEAD at
# staging time or from caller-supplied release metadata.
$ReleaseCommit = $BuildProvenance.sourceCommit.ToLowerInvariant()
if (![string]::IsNullOrWhiteSpace($SourceCommit) -and
    $SourceCommit.Trim().ToLowerInvariant() -ne $ReleaseCommit.ToLowerInvariant()) {
    throw "-SourceCommit does not match the commit recorded by the staged build. Rebuild the requested source instead of overriding provenance."
}

# 3. INSTALL.txt at the zip root.
$InstallTxt = @"
obs-dpdfnet $Version - Windows x64
DPDFNet local speech-enhancement audio filter for OBS Studio.

REQUIREMENTS
- OBS Studio $ObsVersion (x64), Windows 10/11 64-bit
- OBS audio sample rate: 48 kHz preferred; 44.1 kHz is supported through internal resampling

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

# 6. Release notes. Keep reusable install instructions in README.md.
$Kind = if ($Version -match '-rc\d+$') {
    "Release candidate"
} elseif ($IsPreRelease) {
    "Early pre-release"
} else {
    "Release"
}
$CleanChangelog = @($Changelog | Where-Object { ![string]::IsNullOrWhiteSpace($_) })
$ChangelogSection = ""
if ($CleanChangelog.Count -gt 0) {
    $ChangelogLines = ($CleanChangelog | ForEach-Object {
        $Item = $_.Trim()
        if ($Item.StartsWith("- ")) { $Item } else { "- $Item" }
    }) -join "`r`n"

    $ChangelogSection = @"

## What's Changed

$ChangelogLines
"@
}

$ReadmeInstallUrl = "https://github.com/$Repo#install-a-release-build"
$Notes = @"
$Kind of **obs-dpdfnet**, a native OBS audio filter for local DPDFNet speech enhancement. Audio is processed locally; the plugin makes no network requests at runtime.
$ChangelogSection

## Install

See the Windows release install instructions in the README:
$ReadmeInstallUrl

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

# 7. Commit stamp last: it must only exist once every artifact above was
# regenerated by this run, so a mid-run failure can never pair a fresh stamp
# with a stale zip.
Set-Content -Encoding ASCII -Path $CommitPath -Value $ReleaseCommit

Write-Host ""
Write-Host "Staged $ZipName"
Write-Host "  zip:    $ZipPath"
Write-Host "  sha256: $Sha"
Write-Host "  notes:  $NotesPath"
Write-Host "  commit: $ReleaseCommit"
Write-Host "  tag:    $Tag (prerelease=$IsPreRelease)"
Write-Host ""
Write-Host "Publish from WSL with:"
Write-Host "  ./scripts/publish-release-wsl.sh $Version"
