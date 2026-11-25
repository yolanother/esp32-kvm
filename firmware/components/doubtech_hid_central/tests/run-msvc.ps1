# Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
# Compiles and runs the portable HOGP parser tests with MSVC and Windows SDK
# headers, with AddressSanitizer when the toolchain ships it.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
$msvcRoot = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC'
$kitRoot = 'C:/Program Files (x86)/Windows Kits/10'
$msvc = Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$kit = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if ($null -eq $msvc -or $null -eq $kit) { throw 'MSVC or Windows SDK is missing' }
$binDir = Join-Path $msvc.FullName 'bin/HostX64/x64'
$compiler = Join-Path $binDir 'cl.exe'
$output = Join-Path $env:TEMP 'esp32-kvm-hid-central-test'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$includes = @("/I$($msvc.FullName)/include", "/I$($kit.FullName)/ucrt", "/I$repo/firmware/components/doubtech_hid_central")
$libraries = @("/LIBPATH:$($msvc.FullName)/lib/x64", "/LIBPATH:$kitRoot/Lib/$($kit.Name)/um/x64", "/LIBPATH:$kitRoot/Lib/$($kit.Name)/ucrt/x64")
[string[]]$sanitize = @()
if (Test-Path (Join-Path $binDir 'clang_rt.asan_dynamic-x86_64.dll')) {
    $sanitize = @('/fsanitize=address', '/Zi')
    $env:PATH = "$binDir;$env:PATH"
    Write-Output 'AddressSanitizer enabled'
}
$component = "$repo/firmware/components/doubtech_hid_central"
# The registry store tests reuse the NVS and error-code mocks of the HID library.
$mocks = "/I$repo/firmware/components/doubtech_hid/tests/mocks"
$suites = @(
    @{ Name = 'test_hogp_parse'; Sources = @('hogp_parse.c'); Extra = @() },
    @{ Name = 'test_central_registry'; Sources = @('central_registry.c', 'central_store.c'); Extra = @($mocks) }
)
foreach ($suite in $suites) {
    $name = $suite.Name
    $sources = @($suite.Sources | ForEach-Object { Join-Path $component $_ })
    & $compiler /nologo /W4 /WX /std:c11 @sanitize @includes @($suite.Extra) (Join-Path $PSScriptRoot "$name.c") @sources "/Fo:$output/" "/Fd:$output/" "/Fe:$output/$name.exe" /link @libraries
    if ($LASTEXITCODE -ne 0) { throw "$name compile failed" }
    & (Join-Path $output "$name.exe")
    if ($LASTEXITCODE -ne 0) { throw "$name failed" }
    Write-Output "PASS $name"
}
