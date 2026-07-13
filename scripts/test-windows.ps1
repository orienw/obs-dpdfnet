# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$Configuration = "Release",
    [switch]$RequireCleanProvenance
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dependency-versions.ps1")

if ($Configuration -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,30}[A-Za-z0-9_-])?$') {
    throw "Configuration '$Configuration' must be a simple directory name."
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$OutputDir = Join-Path $Root "build\msvc\$Configuration"
$BuildProvenanceFile = Join-Path $OutputDir "build-provenance.json"
$SourceCommitFile = Join-Path $OutputDir "source-commit.txt"
$PassedFile = Join-Path $OutputDir "tests-passed.txt"
Remove-Item $PassedFile -Force -ErrorAction SilentlyContinue

function Read-BuildProvenance {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (!(Test-Path $Path -PathType Leaf)) {
        throw "Build provenance not found at $Path. Rebuild before testing."
    }
    try {
        $Provenance = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    } catch {
        throw "Build provenance is not valid JSON: $Path"
    }

    if ($Provenance.schemaVersion -ne 2) {
        throw "Unsupported build provenance schema '$($Provenance.schemaVersion)'."
    }
    foreach ($Property in @(
        "sourceCommit", "pluginVersion", "obsVersion", "obsSourceArchiveSha256",
        "obsRuntimeProductVersion", "obsRuntimeSha256",
        "onnxRuntimeVersion", "onnxRuntimeReportedVersion",
        "onnxRuntimeArchiveSha256",
        "configuration", "architecture"
    )) {
        if ([string]::IsNullOrWhiteSpace([string]$Provenance.$Property)) {
            throw "Build provenance is missing '$Property'."
        }
    }
    if ($Provenance.sourceCommit -notmatch '^[0-9a-fA-F]{40,64}$') {
        throw "Build provenance contains an invalid source commit."
    }
    if ($Provenance.pluginVersion -notmatch '^\d+\.\d+\.\d+(-[A-Za-z0-9.]+)?$' -or
        $Provenance.obsVersion -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or
        $Provenance.onnxRuntimeVersion -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') {
        throw "Build provenance contains invalid version metadata."
    }
    if ($Provenance.obsRuntimeProductVersion -cne $Provenance.obsVersion -or
        $Provenance.onnxRuntimeReportedVersion -cne $Provenance.onnxRuntimeVersion -or
        $Provenance.obsRuntimeSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $Provenance.obsSourceArchiveSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $Provenance.onnxRuntimeArchiveSha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Build provenance contains invalid dependency or runtime metadata."
    }
    $PinnedObsArchiveHash = $DpdfnetKnownObsArchiveHashes[$Provenance.obsVersion]
    if ($PinnedObsArchiveHash -and
        $Provenance.obsSourceArchiveSha256 -cne $PinnedObsArchiveHash) {
        throw "Build provenance OBS archive hash does not match the pinned hash for $($Provenance.obsVersion)."
    }
    $PinnedOrtArchiveHash = $DpdfnetKnownOnnxRuntimeHashes[$Provenance.onnxRuntimeVersion]
    if ($PinnedOrtArchiveHash -and
        $Provenance.onnxRuntimeArchiveSha256 -cne $PinnedOrtArchiveHash) {
        throw "Build provenance ONNX Runtime archive hash does not match the pinned hash for $($Provenance.onnxRuntimeVersion)."
    }
    if ($Provenance.sourceDirty -isnot [bool]) {
        throw "Build provenance contains an invalid sourceDirty value."
    }
    if ($Provenance.configuration -cne $Configuration) {
        throw "Build configuration '$($Provenance.configuration)' does not match requested configuration '$Configuration'."
    }
    if ($Provenance.architecture -cne "x64") {
        throw "Build architecture '$($Provenance.architecture)' is not supported by this Windows test gate."
    }

    $ExpectedArtifacts = @(
        "obs-dpdfnet.dll",
        "dpdfnet-model-smoke.exe",
        "dpdfnet-model-contract-test.exe",
        "dpdfnet-onnxruntime-version.exe",
        "obs-dpdfnet-tests.exe",
        "obs-dpdfnet-filter-tests.exe",
        "dpdfnet-stream-dump.exe",
        "dpdfnet-quality-benchmark.exe",
        "dpdfnet-processor-benchmark.exe",
        "onnxruntime_dpdfnet.dll",
        "onnxruntime_providers_shared.dll"
    )
    foreach ($ArtifactName in $ExpectedArtifacts) {
        $Entries = @($Provenance.artifacts | Where-Object { $_.path -ceq $ArtifactName })
        if ($Entries.Count -ne 1 -or $Entries[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Build provenance must contain one valid hash for $ArtifactName."
        }
        $ArtifactPath = Join-Path $OutputDir $ArtifactName
        if (!(Test-Path $ArtifactPath -PathType Leaf)) {
            throw "Build artifact not found: $ArtifactPath"
        }
        $ActualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ArtifactPath).Hash.ToLowerInvariant()
        if ($ActualHash -cne $Entries[0].sha256.ToLowerInvariant()) {
            throw "Build artifact hash mismatch for $ArtifactName. Rebuild before testing."
        }
    }

    return $Provenance
}

