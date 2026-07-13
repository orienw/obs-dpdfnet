# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dependency-versions.ps1")

if ($Configuration -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,30}[A-Za-z0-9_-])?$') {
    throw "Configuration '$Configuration' must be a simple directory name."
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$OutputDir = Join-Path $Root "build\msvc\$Configuration"
$Executable = Join-Path $OutputDir "dpdfnet-processor-benchmark.exe"
$BuildProvenanceFile = Join-Path $OutputDir "build-provenance.json"
$OrtVersionFile = Join-Path $Root "third_party\onnxruntime\VERSION_NUMBER"
$OrtDll = Join-Path $OutputDir "onnxruntime_dpdfnet.dll"
$OrtProvidersDll = Join-Path $OutputDir "onnxruntime_providers_shared.dll"
$OrtVersionExecutable = Join-Path $OutputDir "dpdfnet-onnxruntime-version.exe"
if (!(Test-Path $Executable)) {
    throw "Processor benchmark not found at $Executable. Run build-windows-msvc.ps1 first."
}
if (!(Test-Path $BuildProvenanceFile -PathType Leaf)) {
    throw "Build provenance not found at $BuildProvenanceFile"
}
if (!(Test-Path $OrtVersionFile)) { throw "ONNX Runtime version not found at $OrtVersionFile" }
if (!(Test-Path $OrtDll)) { throw "ONNX Runtime DLL not found at $OrtDll" }
if (!(Test-Path $OrtProvidersDll)) { throw "ONNX Runtime providers DLL not found at $OrtProvidersDll" }
if (!(Test-Path $OrtVersionExecutable)) { throw "ONNX Runtime version query not found at $OrtVersionExecutable" }

try {
    $BuildProvenance = Get-Content -Raw -LiteralPath $BuildProvenanceFile | ConvertFrom-Json
} catch {
    throw "Build provenance is not valid JSON: $BuildProvenanceFile"
}
foreach ($Property in @(
    "sourceCommit", "pluginVersion", "obsVersion", "obsSourceArchiveSha256",
    "obsRuntimeProductVersion", "obsRuntimeSha256",
    "onnxRuntimeVersion", "onnxRuntimeReportedVersion",
    "onnxRuntimeArchiveSha256",
    "configuration", "architecture"
)) {
    if ([string]::IsNullOrWhiteSpace([string]$BuildProvenance.$Property)) {
        throw "Build provenance is missing '$Property'."
    }
}
if ($BuildProvenance.schemaVersion -ne 2 -or
    $BuildProvenance.sourceCommit -notmatch '^[0-9a-fA-F]{40,64}$' -or
    $BuildProvenance.sourceDirty -isnot [bool] -or
    $BuildProvenance.obsRuntimeProductVersion -cne $BuildProvenance.obsVersion -or
    $BuildProvenance.onnxRuntimeReportedVersion -cne $BuildProvenance.onnxRuntimeVersion -or
    $BuildProvenance.obsRuntimeSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
    $BuildProvenance.obsSourceArchiveSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
    $BuildProvenance.onnxRuntimeArchiveSha256 -notmatch '^[0-9a-fA-F]{64}$') {
    throw "Build provenance is incomplete or invalid. Rebuild before benchmarking."
}
$PinnedObsArchiveHash = $DpdfnetKnownObsArchiveHashes[$BuildProvenance.obsVersion]
$PinnedOrtArchiveHash = $DpdfnetKnownOnnxRuntimeHashes[$BuildProvenance.onnxRuntimeVersion]
if (!$PinnedObsArchiveHash -or !$PinnedOrtArchiveHash) {
    throw "Benchmark reports require pinned OBS and ONNX Runtime versions."
}
if ($BuildProvenance.obsSourceArchiveSha256 -cne $PinnedObsArchiveHash -or
    $BuildProvenance.onnxRuntimeArchiveSha256 -cne $PinnedOrtArchiveHash) {
    throw "Build provenance dependency archive hashes do not match their pinned versions."
}
if ($BuildProvenance.configuration -cne $Configuration -or
    $BuildProvenance.architecture -cne "x64") {
    throw "Build provenance does not match the requested $Configuration x64 benchmark."
}
if ($BuildProvenance.sourceDirty) {
    throw "Benchmark reports require a clean build. Commit and rebuild first."
}

