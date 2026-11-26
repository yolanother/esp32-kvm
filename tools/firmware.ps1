# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds and flashes the ESP32-S3 firmware with ESP-IDF; never erases flash.
#
# Usage: tools/firmware.ps1 <build|flash|ports|usb-serial> [-Port COMn] [-Full] [-DryRun]
#   flash runs app-flash by default; -Full runs flash (all images). No erase option is ever passed.
#   A board in USB composite mode is first asked to restart once in serial mode (serial-once magic),
#   then flashed exactly like a serial board. usb-serial writes the serial-mode magic, which
#   persists serial mode on the board.
# Warning: ports and the flash busy check open the port and may reset the board; BLE links drop if so.
# Test-only: ESP32_KVM_FAKE_PORTS (comma-separated, empty for none) replaces port detection. Each
#   entry is COMn or COMn:serial (303A:1001) or COMn:composite (allow-listed PID, interface MI_02).
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('build', 'flash', 'ports', 'usb-serial')]
    [string]$Action,
    [string]$Port,
    [switch]$Full,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$usbId = '303A:1001'
# Allow-listed composite product ids (VID 303A), matched with interface MI_02 (CDC control). This is
# the only place the list lives. It mirrors the interim CONFIG_KVM_USB_COMPOSITE_PID default (0x4009)
# in firmware/components/usb_device/Kconfig and spec Decision 13 in
# docs/superpowers/specs/2026-11-25-usb-hid-device-design.md.
$compositePids = @('4009')
$compositeIds = ($compositePids | ForEach-Object { '303A:' + $_ }) -join ', '
# Magic strings (spec Decision 9): 21 bytes each, with a leading 0 byte.
# serial-once: the board restarts in serial mode for one boot. serial-mode: persists serial mode.
$magicSerialOnce = [byte[]](@(0) + [Text.Encoding]::ASCII.GetBytes("KVM-USB-SERIAL-ONCE`n"))
$magicSerialMode = [byte[]](@(0) + [Text.Encoding]::ASCII.GetBytes("KVM-USB-SERIAL-MODE`n"))
$reenumerateSeconds = 15

function Stop-Script {
    param([string]$Message)
    [Console]::Error.WriteLine($Message)
    exit 1
}

function Find-IdfPath {
    $candidates = @($env:IDF_PATH, 'D:\esp\esp-idf-v5.5.1', (Join-Path $env:USERPROFILE 'esp\esp-idf-v5.5.1'))
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'export.ps1'))) { return $candidate }
    }
    Stop-Script 'ESP-IDF not found. Set IDF_PATH or install ESP-IDF v5.5.1 at D:\esp\esp-idf-v5.5.1.'
}

function New-Board {
    param([string]$Name, [string]$Mode)
    $id = if ($Mode -eq 'composite') { '303A:' + $compositePids[0] } else { $usbId }
    return [pscustomobject]@{ Port = $Name; Mode = $Mode; Id = $id }
}

