# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Fails when a public name declared in a public header is missing from its API document:
#   include/doubtech_hid.h (every doubtech_hid_* function, type and callback, and every
#   DOUBTECH_HID_* macro or enumerator) -> docs/Designs/doubtech-hid/api.md
#   ../doubtech_hid_central/include/doubtech_hid_central.h (every doubtech_hid_central_*
#   function and type, DOUBTECH_HID_CENTRAL_* macro and DTHID_CENTRAL_* enumerator)
#   -> docs/Designs/doubtech-hid/central.md
# Comments are ignored when collecting names. Prints the missing names and the number of names checked.
$ErrorActionPreference = 'Stop'
$component = Split-Path -Parent $PSScriptRoot
$repo = (Resolve-Path (Join-Path $component '../../..')).Path
$checks = @(
    @{ Header = (Join-Path $component 'include/doubtech_hid.h'); Doc = 'docs/Designs/doubtech-hid/api.md';
       Pattern = '\b(?:doubtech_hid_\w+|DOUBTECH_HID_\w+)\b'; Guard = 'DOUBTECH_HID_H' },
    @{ Header = (Join-Path $component '../doubtech_hid_central/include/doubtech_hid_central.h'); Doc = 'docs/Designs/doubtech-hid/central.md';
       Pattern = '\b(?:doubtech_hid_central_\w+|DOUBTECH_HID_CENTRAL_\w+|DTHID_CENTRAL_\w+)\b'; Guard = 'DOUBTECH_HID_CENTRAL_H' }
)
$failed = $false
foreach ($check in $checks) {
    $doc = Join-Path $repo $check.Doc
    $source = [IO.File]::ReadAllText($check.Header)
    $code = [regex]::Replace($source, '(?s)/\*.*?\*/', ' ')
    $code = [regex]::Replace($code, '(?m)//.*$', ' ')
    # The include guard is not part of the API.
    $names = [regex]::Matches($code, $check.Pattern) |
        ForEach-Object { $_.Value } | Where-Object { $_ -ne $check.Guard } | Sort-Object -Unique
    if ($names.Count -eq 0) {
        Write-Host "FAIL: no public names found in $($check.Header)"
        exit 1
    }
    $text = if (Test-Path -LiteralPath $doc) { [IO.File]::ReadAllText($doc) } else { '' }
    $missing = @($names | Where-Object { $text -notmatch ('(?<![A-Za-z0-9_])' + [regex]::Escape($_) + '(?![A-Za-z0-9_])') })
    if ($missing.Count -gt 0) {
        Write-Host "FAIL: $($missing.Count) of $($names.Count) public names are missing from $($check.Doc):"
        $missing | ForEach-Object { Write-Host "  $_" }
        $failed = $true
    } else {
        Write-Host "OK: all $($names.Count) public names are documented in $($check.Doc)"
    }
}
if ($failed) { exit 1 }
exit 0
