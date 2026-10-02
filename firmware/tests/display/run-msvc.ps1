# Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
# Compiles the portable display/button model with MSVC and executes its
# deterministic checks without touching ESP-IDF, USB, or the physical board.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$msvcRoot = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC'
$kitRoot = 'C:/Program Files (x86)/Windows Kits/10'
$msvc = Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$kit = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if ($null -eq $msvc -or $null -eq $kit) { throw 'MSVC or Windows SDK missing' }
$out = Join-Path $env:TEMP 'esp32-kvm-display-test'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$includes = @("/I$($msvc.FullName)/include", "/I$($kit.FullName)/ucrt", "/I$repo/firmware/components/display")
$libs = @("/LIBPATH:$($msvc.FullName)/lib/x64", "/LIBPATH:$kitRoot/Lib/$($kit.Name)/um/x64", "/LIBPATH:$kitRoot/Lib/$($kit.Name)/ucrt/x64")
& (Join-Path $msvc.FullName 'bin/HostX64/x64/cl.exe') /nologo /W4 /WX /std:c11 @includes (Join-Path $PSScriptRoot 'test_display_model.c') (Join-Path $repo 'firmware/components/display/display_model.c') "/Fo:$out/" "/Fe:$out/test_display_model.exe" /link @libs
if ($LASTEXITCODE -ne 0) { throw 'Display model compile failed' }
& (Join-Path $out 'test_display_model.exe')
if ($LASTEXITCODE -ne 0) { throw 'Display model tests failed' }
Write-Output 'PASS display model'