function Get-BoardPorts {
    # Returns objects { Port; Mode; Id }: serial (303A:1001) or composite (allow-listed PID, MI_02).
    if ($null -ne $env:ESP32_KVM_FAKE_PORTS) {
        $fake = @()
        foreach ($entry in ($env:ESP32_KVM_FAKE_PORTS -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
            $name, $mode = $entry -split ':', 2
            $fake += New-Board $name $(if ($mode -eq 'composite') { 'composite' } else { 'serial' })
        }
        return $fake
    }
    $found = @()
    $devices = Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE '%VID_303A&PID_1001%'" -ErrorAction SilentlyContinue
    foreach ($device in $devices) {
        if ($device.Name -match '\((COM\d+)\)') { $found += New-Board $Matches[1] 'serial' }
    }
    foreach ($productId in $compositePids) {
        $devices = Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE '%VID_303A&PID_$productId&MI_02%'" -ErrorAction SilentlyContinue
        foreach ($device in $devices) {
            if ($device.Name -match '\((COM\d+)\)') { $found += New-Board $Matches[1] 'composite' }
        }
    }
    return @($found | Sort-Object Port -Unique)
}

function Test-PortBusy {
    param([string]$Name)
    $serial = New-Object System.IO.Ports.SerialPort $Name
    $serial.DtrEnable = $false
    $serial.RtsEnable = $false
    try {
        $serial.Open()
        return $false
    } catch {
        return $true
    } finally {
        if ($serial.IsOpen) { $serial.Close() }
        $serial.Dispose()
    }
}

function Format-Bytes {
    param([byte[]]$Bytes)
    return (($Bytes | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}

function Send-UsbMagic {
    # Writes one magic string to the board's port (115200 8N1, DTR and RTS low: the native USB
    # Serial/JTAG reads DTR/RTS transitions as reset commands). Dry run only prints the step.
    param([string]$Name, [byte[]]$Magic, [string]$Label)
    Write-Host ("write {0} magic to {1}: {2} bytes at 115200 8N1, DTR and RTS off: {3}" -f $Label, $Name, $Magic.Length, (Format-Bytes $Magic))
    if ($DryRun) { return }
    $serial = New-Object System.IO.Ports.SerialPort $Name, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $serial.DtrEnable = $false
    $serial.RtsEnable = $false
    try {
        $serial.Open()
        $serial.Write($Magic, 0, $Magic.Length)
        $serial.BaseStream.Flush()
        Start-Sleep -Milliseconds 200
    } finally {
        if ($serial.IsOpen) { $serial.Close() }
        $serial.Dispose()
    }
}

function Wait-SerialBoard {
    # After the serial-once restart the board re-enumerates as 303A:1001; the COM number can change.
    Write-Host "wait up to $reenumerateSeconds s for a serial board ($usbId) to re-enumerate"
    if ($DryRun) { return $null }
    $deadline = [DateTime]::UtcNow.AddSeconds($reenumerateSeconds)
    do {
        Start-Sleep -Milliseconds 500
        $serialBoards = @(Get-BoardPorts | Where-Object { $_.Mode -eq 'serial' })
        if ($serialBoards.Count -eq 1) { return $serialBoards[0].Port }
    } while ([DateTime]::UtcNow -lt $deadline)
    Stop-Script "No serial board ($usbId) appeared within $reenumerateSeconds s of the serial-once restart. Replug the board, or hold BOOT while plugging in for ROM download mode."
}

function Resolve-Board {
    # The board an action applies to. -Port picks one (a port not in the listing counts as serial).
    param([string]$Verb)
    $boards = @(Get-BoardPorts)
    if ($Port) {
        $match = @($boards | Where-Object { $_.Port -eq $Port })
        if ($match.Count -gt 0) { return $match[0] }
        return New-Board $Port 'serial'
    }
    if ($boards.Count -eq 0) {
        Stop-Script "No board with USB id $usbId or $compositeIds found. Connect the board or pass -Port COMn."
    }
    if ($boards.Count -gt 1) {
        $list = ($boards | ForEach-Object { '{0} ({1})' -f $_.Port, $_.Mode }) -join ', '
        Stop-Script "Several boards found: $list. Pass -Port COMn to $Verb one of them."
    }
    return $boards[0]
}

function Assert-PortFree {
    param([string]$Name)
    if (-not $DryRun -and (Test-PortBusy $Name)) {
        Stop-Script "$Name is busy. Quit the ESP32 KVM desktop app (it owns the port) and try again."
    }
}

function Invoke-Idf {
    param([string[]]$IdfArgs)
    $line = 'idf.py ' + ($IdfArgs -join ' ')
    if ($DryRun) {
        Write-Host $line
        return
    }
    Write-Host $line
    & idf.py @IdfArgs
    if ($LASTEXITCODE -ne 0) { Stop-Script "idf.py failed with exit code $LASTEXITCODE." }
}

function Get-IdfPythonScripts {
    # The ESP-IDF python env is preferred over whatever python is first on PATH
    # (a newer python has no matching IDF env and breaks export.ps1).
    # Test-only: ESP32_KVM_IDF_PYTHON_ROOT replaces ~/.espressif/python_env.
    $root = if ($env:ESP32_KVM_IDF_PYTHON_ROOT) { $env:ESP32_KVM_IDF_PYTHON_ROOT } else { Join-Path $env:USERPROFILE '.espressif\python_env' }
    $pyEnv = Get-ChildItem $root -Directory -Filter 'idf5.5*' -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'Scripts') } |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($pyEnv) { return (Join-Path $pyEnv.FullName 'Scripts') }
    return $null
}

function Enter-IdfEnvironment {
    $scripts = Get-IdfPythonScripts
    if ($DryRun) {
        if ($scripts) { Write-Host "python env first on PATH: $scripts" }
        return
    }
    $idf = Find-IdfPath
    $env:IDF_PATH = $idf
    if ($scripts) { $env:PATH = $scripts + [IO.Path]::PathSeparator + $env:PATH }
    . (Join-Path $idf 'export.ps1') | Out-Null
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        Stop-Script 'idf.py is not available after loading export.ps1.'
    }
}

Set-Location $repoRoot

switch ($Action) {
    'ports' {
        $boards = @(Get-BoardPorts)
        if ($boards.Count -eq 0) {
            Write-Host "No serial ports with USB id $usbId or $compositeIds found."
            exit 0
        }
        foreach ($board in $boards) {
            $state = if ($DryRun) { 'busy: not checked (dry run)' } elseif (Test-PortBusy $board.Port) { 'busy (cannot open exclusively)' } else { 'free (opens exclusively)' }
            Write-Host "$($board.Port)  $($board.Id)  $($board.Mode)  $state"
        }
    }
    'build' {
        Enter-IdfEnvironment
        if (-not (Test-Path 'firmware\sdkconfig')) {
            Invoke-Idf @('-C', 'firmware', 'set-target', 'esp32s3')
        }
        Invoke-Idf @('-C', 'firmware', 'build')
        if (-not $DryRun) {
            $image = Get-ChildItem 'firmware\build\*.bin' -Exclude 'bootloader.bin', 'partition-table.bin', 'ota_data_initial.bin' -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if (-not $image) { Stop-Script 'Build reported success but no app image was found in firmware\build.' }
            Write-Host ("App image: {0} ({1} bytes)" -f $image.FullName, $image.Length)
        }
    }
    'flash' {
        $board = Resolve-Board 'flash'
        $Port = $board.Port
        Assert-PortFree $Port
        if ($board.Mode -eq 'composite') {
            Send-UsbMagic $Port $magicSerialOnce 'serial-once'
            $reenumerated = Wait-SerialBoard
            if ($reenumerated) { $Port = $reenumerated }
        }
        Enter-IdfEnvironment
        $target = if ($Full) { 'flash' } else { 'app-flash' }
        Invoke-Idf @('-C', 'firmware', '-p', $Port, $target)
    }
    'usb-serial' {
        $board = Resolve-Board 'switch'
        Assert-PortFree $board.Port
        Send-UsbMagic $board.Port $magicSerialMode 'serial-mode'
    }
}
exit 0
