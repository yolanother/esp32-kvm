# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Fails when a product name ("kvm", any case) appears in the shipped part of the
# doubtech_hid or doubtech_hid_central components. Scanned: doubtech_hid include/,
# core/, ble/ (recursively), Kconfig, CMakeLists.txt, idf_component.yml; and every
# .c/.h/Kconfig/CMakeLists.txt/idf_component.yml of doubtech_hid_central, including
# subdirectories, except its tests/. Markdown is not scanned. input_hub is not scanned:
# it is firmware-internal and shares the kvm_* router types. Allowed: the legacy
# NVS namespace token kvm_bonds and the copyright line. Also fails on
# KVM-protocol phrases: protocol v1, USB worker, STATUS cache, FORGET_BOND (whole word; the
# private RPC_FORGET_BOND enumerator is not matched).
$ErrorActionPreference = 'Stop'
$component = Split-Path -Parent $PSScriptRoot
$central = (Resolve-Path (Join-Path $component '../doubtech_hid_central')).Path
$rootNames = 'Kconfig', 'CMakeLists.txt', 'idf_component.yml'

$files = @()
$targets = @('include', 'core', 'ble') | ForEach-Object { Join-Path $component $_ }
$files += @(Get-ChildItem -Path $targets -Recurse -File)
foreach ($name in $rootNames) {
    $path = Join-Path $component $name
    if (Test-Path $path) { $files += Get-Item $path }
}
$centralTests = Join-Path $central 'tests'
$files += @(Get-ChildItem -Path $central -File -Recurse | Where-Object {
        -not $_.FullName.StartsWith($centralTests, [StringComparison]::OrdinalIgnoreCase) -and
        ($_.Extension -in '.c', '.h' -or $_.Name -in $rootNames) })

$protocolPhrases = @('protocol v1', 'USB worker', 'STATUS cache', '\bFORGET_BOND\b')
$offenders = @()
foreach ($file in $files) {
    $lineNo = 0
    $relative = $file.FullName.Substring((Split-Path -Parent $component).Length + 1).Replace('\', '/')
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        $lineNo++
        $rest = $line -replace 'Copyright \(c\) ESP32 KVM contributors', '' -replace 'kvm_bonds', ''
        if ($rest -match '(?i)kvm') {
            $offenders += "${relative}:${lineNo}:$($line.Trim())"
        }
        foreach ($phrase in $protocolPhrases) {
            if ($line -match "(?i)$phrase") {
                $offenders += "${relative}:${lineNo}:$($line.Trim())"
                break
            }
        }
    }
}
if ($offenders.Count -gt 0) {
    Write-Host "FAIL: product names or KVM-protocol wording found in doubtech_hid or doubtech_hid_central:"
    $offenders | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host "OK: no product names or KVM-protocol wording in doubtech_hid and doubtech_hid_central ($($files.Count) files scanned)"
exit 0
