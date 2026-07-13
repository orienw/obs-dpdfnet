# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$CleanWav,
    [Parameter(Mandatory = $true)][string]$NoiseWav,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,62}[A-Za-z0-9_-])?$')]
    [string]$CaseName,
    [ValidatePattern('^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,62}[A-Za-z0-9_-])?$')]
    [string]$ModelName = "dpdfnet8_48khz_hr",
    [double[]]$SnrDb = @(0, 5, 10),
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$Configuration = "Release",
    [switch]$Overwrite
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dependency-versions.ps1")

if ($Configuration -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,30}[A-Za-z0-9_-])?$') {
    throw "Configuration '$Configuration' must be a simple directory name."
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Assert-SafeWindowsLeafName {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$ParameterName
    )

    $Stem = @($Name -split '\.', 2)[0]
    if ($Stem -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
        throw "$ParameterName '$Name' is a reserved Windows file name."
    }
}

function Get-ContainedPath {
    param(
        [Parameter(Mandatory = $true)][string]$Base,
        [Parameter(Mandatory = $true)][string]$Child
    )

    $BaseFull = [System.IO.Path]::GetFullPath($Base)
    $CandidateFull = [System.IO.Path]::GetFullPath((Join-Path $BaseFull $Child))
    $Separator = [System.IO.Path]::DirectorySeparatorChar.ToString()
    $BasePrefix = $BaseFull
    if (!$BasePrefix.EndsWith($Separator)) { $BasePrefix += $Separator }
    if (!$CandidateFull.StartsWith($BasePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path '$CandidateFull' escapes its allowed root '$BaseFull'."
    }
    return $CandidateFull
}

function Assert-NoReparsePoints {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Pending = [System.Collections.Generic.Stack[string]]::new()
    $Pending.Push($Path)
    while ($Pending.Count -gt 0) {
        $Item = Get-Item -Force -LiteralPath $Pending.Pop()
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Quality output paths may not contain a symbolic link or junction: $($Item.FullName)"
        }
        if ($Item.PSIsContainer) {
            foreach ($Child in Get-ChildItem -Force -LiteralPath $Item.FullName) {
                $Pending.Push($Child.FullName)
            }
        }
    }
}

Assert-SafeWindowsLeafName -Name $CaseName -ParameterName "CaseName"
Assert-SafeWindowsLeafName -Name $ModelName -ParameterName "ModelName"

