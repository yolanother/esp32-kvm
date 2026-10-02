# Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
# Compiles and runs the USB-to-router integration test against portable C
# sources with MSVC, without an ESP-IDF install or attached hardware.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$msvcRoot = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC'
$kitRoot = 'C:/Program Files (x86)/Windows Kits/10'
$msvc = Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$kit = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if ($null -eq $msvc -or $null -eq $kit) { throw 'MSVC or Windows SDK missing' }
$output = Join-Path $env:TEMP 'esp32-kvm-integration-test'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$includes = @("/I$($msvc.FullName)/include", "/I$($kit.FullName)/ucrt", "/I$repo/firmware/components/router", "/I$repo/firmware/components/transport", "/I$repo/protocol/schema")
$libraries = @("/LIBPATH:$($msvc.FullName)/lib/x64", "/LIBPATH:$kitRoot/Lib/$($kit.Name)/um/x64", "/LIBPATH:$kitRoot/Lib/$($kit.Name)/ucrt/x64")
& (Join-Path $msvc.FullName 'bin/HostX64/x64/cl.exe') /nologo /W4 /WX /std:c11 @includes (Join-Path $PSScriptRoot 'test_transport_router.c') (Join-Path $repo 'firmware/components/transport/transport_core.c') (Join-Path $repo 'firmware/components/router/router.c') "/Fo:$output/" "/Fe:$output/test_transport_router.exe" /link @libraries
if ($LASTEXITCODE -ne 0) { throw 'Integration compile failed' }
& (Join-Path $output 'test_transport_router.exe')
if ($LASTEXITCODE -ne 0) { throw 'Integration test failed' }
Write-Output 'PASS transport-router integration'
& (Join-Path $msvc.FullName 'bin/HostX64/x64/cl.exe') /nologo /W4 /WX /std:c11 @includes (Join-Path $repo 'firmware/components/transport/tests/transport_core_test.c') (Join-Path $repo 'firmware/components/transport/transport_core.c') (Join-Path $repo 'firmware/components/router/router.c') "/Fo:$output/" "/Fe:$output/transport_core_test.exe" /link @libraries
if ($LASTEXITCODE -ne 0) { throw 'Legacy transport test compile failed' }
& (Join-Path $output 'transport_core_test.exe') (Join-Path $repo 'tests/vectors/hello-v1.cobs') (Join-Path $repo 'tests/vectors/session-open.cobs') (Join-Path $repo 'tests/vectors/get-status.cobs')
if ($LASTEXITCODE -ne 0) { throw 'Legacy transport test failed' }
Write-Output 'PASS legacy transport fixtures'
& (Join-Path $msvc.FullName 'bin/HostX64/x64/cl.exe') /nologo /W4 /WX /std:c11 "/I$repo/firmware/tests/integration/mocks" @includes (Join-Path $PSScriptRoot 'test_router_hid_bridge.c') (Join-Path $repo 'firmware/components/transport/router_hid_bridge.c') "/Fo:$output/" "/Fe:$output/test_router_hid_bridge.exe" /link @libraries
if ($LASTEXITCODE -ne 0) { throw 'HID bridge test compile failed' }
& (Join-Path $output 'test_router_hid_bridge.exe')
if ($LASTEXITCODE -ne 0) { throw 'HID bridge test failed' }
Write-Output 'PASS HID bridge adapter'
