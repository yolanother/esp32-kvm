# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds and flashes the ESP32-S3 firmware with ESP-IDF; never erases flash.
#
# Usage: tools/firmware.ps1 <build|flash|ports> [-Port COMn] [-Full] [-DryRun]
#   flash runs app-flash by default; -Full runs flash (all images). No erase option is ever passed.
# Warning: ports and the flash busy check open the port and may reset the board; BLE links drop if so.
# Test-only: ESP32_KVM_FAKE_PORTS (comma-separated port names, empty for none) replaces port detection.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('build', 'flash', 'ports')]
    [string]$Action,
    [string]$Port,
    [switch]$Full,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$usbId = '303A:1001'

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

function Get-BoardPorts {
    if ($null -ne $env:ESP32_KVM_FAKE_PORTS) {
        return @($env:ESP32_KVM_FAKE_PORTS -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    }
    $found = @()
    $devices = Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE '%VID_303A&PID_1001%'" -ErrorAction SilentlyContinue
    foreach ($device in $devices) {
        if ($device.Name -match '\((COM\d+)\)') { $found += $Matches[1] }
    }
    return @($found | Sort-Object -Unique)
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

function Enter-IdfEnvironment {
    if ($DryRun) { return }
    $idf = Find-IdfPath
    $env:IDF_PATH = $idf
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        $pyEnv = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\python_env') -Directory -Filter 'idf5.5*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($pyEnv) { $env:PATH = (Join-Path $pyEnv.FullName 'Scripts') + [IO.Path]::PathSeparator + $env:PATH }
    }
    . (Join-Path $idf 'export.ps1') | Out-Null
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        Stop-Script 'idf.py is not available after loading export.ps1.'
    }
}

Set-Location $repoRoot

switch ($Action) {
    'ports' {
        $ports = Get-BoardPorts
        if ($ports.Count -eq 0) {
            Write-Host "No serial ports with USB id $usbId found."
            exit 0
        }
        foreach ($name in $ports) {
            $state = if ($DryRun) { 'busy: not checked (dry run)' } elseif (Test-PortBusy $name) { 'busy (cannot open exclusively)' } else { 'free (opens exclusively)' }
            Write-Host "$name  $usbId  $state"
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
        if (-not $Port) {
            $ports = Get-BoardPorts
            if ($ports.Count -eq 0) {
                Stop-Script "No board with USB id $usbId found. Connect the board or pass -Port COMn."
            }
            if ($ports.Count -gt 1) {
                Stop-Script ("Several boards with USB id $usbId found: {0}. Pass -Port COMn." -f ($ports -join ', '))
            }
            $Port = $ports[0]
        }
        if (-not $DryRun -and (Test-PortBusy $Port)) {
            Stop-Script "$Port is busy. Quit the ESP32 KVM desktop app (it owns the port) and try again."
        }
        Enter-IdfEnvironment
        $target = if ($Full) { 'flash' } else { 'app-flash' }
        Invoke-Idf @('-C', 'firmware', '-p', $Port, $target)
    }
}
exit 0
