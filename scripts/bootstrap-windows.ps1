# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$OnnxRuntimeVersion = "",
    [string]$DefaultModelName = "",
    [string[]]$ModelNames = @()
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "dependency-versions.ps1")

if ([string]::IsNullOrWhiteSpace($OnnxRuntimeVersion)) { $OnnxRuntimeVersion = $DpdfnetDefaultOnnxRuntimeVersion }
if ([string]::IsNullOrWhiteSpace($DefaultModelName)) { $DefaultModelName = $DpdfnetDefaultModelName }
if ($ModelNames.Count -eq 0) { $ModelNames = $DpdfnetDefaultModelNames }

& (Join-Path $PSScriptRoot "update-windows.ps1") `
    -OnnxRuntimeVersion $OnnxRuntimeVersion `
    -DefaultModelName $DefaultModelName `
    -ModelNames $ModelNames

Write-Host "Bootstrap complete."