$Provenance = Read-BuildProvenance -Path $BuildProvenanceFile
$BuiltCommit = $Provenance.sourceCommit.ToLowerInvariant()

# Keep the legacy stamp honest while older local report scripts still read it.
if (Test-Path $SourceCommitFile -PathType Leaf) {
    $LegacyCommit = (Get-Content -Raw -LiteralPath $SourceCommitFile).Trim().ToLowerInvariant()
    $ExpectedLegacyCommit = if ($Provenance.sourceDirty) { "$BuiltCommit-dirty" } else { $BuiltCommit }
    if ($LegacyCommit -cne $ExpectedLegacyCommit) {
        throw "Legacy source commit stamp disagrees with build-provenance.json. Rebuild before testing."
    }
}

$OrtVersionFile = Join-Path $Root "third_party\onnxruntime\VERSION_NUMBER"
if (!(Test-Path $OrtVersionFile -PathType Leaf)) {
    throw "ONNX Runtime version file not found at $OrtVersionFile"
}
$InstalledOrtVersion = (Get-Content -Raw -LiteralPath $OrtVersionFile).Trim()
if ($InstalledOrtVersion -cne $Provenance.onnxRuntimeVersion) {
    throw "Build provenance records ONNX Runtime $($Provenance.onnxRuntimeVersion), but third_party contains $InstalledOrtVersion."
}
$OrtVersionExecutable = Join-Path $OutputDir "dpdfnet-onnxruntime-version.exe"
$ReportedOrtVersionOutput = & $OrtVersionExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Could not query the version reported by onnxruntime_dpdfnet.dll."
}
$ReportedOrtVersion = ($ReportedOrtVersionOutput -join "").Trim()
if ($ReportedOrtVersion -cne $Provenance.onnxRuntimeReportedVersion -or
    $ReportedOrtVersion -cne $Provenance.onnxRuntimeVersion) {
    throw "onnxruntime_dpdfnet.dll reports '$ReportedOrtVersion', which does not match build provenance."
}

if ($RequireCleanProvenance) {
    if (!$DpdfnetKnownObsArchiveHashes[$Provenance.obsVersion] -or
        !$DpdfnetKnownOnnxRuntimeHashes[$Provenance.onnxRuntimeVersion]) {
        throw "Release tests require pinned OBS and ONNX Runtime versions."
    }
    if ($Provenance.sourceDirty) {
        throw "Release tests require a build produced from a clean working tree"
    }
    $CurrentCommit = (& git.exe -C $Root rev-parse HEAD).Trim().ToLowerInvariant()
    if ($LASTEXITCODE -ne 0) { throw "Could not read the current source commit" }
    $Dirty = & git.exe -C $Root status --porcelain
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect the working tree" }
    if ($Dirty) { throw "Release tests require a clean working tree" }
    if ($BuiltCommit -ne $CurrentCommit) {
        throw "Test binaries were built from $BuiltCommit, but the current source is $CurrentCommit. Rebuild before staging."
    }
}