$ObsBin = Join-Path $ObsInstallDir "bin\64bit"
$ObsDll = Join-Path $ObsBin "obs.dll"
if (!(Test-Path $ObsDll -PathType Leaf)) {
    throw "OBS runtime not found under $ObsBin"
}
$ObsRuntimeProductVersion = (Get-Item -LiteralPath $ObsDll).VersionInfo.ProductVersion.Trim()
$ObsRuntimeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ObsDll).Hash.ToLowerInvariant()
if ($ObsRuntimeProductVersion -cne $BuildProvenance.obsRuntimeProductVersion -or
    $ObsRuntimeSha256 -cne $BuildProvenance.obsRuntimeSha256.ToLowerInvariant()) {
    throw "Selected OBS runtime does not match the runtime recorded by the build. Rebuild against this OBS installation."
}
$env:PATH = "$OutputDir;$ObsBin;$env:PATH"

$Report = Join-Path $Root "build\processor-benchmark.txt"
$BuildCommit = $BuildProvenance.sourceCommit.ToLowerInvariant()
$Commit = (& git.exe -C $Root rev-parse HEAD).Trim().ToLowerInvariant()
if ($LASTEXITCODE -ne 0) { throw "Could not read the current source commit" }
$Dirty = & git.exe -C $Root status --porcelain
if ($LASTEXITCODE -ne 0) { throw "Could not inspect the working tree" }
if ($Dirty) { throw "Benchmark reports require a clean working tree" }
if ($BuildCommit -ne $Commit) {
    throw "Benchmark was built from $BuildCommit, but the current source is $Commit. Rebuild first."
}
$OrtVersion = (Get-Content -Raw $OrtVersionFile).Trim()
if ($OrtVersion -cne $BuildProvenance.onnxRuntimeVersion) {
    throw "Build provenance records ONNX Runtime $($BuildProvenance.onnxRuntimeVersion), but third_party contains $OrtVersion."
}
foreach ($Artifact in @(
    @{ Name = "dpdfnet-processor-benchmark.exe"; Path = $Executable },
    @{ Name = "dpdfnet-onnxruntime-version.exe"; Path = $OrtVersionExecutable },
    @{ Name = "onnxruntime_dpdfnet.dll"; Path = $OrtDll },
    @{ Name = "onnxruntime_providers_shared.dll"; Path = $OrtProvidersDll }
)) {
    $Entry = @($BuildProvenance.artifacts | Where-Object { $_.path -ceq $Artifact.Name })
    if ($Entry.Count -ne 1 -or $Entry[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Build provenance does not contain one valid hash for $($Artifact.Name)."
    }
    $ActualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Artifact.Path).Hash.ToLowerInvariant()
    if ($ActualHash -cne $Entry[0].sha256.ToLowerInvariant()) {
        throw "Build artifact hash mismatch for $($Artifact.Name). Rebuild before benchmarking."
    }
}
$ReportedOrtVersionOutput = & $OrtVersionExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Could not query the version reported by onnxruntime_dpdfnet.dll."
}
$ReportedOrtVersion = ($ReportedOrtVersionOutput -join "").Trim()
if ($ReportedOrtVersion -cne $BuildProvenance.onnxRuntimeReportedVersion -or
    $ReportedOrtVersion -cne $BuildProvenance.onnxRuntimeVersion) {
    throw "onnxruntime_dpdfnet.dll reports '$ReportedOrtVersion', which does not match build provenance."
}
@(
    "source_commit=$BuildCommit",
    "plugin_version=$($BuildProvenance.pluginVersion)",
    "obs_version=$($BuildProvenance.obsVersion)",
    "obs_source_archive_sha256=$($BuildProvenance.obsSourceArchiveSha256)",
    "obs_runtime_sha256=$($BuildProvenance.obsRuntimeSha256)",
    "benchmark_sha256=$((Get-FileHash $Executable -Algorithm SHA256).Hash.ToLowerInvariant())",
    "onnxruntime_version=$OrtVersion",
    "onnxruntime_reported_version=$ReportedOrtVersion",
    "onnxruntime_archive_sha256=$($BuildProvenance.onnxRuntimeArchiveSha256)",
    "onnxruntime_dll_sha256=$((Get-FileHash $OrtDll -Algorithm SHA256).Hash.ToLowerInvariant())",
    "onnxruntime_providers_sha256=$((Get-FileHash $OrtProvidersDll -Algorithm SHA256).Hash.ToLowerInvariant())"
) | Set-Content -Encoding ASCII $Report

foreach ($ModelName in $DpdfnetDefaultModelNames) {
    $Model = Join-Path $Root "models\$ModelName.onnx"
    foreach ($Rate in @(44100, 48000, 96000)) {
        "" | Add-Content -Encoding ASCII $Report
        "[$ModelName-$Rate]" | Add-Content -Encoding ASCII $Report
        $Output = & $Executable $Model $Rate
        if ($LASTEXITCODE -ne 0) {
            throw "Processor benchmark failed for $ModelName at $Rate Hz"
        }
        $Output | Add-Content -Encoding ASCII $Report
        $Output | Write-Host
    }
}

Write-Host "Benchmark report: $Report"
