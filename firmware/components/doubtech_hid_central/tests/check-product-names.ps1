# Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
# Fails when a product name ("kvm", any case) appears in the shipped files of the
# doubtech_hid_central component (sources, headers including include/, Kconfig,
# CMakeLists.txt, idf_component.yml). tests/ and Markdown are not scanned. Allowed: the legacy
# NVS namespace token kvm_bonds and the copyright line.
$ErrorActionPreference = 'Stop'
$component = Split-Path -Parent $PSScriptRoot
$tests = Join-Path $component 'tests'
$files = @(Get-ChildItem -Path $component -File -Recurse | Where-Object {
        -not $_.FullName.StartsWith($tests, [StringComparison]::OrdinalIgnoreCase) -and
        ($_.Extension -in '.c', '.h' -or $_.Name -in 'Kconfig', 'CMakeLists.txt', 'idf_component.yml') })
$offenders = @()
foreach ($file in $files) {
    $lineNo = 0
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        $lineNo++
        $rest = $line -replace 'Copyright \(c\) ESP32 KVM contributors', '' -replace 'kvm_bonds', ''
        if ($rest -match '(?i)kvm') { $offenders += "$($file.Name):${lineNo}:$($line.Trim())" }
    }
}
if ($offenders.Count -gt 0) {
    Write-Host 'FAIL: product names found in doubtech_hid_central:'
    $offenders | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host "OK: no product names in doubtech_hid_central ($($files.Count) files scanned)"
exit 0
