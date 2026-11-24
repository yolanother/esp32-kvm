# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Fails when a product name ("kvm", any case) appears in the shipped part of the
# doubtech_hid component. tests/ and Markdown are not scanned. Allowed: the legacy
# NVS namespace token kvm_bonds and the copyright line. Also fails on
# KVM-protocol phrases: protocol v1, USB worker, STATUS cache, FORGET_BOND (whole word; the
# private RPC_FORGET_BOND enumerator is not matched).
$ErrorActionPreference = 'Stop'
$component = Split-Path -Parent $PSScriptRoot
$targets = @('include', 'core', 'ble') | ForEach-Object { Join-Path $component $_ }
$files = @(Get-ChildItem -Path $targets -Recurse -File)
foreach ($name in 'Kconfig', 'CMakeLists.txt', 'idf_component.yml') {
    $path = Join-Path $component $name
    if (Test-Path $path) { $files += Get-Item $path }
}
$protocolPhrases = @('protocol v1', 'USB worker', 'STATUS cache', '\bFORGET_BOND\b')
$offenders = @()
foreach ($file in $files) {
    $lineNo = 0
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        $lineNo++
        $rest = $line -replace 'Copyright \(c\) ESP32 KVM contributors', '' -replace 'kvm_bonds', ''
        if ($rest -match '(?i)kvm') {
            $relative = $file.FullName.Substring($component.Length + 1).Replace('\', '/')
            $offenders += "${relative}:${lineNo}:$($line.Trim())"
        }
        foreach ($phrase in $protocolPhrases) {
            if ($line -match "(?i)$phrase") {
                $relative = $file.FullName.Substring($component.Length + 1).Replace('\', '/')
                $offenders += "${relative}:${lineNo}:$($line.Trim())"
                break
            }
        }
    }
}
if ($offenders.Count -gt 0) {
    Write-Host "FAIL: product names or KVM-protocol wording found in doubtech_hid:"
    $offenders | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host "OK: no product names or KVM-protocol wording in doubtech_hid ($($files.Count) files scanned)"
exit 0
