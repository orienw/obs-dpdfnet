# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [Parameter(Mandatory = $true)][string]$Uri,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [string]$FallbackUri
)

$ErrorActionPreference = "Stop"
try {
    $null = Invoke-WebRequest -Uri $Uri -OutFile $OutFile
} catch {
    if (!$FallbackUri) { throw }
    Write-Warning "Download failed for $Uri. Trying $FallbackUri"
    $null = Invoke-WebRequest -Uri $FallbackUri -OutFile $OutFile
}
