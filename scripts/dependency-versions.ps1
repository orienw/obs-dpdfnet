# SPDX-License-Identifier: GPL-2.0-or-later

# OBS refuses a plugin built against a newer major.minor than itself, so the
# release builds against the oldest OBS it supports and CI also tests it on
# the current one.
$DpdfnetMinimumObsVersion = "32.0.0"
$DpdfnetCurrentObsVersion = "32.2.2"
$DpdfnetDefaultOnnxRuntimeVersion = "1.27.0"
$DpdfnetDefaultPluginVersion = (Get-Content -Raw -LiteralPath (Join-Path (Split-Path $PSScriptRoot) "VERSION")).Trim()
$DpdfnetDefaultModelName = "dpdfnet8_48khz_hr"
$DpdfnetDefaultModelNames = @("dpdfnet8_48khz_hr", "dpdfnet2_48khz_hr")

$DpdfnetKnownOnnxRuntimeHashes = @{
    "1.26.0" = "6ebe99b5564bf4d029b6e93eac9ff423682b6212eade769e9ca3f685eaf500b4"
    "1.27.0" = "c5c81710938e68079ff1a192b04897faabe4b43830d48f39f27ecd4e16138bfc"
}

# OBS-Studio-<version>-Windows-x64.zip, the runtimes Windows CI tests with.
$DpdfnetKnownObsRuntimeHashes = @{
    "32.0.0" = "96ebdcc3e5709825303803e5368b5d7ef04dfbd40c2ce6308fe06a6043066fa0"
    "32.2.1" = "db64a2934f8261f85b1410b84be011207a0afda5400d008289f1f1e211bcc7de"
    "32.2.2" = "4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1"
}

# The OBS source archives whose headers the plugin builds against.
$DpdfnetKnownObsArchiveHashes = @{
    "32.0.0" = "8b91b13f966d018ef2de20d74b85857374bac263f30e13ec1e9acb0212413b94"
    "32.1.2" = "21cba22292985cf0da967d5c618999b40eaa32b73d2ab8b06154b5ea1b3d3798"
    "32.2.1" = "0cc1bd46a3d60c8f4317b38c27414fc0472e04609f4e67ad2142ed1598ef5462"
}
