<#
.SYNOPSIS
  Installs steamvr-arc-dx12-bridge into a game folder.
.DESCRIPTION
  Finds every 64-bit openvr_api.dll under the game folder, renames it to openvr_api.orig.dll
  and puts the bridge in its place, together with openvr_proxy.ini.
  Running it again updates the bridge. If Steam has restored or updated the game's own
  openvr_api.dll in the meantime, that file becomes the new openvr_api.orig.dll.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1 "C:\Program Files (x86)\Steam\steamapps\common\Drums Rock"
#>
param([Parameter(Mandatory = $true)][string]$GameDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$bridge = Join-Path $root 'bin\openvr_api.dll'
$ini = Join-Path $root 'openvr_proxy.ini'
if (-not (Test-Path -LiteralPath $bridge)) { throw "Not found: $bridge" }

function Test-IsBridge($path) {
    [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)).Contains('steamvr-arc-dx12-bridge')
}
function Get-PeMachine($path) {
    $b = [IO.File]::ReadAllBytes($path)
    [BitConverter]::ToUInt16($b, [BitConverter]::ToInt32($b, 0x3C) + 4)
}

$found = @(Get-ChildItem -LiteralPath $GameDir -Recurse -Filter 'openvr_api.dll' -File -ErrorAction SilentlyContinue)
if ($found.Count -eq 0) { throw "No openvr_api.dll under '$GameDir': the game does not use OpenVR this way." }
foreach ($f in $found) {
    $dir = $f.DirectoryName
    $orig = Join-Path $dir 'openvr_api.orig.dll'
    if (Test-IsBridge $f.FullName) {
        Copy-Item -LiteralPath $bridge -Destination $f.FullName -Force
        "updated   $($f.FullName)"
        continue
    }
    if ((Get-PeMachine $f.FullName) -ne 0x8664) { "skipped   $($f.FullName) (32-bit, not supported)"; continue }
    Move-Item -LiteralPath $f.FullName -Destination $orig -Force
    Copy-Item -LiteralPath $bridge -Destination $f.FullName
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'openvr_proxy.ini'))) { Copy-Item -LiteralPath $ini -Destination $dir }
    "installed $($f.FullName)"
}
