# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Dry-run checks for tools/firmware.ps1; never builds, flashes or touches a port.
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot '..\firmware.ps1'
$failures = 0

function Invoke-Firmware {
    param([string[]]$Arguments, [string]$FakePorts)
    if ($null -eq $FakePorts) {
        Remove-Item Env:ESP32_KVM_FAKE_PORTS -ErrorAction SilentlyContinue
    } else {
        $env:ESP32_KVM_FAKE_PORTS = $FakePorts
    }
    $out = & pwsh -NoProfile -ExecutionPolicy Bypass -File $script @Arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
    Remove-Item Env:ESP32_KVM_FAKE_PORTS -ErrorAction SilentlyContinue
    return [pscustomobject]@{ Output = $out; Code = $code }
}

function Assert-That {
    param([bool]$Condition, [string]$Message)
    if ($Condition) { Write-Host "PASS: $Message" }
    else { Write-Host "FAIL: $Message"; $script:failures++ }
}

$eraseRegex = '(^|\s)(-e|--erase\S*|erase[-_]flash)(\s|$)'

$build = Invoke-Firmware -Arguments @('build', '-DryRun')
Assert-That ($build.Code -eq 0) 'build -DryRun exits 0'
Assert-That ($build.Output -match 'idf\.py -C firmware build') 'build -DryRun prints idf.py -C firmware build'
Assert-That ($build.Output -notmatch 'erase' -and $build.Output -notmatch $eraseRegex) 'build has no erase option'

$app = Invoke-Firmware -Arguments @('flash', '-DryRun', '-Port', 'COM9')
Assert-That ($app.Code -eq 0) 'flash -DryRun -Port COM9 exits 0'
Assert-That ($app.Output -match 'app-flash' -and $app.Output -match 'COM9') 'flash defaults to app-flash on COM9'
Assert-That ($app.Output -notmatch 'erase' -and $app.Output -notmatch $eraseRegex) 'app-flash has no erase option'

$full = Invoke-Firmware -Arguments @('flash', '-Full', '-DryRun', '-Port', 'COM9')
Assert-That ($full.Code -eq 0) 'flash -Full -DryRun exits 0'
Assert-That ($full.Output -match '\sflash(\s|$)' -and $full.Output -notmatch 'app-flash') 'flash -Full uses flash, not app-flash'
Assert-That ($full.Output -notmatch 'erase' -and $full.Output -notmatch $eraseRegex) 'full flash has no erase option'

$none = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts ''
Assert-That ($none.Code -ne 0) 'flash with no board exits non-zero'
Assert-That ($none.Output -match '303A:1001') 'no-board message names 303A:1001'

$many = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts 'COM7,COM8'
Assert-That ($many.Code -ne 0) 'flash with several boards and no -Port exits non-zero'
Assert-That ($many.Output -match 'COM7' -and $many.Output -match 'COM8') 'multiple-board message lists the ports'

$ports = Invoke-Firmware -Arguments @('ports', '-DryRun') -FakePorts 'COM9'
Assert-That ($ports.Code -eq 0) 'ports -DryRun exits 0'
Assert-That ($ports.Output -match 'COM9') 'ports -DryRun lists COM9'
Assert-That ($ports.Output -match 'not checked') 'ports -DryRun skips the busy probe'

$one = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts 'COM4'
Assert-That ($one.Code -eq 0) 'flash -DryRun with exactly one board and no -Port exits 0'
Assert-That ($one.Output -match '-p COM4(\s|$)') 'single board is auto-selected as -p COM4'
Assert-That ($one.Output -notmatch '-p C(\s|$)') 'single board name is not unrolled to its first character'

$onePorts = Invoke-Firmware -Arguments @('ports', '-DryRun') -FakePorts 'COM4'
Assert-That ($onePorts.Code -eq 0) 'ports -DryRun with one board exits 0'
Assert-That ($onePorts.Output -match '(?m)^COM4\s' -and $onePorts.Output -notmatch '(?m)^C\s') 'ports -DryRun lists COM4, not a lone C'

if ($failures -gt 0) { Write-Host "$failures assertion(s) failed"; exit 1 }
Write-Host 'All firmware script checks passed'
exit 0
