<#
.SYNOPSIS
  Removes steamvr-arc-dx12-bridge from a game folder and puts the original openvr_api.dll back.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1 "C:\Program Files (x86)\Steam\steamapps\common\Drums Rock"
#>
param([Parameter(Mandatory = $true)][string]$GameDir)
$ErrorActionPreference = 'Stop'

function Test-IsBridge($path) {
    [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)).Contains('steamvr-arc-dx12-bridge')
}

$found = @(Get-ChildItem -LiteralPath $GameDir -Recurse -Filter 'openvr_api.orig.dll' -File -ErrorAction SilentlyContinue)
if ($found.Count -eq 0) { 'Nothing to uninstall.'; return }
foreach ($o in $found) {
    $dir = $o.DirectoryName
    $cur = Join-Path $dir 'openvr_api.dll'
    if ((Test-Path -LiteralPath $cur) -and -not (Test-IsBridge $cur)) {
        # Steam has already put a genuine openvr_api.dll back; the saved copy is stale
        Remove-Item -LiteralPath $o.FullName -Force
    } else {
        Move-Item -LiteralPath $o.FullName -Destination $cur -Force
    }
    Remove-Item -LiteralPath (Join-Path $dir 'openvr_proxy.ini'), (Join-Path $dir 'openvr_proxy.log') -Force -ErrorAction SilentlyContinue
    "restored  $cur"
}
