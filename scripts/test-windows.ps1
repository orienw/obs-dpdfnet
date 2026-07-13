# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$Configuration = "Release",
    [switch]$RequireCleanProvenance
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dependency-versions.ps1")

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$OutputDir = Join-Path $Root "build\msvc\$Configuration"
$SourceCommitFile = Join-Path $OutputDir "source-commit.txt"
$PassedFile = Join-Path $OutputDir "tests-passed.txt"
Remove-Item $PassedFile -Force -ErrorAction SilentlyContinue

if (!(Test-Path $SourceCommitFile)) {
    throw "Build provenance not found at $SourceCommitFile. Rebuild before testing."
}
$BuiltCommit = (Get-Content -Raw $SourceCommitFile).Trim().ToLowerInvariant()
if ($RequireCleanProvenance) {
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
if (!(Test-Path (Join-Path $ObsBin "obs.dll"))) {
    throw "OBS runtime not found under $ObsBin"
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

@(
    "source_commit=$BuiltCommit",
    "models=$($DpdfnetDefaultModelNames -join ',')",
    "status=passed"
) | Set-Content -Encoding ASCII $PassedFile
Write-Host "Windows test gate passed: $PassedFile"
