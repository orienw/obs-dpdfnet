# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$OnnxRuntimeVersion = "1.26.0",
    [string]$DefaultModelName = "dpdfnet8_48khz_hr",
    [string[]]$ModelNames = @("dpdfnet8_48khz_hr", "dpdfnet2_48khz_hr")
)

$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "update-windows.ps1") `
    -OnnxRuntimeVersion $OnnxRuntimeVersion `
    -DefaultModelName $DefaultModelName `
    -ModelNames $ModelNames

Write-Host "Bootstrap complete."
