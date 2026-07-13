# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dependency-versions.ps1")

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$OutputDir = Join-Path $Root "build\msvc\$Configuration"
$Executable = Join-Path $OutputDir "dpdfnet-processor-benchmark.exe"
$SourceCommitFile = Join-Path $OutputDir "source-commit.txt"
$OrtVersionFile = Join-Path $Root "third_party\onnxruntime\VERSION_NUMBER"
$OrtDll = Join-Path $OutputDir "onnxruntime_dpdfnet.dll"
if (!(Test-Path $Executable)) {
    throw "Processor benchmark not found at $Executable. Run build-windows-msvc.ps1 first."
}
if (!(Test-Path $SourceCommitFile)) { throw "Build provenance not found at $SourceCommitFile" }
if (!(Test-Path $OrtVersionFile)) { throw "ONNX Runtime version not found at $OrtVersionFile" }
if (!(Test-Path $OrtDll)) { throw "ONNX Runtime DLL not found at $OrtDll" }
$ObsBin = Join-Path $ObsInstallDir "bin\64bit"
if (!(Test-Path (Join-Path $ObsBin "obs.dll"))) {
    throw "OBS runtime not found under $ObsBin"
}
$env:PATH = "$OutputDir;$ObsBin;$env:PATH"

$Report = Join-Path $Root "build\processor-benchmark.txt"
$BuildCommit = (Get-Content -Raw $SourceCommitFile).Trim().ToLowerInvariant()
if ($BuildCommit -match '-dirty$') {
    throw "Benchmark reports require a clean build. Commit and rebuild first."
}
$Commit = (& git.exe -C $Root rev-parse HEAD).Trim().ToLowerInvariant()
if ($LASTEXITCODE -ne 0) { throw "Could not read the current source commit" }
$Dirty = & git.exe -C $Root status --porcelain
if ($LASTEXITCODE -ne 0) { throw "Could not inspect the working tree" }
if ($Dirty) { throw "Benchmark reports require a clean working tree" }
if ($BuildCommit -ne $Commit) {
    throw "Benchmark was built from $BuildCommit, but the current source is $Commit. Rebuild first."
}
$OrtVersion = (Get-Content -Raw $OrtVersionFile).Trim()
@(
    "source_commit=$BuildCommit",
    "benchmark_sha256=$((Get-FileHash $Executable -Algorithm SHA256).Hash.ToLowerInvariant())",
    "onnxruntime_version=$OrtVersion",
    "onnxruntime_dll_sha256=$((Get-FileHash $OrtDll -Algorithm SHA256).Hash.ToLowerInvariant())"
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
