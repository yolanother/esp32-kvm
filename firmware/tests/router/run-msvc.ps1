# Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
# Compiles and executes the portable routing actor tests with MSVC and Windows
# SDK headers, using a temporary output directory outside the source tree.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$msvcRoot = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC'
$kitRoot = 'C:/Program Files (x86)/Windows Kits/10'
$msvc = Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$kit = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if ($null -eq $msvc -or $null -eq $kit) { throw 'MSVC or Windows SDK is missing' }
$compiler = Join-Path $msvc.FullName 'bin/HostX64/x64/cl.exe'
$output = Join-Path $env:TEMP 'esp32-kvm-router-test'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$includes = @("/I$($msvc.FullName)/include", "/I$($kit.FullName)/ucrt", "/I$repo/firmware/components/router")
$libraries = @("/LIBPATH:$($msvc.FullName)/lib/x64", "/LIBPATH:$kitRoot/Lib/$($kit.Name)/um/x64", "/LIBPATH:$kitRoot/Lib/$($kit.Name)/ucrt/x64")
& $compiler /nologo /W4 /WX /std:c11 @includes (Join-Path $PSScriptRoot 'test_router.c') (Join-Path $repo 'firmware/components/router/router.c') "/Fo:$output/" "/Fe:$output/test_router.exe" /link @libraries
if ($LASTEXITCODE -ne 0) { throw 'Router test compile failed' }
& (Join-Path $output 'test_router.exe')
if ($LASTEXITCODE -ne 0) { throw 'Router tests failed' }
Write-Output 'PASS test_router'
