# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [Parameter(Mandatory = $false)]
    [string]$BuildDir = ".\build",

    [Parameter(Mandatory = $false)]
    [string]$Configuration = "Release",

    [Parameter(Mandatory = $false)]
    [string]$PluginRoot = "$env:ProgramData\obs-studio\plugins\obs-dpdfnet"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Resolve-Path $BuildDir
$BinDir = Join-Path $PluginRoot "bin\64bit"
$DataDir = Join-Path $PluginRoot "data"

function Get-OnnxRuntimeImport {
    param([string]$Path)

    $Reader = [System.IO.BinaryReader]::new([System.IO.File]::OpenRead($Path))
    try {
        if ($Reader.ReadUInt16() -ne 0x5a4d) { throw "Not a Windows DLL: $Path" }
        $Reader.BaseStream.Position = 0x3c
        $PeOffset = $Reader.ReadUInt32()
        $Reader.BaseStream.Position = $PeOffset
        if ($Reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
        $Reader.BaseStream.Position = $PeOffset + 6
        $SectionCount = $Reader.ReadUInt16()
        $Reader.BaseStream.Position = $PeOffset + 20
        $OptionalHeaderSize = $Reader.ReadUInt16()
        $OptionalHeader = $PeOffset + 24
        $Reader.BaseStream.Position = $OptionalHeader
        if ($Reader.ReadUInt16() -ne 0x20b) { throw "Expected a 64-bit Windows DLL: $Path" }

        # PE32+ data directory entry 1 describes the DLL import table.
        $Reader.BaseStream.Position = $OptionalHeader + 120
        $ImportRva = $Reader.ReadUInt32()
        $ImportSize = $Reader.ReadUInt32()
        $Sections = @(for ($Index = 0; $Index -lt $SectionCount; ++$Index) {
            $Reader.BaseStream.Position = $OptionalHeader + $OptionalHeaderSize + $Index * 40 + 12
            [PSCustomObject]@{
                Rva = $Reader.ReadUInt32()
                Size = $Reader.ReadUInt32()
                Offset = $Reader.ReadUInt32()
            }
        })

        function Get-FileOffset([uint32]$Rva) {
            foreach ($Section in $Sections) {
                if ($Rva -ge $Section.Rva -and [long]$Rva -lt [long]$Section.Rva + $Section.Size) {
                    return [long]$Section.Offset + $Rva - $Section.Rva
                }
            }
            throw "DLL import address is outside the PE sections: $Path"
        }

        $ImportOffset = Get-FileOffset $ImportRva
        for ($Offset = 0; $Offset -lt $ImportSize; $Offset += 20) {
            $Reader.BaseStream.Position = $ImportOffset + $Offset + 12
            $NameRva = $Reader.ReadUInt32()
            if (!$NameRva) { break }
            $Reader.BaseStream.Position = Get-FileOffset $NameRva
            $Name = [System.Text.StringBuilder]::new()
            while (($Byte = $Reader.ReadByte()) -ne 0) {
                [void]$Name.Append([char]$Byte)
            }
            $Name = $Name.ToString().ToLowerInvariant()
            if ($Name -in @("onnxruntime.dll", "onnxruntime_dpdfnet.dll")) {
                return $Name
            }
        }
        throw "No supported ONNX Runtime DLL import was found in $Path."
    } finally {
        $Reader.Dispose()
    }
}

# The MSVC script and CMake's Visual Studio generator put the plugin in
# <BuildDir>\<Configuration>; single-config CMake generators put it in
# <BuildDir>. Anything else must be unambiguous.
$Candidates = @(
    (Join-Path $BuildPath "$Configuration\obs-dpdfnet.dll"),
    (Join-Path $BuildPath "obs-dpdfnet.dll")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
if ($Candidates.Count -eq 0) {
    $Candidates = @(Get-ChildItem -Path $BuildPath -Recurse -Filter "obs-dpdfnet.dll" |
        Where-Object { $_.DirectoryName -match "\\$Configuration$" } |
        ForEach-Object { $_.FullName })
    if ($Candidates.Count -gt 1) {
        throw "Found several $Configuration builds of obs-dpdfnet.dll under $BuildPath. Pass -BuildDir for the one to install:`n$($Candidates -join "`n")"
    }
}
if ($Candidates.Count -eq 0) {
    throw "Could not find obs-dpdfnet.dll under $BuildPath. Build the $Configuration configuration first."
}
$Dll = Get-Item -LiteralPath @($Candidates)[0]
$PluginBuildDir = $Dll.DirectoryName

# Select the DLL the plugin imports, even when another build left its runtime
# in the same directory. Remove the other name from the installation.
$RuntimeName = Get-OnnxRuntimeImport $Dll.FullName
$SelectedOrtDll = Join-Path $PluginBuildDir $RuntimeName
if (!(Test-Path -LiteralPath $SelectedOrtDll -PathType Leaf)) {
    throw "ONNX Runtime was not found at $SelectedOrtDll, as imported by $($Dll.FullName). Rebuild; the plugin does not load without it."
}
if ($RuntimeName -eq "onnxruntime_dpdfnet.dll") {
    $ObsoleteOrtName = "onnxruntime.dll"
} else {
    $ObsoleteOrtName = "onnxruntime_dpdfnet.dll"
}

# ONNX Runtime's license and third-party notices go wherever its DLLs go.
$OrtRoot = Join-Path $Root "third_party\onnxruntime"
$Notices = @(
    @{ Source = "LICENSE"; Name = "ONNXRuntime-LICENSE.txt" },
    @{ Source = "ThirdPartyNotices.txt"; Name = "ThirdPartyNotices.txt" }
)
foreach ($Notice in $Notices) {
    $NoticePath = Join-Path $OrtRoot $Notice.Source
    if (!(Test-Path -LiteralPath $NoticePath -PathType Leaf)) {
        throw "ONNX Runtime's $($Notice.Source) is missing from $OrtRoot. Run scripts\bootstrap-windows.ps1."
    }
}

# Check every required source before changing an existing installation.
foreach ($RelativePath in @(
    "LICENSE", "THIRD_PARTY.md", "data\locale\en-US.ini",
    "models\dpdfnet8_48khz_hr.onnx", "models\dpdfnet2_48khz_hr.onnx"
)) {
    if (!(Test-Path -LiteralPath (Join-Path $Root $RelativePath) -PathType Leaf)) {
        throw "Required install file is missing: $RelativePath"
    }
}
if (!(Test-Path -LiteralPath (Join-Path $Root "LICENSES") -PathType Container)) {
    throw "Required install directory is missing: LICENSES"
}
$Models = @(Get-ChildItem -LiteralPath (Join-Path $Root "models") -Filter "*.onnx" -File)

New-Item -ItemType Directory -Force -Path $BinDir, $DataDir | Out-Null
Copy-Item $Dll.FullName -Destination $BinDir -Force
Copy-Item -LiteralPath $SelectedOrtDll -Destination $BinDir -Force
Remove-Item (Join-Path $BinDir $ObsoleteOrtName) -Force -ErrorAction SilentlyContinue

$OrtProvidersDll = Join-Path $PluginBuildDir "onnxruntime_providers_shared.dll"
if (Test-Path -LiteralPath $OrtProvidersDll -PathType Leaf) {
    Copy-Item $OrtProvidersDll -Destination $BinDir -Force
}

Copy-Item (Join-Path $Root "data\*") -Destination $DataDir -Recurse -Force
Copy-Item (Join-Path $Root "LICENSE") -Destination $DataDir -Force
Copy-Item (Join-Path $Root "THIRD_PARTY.md") -Destination $DataDir -Force
Copy-Item (Join-Path $Root "LICENSES") -Destination $DataDir -Recurse -Force
foreach ($Notice in $Notices) {
    Copy-Item -LiteralPath (Join-Path $OrtRoot $Notice.Source) `
        -Destination (Join-Path $DataDir $Notice.Name) -Force
}

$ModelDir = Join-Path $DataDir "models"
New-Item -ItemType Directory -Force -Path $ModelDir | Out-Null
$Models | Copy-Item -Destination $ModelDir -Force

$ModelManifest = Join-Path $Root "models\manifest.json"
if (Test-Path $ModelManifest) {
    Copy-Item $ModelManifest -Destination $ModelDir -Force
}

Write-Host "Installed obs-dpdfnet to $PluginRoot"
Write-Host "Restart OBS and add DPDFNet Noise Suppression as an audio filter."
