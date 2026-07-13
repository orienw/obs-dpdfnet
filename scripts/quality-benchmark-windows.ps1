# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$CleanWav,
    [Parameter(Mandatory = $true)][string]$NoiseWav,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')]
    [string]$CaseName,
    [string]$ModelName = "dpdfnet8_48khz_hr",
    [double[]]$SnrDb = @(0, 5, 10),
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$Configuration = "Release",
    [switch]$Overwrite
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dependency-versions.ps1")

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$OutputDir = Join-Path $Root "build\msvc\$Configuration"
$Executable = Join-Path $OutputDir "dpdfnet-quality-benchmark.exe"
$Model = Join-Path $Root "models\$ModelName.onnx"
$SourceCommitFile = Join-Path $Root "build\msvc\$Configuration\source-commit.txt"
$OrtVersionFile = Join-Path $Root "third_party\onnxruntime\VERSION_NUMBER"
$OrtDll = Join-Path $Root "build\msvc\$Configuration\onnxruntime_dpdfnet.dll"
if (!(Test-Path $Executable)) {
    throw "Quality benchmark not found at $Executable. Run scripts\build-windows-msvc.ps1 first."
}
if (!(Test-Path $Model)) { throw "Model not found at $Model" }
if (!(Test-Path $CleanWav)) { throw "Clean WAV not found at $CleanWav" }
if (!(Test-Path $NoiseWav)) { throw "Noise WAV not found at $NoiseWav" }
if (!(Test-Path $SourceCommitFile)) { throw "Build provenance not found at $SourceCommitFile" }
if (!(Test-Path $OrtVersionFile)) { throw "ONNX Runtime version not found at $OrtVersionFile" }
if (!(Test-Path $OrtDll)) { throw "ONNX Runtime DLL not found at $OrtDll" }
$ObsBin = Join-Path $ObsInstallDir "bin\64bit"
if (!(Test-Path (Join-Path $ObsBin "obs.dll"))) {
    throw "OBS runtime not found under $ObsBin"
}
$env:PATH = "$OutputDir;$ObsBin;$env:PATH"

$BuildCommit = (Get-Content -Raw $SourceCommitFile).Trim().ToLowerInvariant()
if ($BuildCommit -match '-dirty$') {
    throw "Quality baselines require a clean build. Commit and rebuild first."
}
$Commit = (& git.exe -C $Root rev-parse HEAD).Trim().ToLowerInvariant()
if ($LASTEXITCODE -ne 0) { throw "Could not read the source commit" }
$Dirty = & git.exe -C $Root status --porcelain
if ($LASTEXITCODE -ne 0) { throw "Could not inspect the working tree" }
if ($Dirty) { throw "Quality baselines require a clean working tree" }
if ($BuildCommit -ne $Commit) {
    throw "Quality benchmark was built from $BuildCommit, but the current source is $Commit. Rebuild first."
}
$OrtVersion = (Get-Content -Raw $OrtVersionFile).Trim()

$ResultRoot = Join-Path $Root "build\quality-results\$ModelName\$CaseName"
if (Test-Path $ResultRoot) {
    $Existing = Get-ChildItem -Force $ResultRoot | Select-Object -First 1
    if ($Existing -and !$Overwrite) {
        throw "Quality case '$CaseName' already has results. Use a new case name or pass -Overwrite explicitly."
    }
    if ($Overwrite) { Remove-Item -Recurse -Force $ResultRoot }
}
New-Item -ItemType Directory -Force -Path $ResultRoot | Out-Null
$Report = Join-Path $ResultRoot "report.txt"
$Header = @(
    "case_name=$CaseName",
    "source_commit=$BuildCommit",
    "benchmark_sha256=$((Get-FileHash $Executable -Algorithm SHA256).Hash.ToLowerInvariant())",
    "onnxruntime_version=$OrtVersion",
    "onnxruntime_dll_sha256=$((Get-FileHash $OrtDll -Algorithm SHA256).Hash.ToLowerInvariant())",
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
