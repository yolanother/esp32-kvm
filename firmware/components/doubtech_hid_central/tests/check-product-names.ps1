# Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
# Fails when a product name ("kvm", any case) or KVM-protocol wording appears in the
# shipped files of the doubtech_hid_central component (every .c/.h in the
# component and its subdirectories, Kconfig, CMakeLists.txt, idf_component.yml;
# tests/ and Markdown are not scanned). The rule set is the one the doubtech_hid
# guard applies to this component too (doubtech_hid/tests/check-product-names.ps1).
# Allowed: the legacy NVS namespace token kvm_bonds and the copyright line.
$ErrorActionPreference = 'Stop'
$component = Split-Path -Parent $PSScriptRoot
$tests = Join-Path $component 'tests'
$files = @(Get-ChildItem -Path $component -File -Recurse | Where-Object {
        -not $_.FullName.StartsWith($tests, [StringComparison]::OrdinalIgnoreCase) -and
        ($_.Extension -in '.c', '.h' -or $_.Name -in 'Kconfig', 'CMakeLists.txt', 'idf_component.yml') })
$protocolPhrases = @('protocol v1', 'USB worker', 'STATUS cache', '\bFORGET_BOND\b')
$offenders = @()
foreach ($file in $files) {
    $lineNo = 0
    $relative = $file.FullName.Substring($component.Length + 1).Replace('\', '/')
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        $lineNo++
        $rest = $line -replace 'Copyright \(c\) ESP32 KVM contributors', '' -replace 'kvm_bonds', ''
        if ($rest -match '(?i)kvm') { $offenders += "${relative}:${lineNo}:$($line.Trim())"; continue }
        foreach ($phrase in $protocolPhrases) {
            if ($line -match "(?i)$phrase") { $offenders += "${relative}:${lineNo}:$($line.Trim())"; break }
        }
    }
}
if ($offenders.Count -gt 0) {
    Write-Host 'FAIL: product names or KVM-protocol wording found in doubtech_hid_central:'
    $offenders | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host "OK: no product names or KVM-protocol wording in doubtech_hid_central ($($files.Count) files scanned)"
exit 0
