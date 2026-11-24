# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Fails when a public name declared in include/doubtech_hid.h (every doubtech_hid_*
# function, type and callback, and every DOUBTECH_HID_* macro or enumerator) is
# missing from docs/Designs/doubtech-hid/api.md. Comments are ignored when
# collecting names. Prints the missing names and the number of names checked.
$ErrorActionPreference = 'Stop'
$component = Split-Path -Parent $PSScriptRoot
$repo = (Resolve-Path (Join-Path $component '../../..')).Path
$header = Join-Path $component 'include/doubtech_hid.h'
$doc = Join-Path $repo 'docs/Designs/doubtech-hid/api.md'
$source = [IO.File]::ReadAllText($header)
$code = [regex]::Replace($source, '(?s)/\*.*?\*/', ' ')
$code = [regex]::Replace($code, '(?m)//.*$', ' ')
# The include guard is not part of the API.
$names = [regex]::Matches($code, '\b(?:doubtech_hid_\w+|DOUBTECH_HID_\w+)\b') |
    ForEach-Object { $_.Value } | Where-Object { $_ -ne 'DOUBTECH_HID_H' } | Sort-Object -Unique
if ($names.Count -eq 0) {
    Write-Host "FAIL: no public names found in $header"
    exit 1
}
$text = if (Test-Path -LiteralPath $doc) { [IO.File]::ReadAllText($doc) } else { '' }
$missing = @($names | Where-Object { $text -notmatch ('(?<![A-Za-z0-9_])' + [regex]::Escape($_) + '(?![A-Za-z0-9_])') })
if ($missing.Count -gt 0) {
    Write-Host "FAIL: $($missing.Count) of $($names.Count) public names are missing from docs/Designs/doubtech-hid/api.md:"
    $missing | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "OK: all $($names.Count) public names are documented in docs/Designs/doubtech-hid/api.md"
exit 0