$ObsBin = Join-Path $ObsInstallDir "bin\64bit"
$ObsDll = Join-Path $ObsBin "obs.dll"
if (!(Test-Path $ObsDll -PathType Leaf)) {
    throw "OBS runtime not found under $ObsBin"
}
$ObsRuntimeProductVersion = (Get-Item -LiteralPath $ObsDll).VersionInfo.ProductVersion.Trim()
$ObsRuntimeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ObsDll).Hash.ToLowerInvariant()
if ($ObsRuntimeProductVersion -cne $Provenance.obsRuntimeProductVersion -or
    $ObsRuntimeSha256 -cne $Provenance.obsRuntimeSha256.ToLowerInvariant()) {
    throw "Selected OBS runtime does not match the runtime recorded by the build. Rebuild against this OBS installation."
}
$env:PATH = "$OutputDir;$ObsBin;$env:PATH"

$ManifestPath = Join-Path $Root "models\manifest.json"
$Manifest = Get-Content -Raw $ManifestPath | ConvertFrom-Json
foreach ($ModelName in $DpdfnetDefaultModelNames) {
    $ModelPath = Join-Path $Root "models\$ModelName.onnx"
    if (!(Test-Path $ModelPath)) { throw "Bundled model missing: $ModelPath" }
    $Entry = $Manifest.models | Where-Object { $_.name -eq $ModelName }
    if (!$Entry) { throw "Model $ModelName is missing from manifest.json" }
    $Hash = (Get-FileHash $ModelPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($Hash -ne $Entry.sha256) {
        throw "Model hash mismatch for $ModelName. Expected $($Entry.sha256), got $Hash"
    }
}

$QualityModel = Join-Path $Root "models\dpdfnet8_48khz_hr.onnx"
$LowCpuModel = Join-Path $Root "models\dpdfnet2_48khz_hr.onnx"
$Fixtures = Join-Path $Root "tests\fixtures\model-contract"

function Invoke-TestExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $Executable = Join-Path $OutputDir "$Name.exe"
    if (!(Test-Path $Executable)) {
        throw "Test executable not found: $Executable"
    }
    Write-Host "Running $Name"
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
}

Invoke-TestExecutable -Name "dpdfnet-model-smoke" -Arguments @($QualityModel)
Invoke-TestExecutable -Name "dpdfnet-model-smoke" -Arguments @($LowCpuModel)
Invoke-TestExecutable -Name "dpdfnet-model-contract-test" -Arguments @(
    $Fixtures, $QualityModel, $LowCpuModel
)
Invoke-TestExecutable -Name "obs-dpdfnet-tests" -Arguments @(
    $QualityModel, $LowCpuModel, $Fixtures
)
Invoke-TestExecutable -Name "obs-dpdfnet-filter-tests" -Arguments @(
    $QualityModel
)

@(
    "source_commit=$BuiltCommit",
    "source_dirty=$($Provenance.sourceDirty.ToString().ToLowerInvariant())",
    "plugin_version=$($Provenance.pluginVersion)",
    "obs_version=$($Provenance.obsVersion)",
    "obs_source_archive_sha256=$($Provenance.obsSourceArchiveSha256)",
    "obs_runtime_sha256=$($Provenance.obsRuntimeSha256)",
    "onnxruntime_version=$($Provenance.onnxRuntimeVersion)",
    "onnxruntime_reported_version=$($Provenance.onnxRuntimeReportedVersion)",
    "onnxruntime_archive_sha256=$($Provenance.onnxRuntimeArchiveSha256)",
    "configuration=$($Provenance.configuration)",
    "architecture=$($Provenance.architecture)",
    "models=$($DpdfnetDefaultModelNames -join ',')",
    "status=passed"
) | Set-Content -Encoding ASCII $PassedFile
Write-Host "Windows test gate passed: $PassedFile"
