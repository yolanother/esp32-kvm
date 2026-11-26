# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Dry-run checks for tools/firmware.ps1; never builds, flashes or touches a port.
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot '..\firmware.ps1'
$failures = 0

function Invoke-Firmware {
    param([string[]]$Arguments, [string]$FakePorts, [string]$FakePortsAfter)
    if ($null -eq $FakePorts) {
        Remove-Item Env:ESP32_KVM_FAKE_PORTS -ErrorAction SilentlyContinue
    } else {
        $env:ESP32_KVM_FAKE_PORTS = $FakePorts
    }
    if ($PSBoundParameters.ContainsKey("FakePortsAfter")) { $env:ESP32_KVM_FAKE_PORTS_AFTER = $FakePortsAfter } else { Remove-Item Env:ESP32_KVM_FAKE_PORTS_AFTER -ErrorAction SilentlyContinue }
    $out = & pwsh -NoProfile -ExecutionPolicy Bypass -File $script @Arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
    Remove-Item Env:ESP32_KVM_FAKE_PORTS, Env:ESP32_KVM_FAKE_PORTS_AFTER -ErrorAction SilentlyContinue
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

$pyRoot = Join-Path ([IO.Path]::GetTempPath()) ("kvm-pyenv-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force (Join-Path $pyRoot 'idf5.5_py3.12_env\Scripts') | Out-Null
$env:ESP32_KVM_IDF_PYTHON_ROOT = $pyRoot
$pyBuild = Invoke-Firmware -Arguments @('build', '-DryRun')
Assert-That ($pyBuild.Code -eq 0 -and $pyBuild.Output -match [regex]::Escape('python env first on PATH: ' + (Join-Path $pyRoot 'idf5.5_py3.12_env\Scripts'))) 'build prefers the ESP-IDF python env over PATH python'
$env:ESP32_KVM_IDF_PYTHON_ROOT = Join-Path $pyRoot 'missing'
$noPy = Invoke-Firmware -Arguments @('build', '-DryRun')
Assert-That ($noPy.Code -eq 0 -and $noPy.Output -notmatch 'python env first') 'build -DryRun without an IDF python env does not reorder PATH'
Remove-Item Env:ESP32_KVM_IDF_PYTHON_ROOT -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $pyRoot -ErrorAction SilentlyContinue

# Composite board (spec Decisions 9 and 12): serial-once handoff before app-flash.
$onceBytes = '00 4B 56 4D 2D 55 53 42 2D 53 45 52 49 41 4C 2D 4F 4E 43 45 0A'
$modeBytes = '00 4B 56 4D 2D 55 53 42 2D 53 45 52 49 41 4C 2D 4D 4F 44 45 0A'

$cports = Invoke-Firmware -Arguments @('ports', '-DryRun') -FakePorts 'COM5:composite,COM4:serial'
Assert-That ($cports.Code -eq 0) 'ports -DryRun with mixed modes exits 0'
Assert-That ($cports.Output -match '(?m)^COM5\s+303A:4009\s+composite\b') 'ports lists COM5 as composite with its PID'
Assert-That ($cports.Output -match '(?m)^COM4\s+303A:1001\s+serial\b') 'ports lists COM4 as serial'
$plain = Invoke-Firmware -Arguments @('ports', '-DryRun') -FakePorts 'COM9'
Assert-That ($plain.Output -match '(?m)^COM9\s+303A:1001\s+serial\b') 'a bare fake port name is a serial board'

$comp = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts 'COM5:composite'
Assert-That ($comp.Code -eq 0) 'flash -DryRun with only a composite board exits 0'
Assert-That ($comp.Output -match 'serial-once' -and $comp.Output -match [regex]::Escape($onceBytes) -and $comp.Output -match '21 bytes') 'composite flash prints the 21-byte serial-once magic write'
Assert-That ($comp.Output -match 'COM5' -and $comp.Output -match '115200') 'magic write names the port and 115200'
Assert-That ($comp.Output -match '(?i)wait.*15 s') 'composite flash prints the 15 s wait for a serial board'
Assert-That ($comp.Output -match 'idf\.py -C firmware -p COM5 app-flash') 'composite flash then runs the unchanged app-flash line'
Assert-That ($comp.Output -notmatch 'erase' -and $comp.Output -notmatch $eraseRegex) 'composite flash has no erase option'
$compOrder = ($comp.Output.IndexOf('serial-once') -lt $comp.Output.IndexOf('app-flash'))
Assert-That $compOrder 'the magic write is printed before app-flash'

$compFull = Invoke-Firmware -Arguments @('flash', '-Full', '-DryRun') -FakePorts 'COM5:composite'
Assert-That ($compFull.Code -eq 0 -and $compFull.Output -match '\sflash(\s|$)' -and $compFull.Output -notmatch 'app-flash' -and $compFull.Output -notmatch 'erase') 'composite -Full keeps the flash target and never erases'

$serialOnly = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts 'COM4:serial'
Assert-That ($serialOnly.Output -notmatch 'serial-once' -and $serialOnly.Output -match 'app-flash') 'a serial board gets no magic write'

$twoComp = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts 'COM5:composite,COM6:composite'
Assert-That ($twoComp.Code -ne 0 -and $twoComp.Output -match 'COM5' -and $twoComp.Output -match 'COM6' -and $twoComp.Output -match '-Port') 'two composite boards without -Port refuse and name the ports'
Assert-That ($twoComp.Output -notmatch 'app-flash') 'two composite boards print no flash line'
$mixed = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts 'COM4:serial,COM5:composite'
Assert-That ($mixed.Code -ne 0 -and $mixed.Output -match 'COM4' -and $mixed.Output -match 'COM5') 'serial plus composite without -Port refuses'
$mixedPort = Invoke-Firmware -Arguments @('flash', '-DryRun', '-Port', 'COM5') -FakePorts 'COM4:serial,COM5:composite'
Assert-That ($mixedPort.Code -ne 0 -and $mixedPort.Output -notmatch "app-flash") 'explicit -Port on the composite board refuses while another board is present'
$mixedSerialPort = Invoke-Firmware -Arguments @('flash', '-DryRun', '-Port', 'COM4') -FakePorts 'COM4:serial,COM5:composite'
Assert-That ($mixedSerialPort.Code -eq 0 -and $mixedSerialPort.Output -notmatch 'serial-once' -and $mixedSerialPort.Output -match '-p COM4 app-flash') 'explicit -Port on the serial board flashes directly'

$usb = Invoke-Firmware -Arguments @('usb-serial', '-DryRun') -FakePorts 'COM5:composite'
Assert-That ($usb.Code -eq 0 -and $usb.Output -match 'serial-mode' -and $usb.Output -match [regex]::Escape($modeBytes) -and $usb.Output -match '21 bytes') 'usb-serial -DryRun prints the 21-byte serial-mode magic write'
Assert-That ($usb.Output -notmatch 'idf\.py' -and $usb.Output -notmatch 'erase') 'usb-serial runs no idf.py and no erase'
$usbTwo = Invoke-Firmware -Arguments @('usb-serial', '-DryRun') -FakePorts 'COM5:composite,COM6:composite'
Assert-That ($usbTwo.Code -ne 0 -and $usbTwo.Output -match '-Port') 'usb-serial with several boards and no -Port refuses'
$usbNone = Invoke-Firmware -Arguments @('usb-serial', '-DryRun') -FakePorts ''
Assert-That ($usbNone.Code -ne 0) 'usb-serial with no board exits non-zero'
$usbPort = Invoke-Firmware -Arguments @('usb-serial', '-DryRun', '-Port', 'COM5') -FakePorts 'COM4:serial,COM5:composite'
Assert-That ($usbPort.Code -eq 0 -and $usbPort.Output -match 'COM5') 'usb-serial -Port picks the named board'

$noneMsg = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts ''
Assert-That ($noneMsg.Output -match '303A:4009') 'no-board message also names the composite id'



# Fix round 1: handoff must flash only the NEW serial board; strict fake entries; serial-once action.
$handoff = Invoke-Firmware -Arguments @('flash', '-DryRun', '-Port', 'COM5') -FakePorts 'COM5:composite' -FakePortsAfter 'COM6:serial'
Assert-That ($handoff.Code -eq 0 -and $handoff.Output -match 'idf\.py -C firmware -p COM6 app-flash' -and $handoff.Output -notmatch '-p COM5 app-flash') 'handoff flashes the re-enumerated port (COM6), not the composite port'
$handoffSame = Invoke-Firmware -Arguments @('flash', '-DryRun') -FakePorts 'COM5:composite' -FakePortsAfter 'COM5:serial'
Assert-That ($handoffSame.Code -eq 0 -and $handoffSame.Output -match '-p COM5 app-flash') 'a board re-enumerating on the same COM number is flashed there'
$other = Invoke-Firmware -Arguments @('flash', '-DryRun', '-Port', 'COM5') -FakePorts 'COM4:serial,COM5:composite' -FakePortsAfter 'COM4:serial'
Assert-That ($other.Code -ne 0 -and $other.Output -match 'COM4' -and $other.Output -notmatch 'app-flash' -and $other.Output -notmatch 'serial-once magic') 'composite plus another serial board refuses the handoff and prints no write'
$otherComp = Invoke-Firmware -Arguments @('flash', '-DryRun', '-Port', 'COM5') -FakePorts 'COM5:composite,COM6:composite'
Assert-That ($otherComp.Code -ne 0 -and $otherComp.Output -notmatch 'app-flash') 'composite plus another composite board refuses the handoff'
$onlyOld = Invoke-Firmware -Arguments @('flash', '-DryRun', '-Port', 'COM5') -FakePorts 'COM5:composite' -FakePortsAfter 'COM5:composite'
Assert-That ($onlyOld.Code -ne 0 -and $onlyOld.Output -notmatch 'app-flash') 'no new serial board after the write refuses to flash'

foreach ($bad in @(':composite', 'COM5:bogus', 'COM5:composite,:serial')) {
    $badRun = Invoke-Firmware -Arguments @('ports', '-DryRun') -FakePorts $bad
    Assert-That ($badRun.Code -ne 0 -and $badRun.Output -match 'ESP32_KVM_FAKE_PORTS') "malformed fake entry '$bad' is rejected"
}

$unlisted = Invoke-Firmware -Arguments @('usb-serial', '-DryRun', '-Port', 'COM9') -FakePorts 'COM5:composite'
Assert-That ($unlisted.Code -ne 0 -and $unlisted.Output -notmatch 'write serial-mode') 'usb-serial refuses a port that is not a listed project board'
$usbOnSerial = Invoke-Firmware -Arguments @('usb-serial', '-DryRun') -FakePorts 'COM4:serial'
Assert-That ($usbOnSerial.Code -eq 0 -and $usbOnSerial.Output -match 'reboots') 'usb-serial on a serial board says it persists serial mode and reboots'

$once = Invoke-Firmware -Arguments @('serial-once', '-DryRun') -FakePorts 'COM5:composite'
Assert-That ($once.Code -eq 0 -and $once.Output -match [regex]::Escape($onceBytes) -and $once.Output -match '21 bytes' -and $once.Output -notmatch 'idf\.py' -and $once.Output -notmatch 'erase') 'serial-once -DryRun prints the 21-byte serial-once write and no idf.py'
$onceUnlisted = Invoke-Firmware -Arguments @('serial-once', '-DryRun', '-Port', 'COM9') -FakePorts 'COM5:composite'
Assert-That ($onceUnlisted.Code -ne 0) 'serial-once refuses an unlisted port'
$onceTwo = Invoke-Firmware -Arguments @('serial-once', '-DryRun') -FakePorts 'COM5:composite,COM6:composite'
Assert-That ($onceTwo.Code -ne 0 -and $onceTwo.Output -match '-Port') 'serial-once with several boards and no -Port refuses'

if ($failures -gt 0) { Write-Host "$failures assertion(s) failed"; exit 1 }
Write-Host 'All firmware script checks passed'
exit 0
