# Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
# Builds and runs the portable HID report and NimBLE-adapter contract tests on
# Windows with installed MSVC and Windows SDK headers, without device access.
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
$includes = @(
    "/I$($msvc.FullName)/include",
    "/I$($kit.FullName)/ucrt",
    "/I$repo/firmware/components/hid",
    "/I$repo/firmware/tests/hid/mocks"
)
$libraries = @(
    "/LIBPATH:$($msvc.FullName)/lib/x64",
    "/LIBPATH:$kitRoot/Lib/$($kit.Name)/um/x64",
    "/LIBPATH:$kitRoot/Lib/$($kit.Name)/ucrt/x64"
)
foreach ($name in @('test_hid_report', 'test_hid_gatt', 'test_hid_guest')) {
    $sources = @(
        (Join-Path $PSScriptRoot "$name.c"),
        (Join-Path $repo 'firmware/components/hid/hid_report.c')
    )
    if ($name -eq 'test_hid_gatt') {
        $sources += (Join-Path $repo 'firmware/components/hid/hid_gatt.c')
    }
    if ($name -eq 'test_hid_guest') {
        $sources += (Join-Path $repo 'firmware/components/hid/hid_guest.c')
    }
    & $compiler /nologo /W4 /WX /std:c11 @includes @sources "/Fo:$output/" "/Fe:$output/$name.exe" /link @libraries
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $name" }
    & (Join-Path $output "$name.exe")
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
    Write-Output "PASS $name"
}