$OutputDir = Join-Path $Root "build\msvc\$Configuration"
$Executable = Join-Path $OutputDir "dpdfnet-quality-benchmark.exe"
$ModelsRoot = [System.IO.Path]::GetFullPath((Join-Path $Root "models"))
$Model = Get-ContainedPath -Base $ModelsRoot -Child "$ModelName.onnx"
$BuildProvenanceFile = Join-Path $OutputDir "build-provenance.json"
$OrtVersionFile = Join-Path $Root "third_party\onnxruntime\VERSION_NUMBER"
$OrtDll = Join-Path $Root "build\msvc\$Configuration\onnxruntime_dpdfnet.dll"
$OrtProvidersDll = Join-Path $OutputDir "onnxruntime_providers_shared.dll"
$OrtVersionExecutable = Join-Path $OutputDir "dpdfnet-onnxruntime-version.exe"
if (!(Test-Path $Executable)) {
    throw "Quality benchmark not found at $Executable. Run scripts\build-windows-msvc.ps1 first."
}
if (!(Test-Path $Model)) { throw "Model not found at $Model" }
if (!(Test-Path $CleanWav)) { throw "Clean WAV not found at $CleanWav" }
if (!(Test-Path $NoiseWav)) { throw "Noise WAV not found at $NoiseWav" }
if (!(Test-Path $BuildProvenanceFile -PathType Leaf)) {
    throw "Build provenance not found at $BuildProvenanceFile"
}
if (!(Test-Path $OrtVersionFile)) { throw "ONNX Runtime version not found at $OrtVersionFile" }
if (!(Test-Path $OrtDll)) { throw "ONNX Runtime DLL not found at $OrtDll" }
if (!(Test-Path $OrtProvidersDll)) { throw "ONNX Runtime providers DLL not found at $OrtProvidersDll" }
if (!(Test-Path $OrtVersionExecutable)) { throw "ONNX Runtime version query not found at $OrtVersionExecutable" }
$ObsBin = Join-Path $ObsInstallDir "bin\64bit"
$ObsDll = Join-Path $ObsBin "obs.dll"
if (!(Test-Path $ObsDll -PathType Leaf)) {
    throw "OBS runtime not found under $ObsBin"
}

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
    throw "Quality baselines require pinned OBS and ONNX Runtime versions."
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
    throw "Quality baselines require a clean build. Commit and rebuild first."
}
$ObsRuntimeProductVersion = (Get-Item -LiteralPath $ObsDll).VersionInfo.ProductVersion.Trim()
$ObsRuntimeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ObsDll).Hash.ToLowerInvariant()
if ($ObsRuntimeProductVersion -cne $BuildProvenance.obsRuntimeProductVersion -or
    $ObsRuntimeSha256 -cne $BuildProvenance.obsRuntimeSha256.ToLowerInvariant()) {
    throw "Selected OBS runtime does not match the runtime recorded by the build. Rebuild against this OBS installation."
}
$env:PATH = "$OutputDir;$ObsBin;$env:PATH"
$BuildCommit = $BuildProvenance.sourceCommit.ToLowerInvariant()
$Commit = (& git.exe -C $Root rev-parse HEAD).Trim().ToLowerInvariant()
if ($LASTEXITCODE -ne 0) { throw "Could not read the source commit" }
$Dirty = & git.exe -C $Root status --porcelain
if ($LASTEXITCODE -ne 0) { throw "Could not inspect the working tree" }
if ($Dirty) { throw "Quality baselines require a clean working tree" }
if ($BuildCommit -ne $Commit) {
    throw "Quality benchmark was built from $BuildCommit, but the current source is $Commit. Rebuild first."
}
$OrtVersion = (Get-Content -Raw $OrtVersionFile).Trim()
if ($OrtVersion -cne $BuildProvenance.onnxRuntimeVersion) {
    throw "Build provenance records ONNX Runtime $($BuildProvenance.onnxRuntimeVersion), but third_party contains $OrtVersion."
}
foreach ($Artifact in @(
    @{ Name = "dpdfnet-quality-benchmark.exe"; Path = $Executable },
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

$BuildRoot = [System.IO.Path]::GetFullPath((Join-Path $Root "build"))
$ResultsRoot = Get-ContainedPath -Base $BuildRoot -Child "quality-results"
$ModelResultRoot = Get-ContainedPath -Base $ResultsRoot -Child $ModelName
$ResultRoot = Get-ContainedPath -Base $ModelResultRoot -Child $CaseName
foreach ($Candidate in @($BuildRoot, $ResultsRoot, $ModelResultRoot, $ResultRoot)) {
    if (Test-Path -LiteralPath $Candidate) {
        $Item = Get-Item -Force -LiteralPath $Candidate
        if (!$Item.PSIsContainer) {
            throw "Quality output path is not a directory: $Candidate"
        }
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Quality output paths may not traverse a symbolic link or junction: $Candidate"
        }
    }
}
if (Test-Path -LiteralPath $ResultRoot) {
    $Existing = Get-ChildItem -Force -LiteralPath $ResultRoot | Select-Object -First 1
    if ($Existing -and !$Overwrite) {
        throw "Quality case '$CaseName' already has results. Use a new case name or pass -Overwrite explicitly."
    }
    if ($Overwrite) {
        Assert-NoReparsePoints -Path $ResultRoot
        Remove-Item -Recurse -Force -LiteralPath $ResultRoot
    }
}
New-Item -ItemType Directory -Force -Path $ResultRoot | Out-Null
$Report = Join-Path $ResultRoot "report.txt"
$Header = @(
    "case_name=$CaseName",
    "model_name=$ModelName",
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
    "onnxruntime_providers_sha256=$((Get-FileHash $OrtProvidersDll -Algorithm SHA256).Hash.ToLowerInvariant())",
    "model_sha256=$((Get-FileHash $Model -Algorithm SHA256).Hash.ToLowerInvariant())",
    "clean_sha256=$((Get-FileHash $CleanWav -Algorithm SHA256).Hash.ToLowerInvariant())",
    "noise_sha256=$((Get-FileHash $NoiseWav -Algorithm SHA256).Hash.ToLowerInvariant())",
    "subjective_review_required=true"
)
$Header | Set-Content -Encoding UTF8 $Report

foreach ($Snr in $SnrDb) {
    $CaseDirectory = Join-Path $ResultRoot "snr-$Snr-db"
    New-Item -ItemType Directory -Force -Path $CaseDirectory | Out-Null
    "" | Add-Content -Encoding UTF8 $Report
    "[snr-$Snr-db]" | Add-Content -Encoding UTF8 $Report
    $Output = & $Executable $Model $CleanWav $NoiseWav $Snr $CaseDirectory
    if ($LASTEXITCODE -ne 0) { throw "Quality benchmark failed at $Snr dB SNR" }
    $Output | Add-Content -Encoding UTF8 $Report
}

Write-Host "Quality report: $Report"
Write-Host "Listen to every enhanced WAV before approving a new baseline."
