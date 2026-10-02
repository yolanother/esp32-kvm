# Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
# Builds and runs portable pairing policy tests with MSVC without BLE hardware.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$msvcRoot = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC'
$kitRoot = 'C:/Program Files (x86)/Windows Kits/10'
$msvc = Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$kit = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if ($null -eq $msvc -or $null -eq $kit) { throw 'MSVC or Windows SDK is missing' }
$compiler = Join-Path $msvc.FullName 'bin/HostX64/x64/cl.exe'
$output = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach ($name in @('test_pairing', 'test_pairing_store')) {
    $sources = @((Join-Path $PSScriptRoot "$name.c"), (Join-Path $repo 'firmware/components/hid/hid_pairing.c'))
    if ($name -eq 'test_pairing_store') { $sources += (Join-Path $repo 'firmware/components/hid/hid_pairing_store.c') }
    & $compiler /nologo /W4 /WX /std:c11 "/I$($msvc.FullName)/include" "/I$($kit.FullName)/ucrt" "/I$repo/firmware/components/hid" "/I$repo/firmware/tests/hid/mocks" @sources "/Fo:$output/" "/Fe:$output/$name.exe" /link "/LIBPATH:$($msvc.FullName)/lib/x64" "/LIBPATH:$kitRoot/Lib/$($kit.Name)/um/x64" "/LIBPATH:$kitRoot/Lib/$($kit.Name)/ucrt/x64"
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $name" }
    & (Join-Path $output "$name.exe")
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
    Write-Output "PASS $name"
}
