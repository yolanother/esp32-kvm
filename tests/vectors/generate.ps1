# Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
# Generates independent version-one wire fixtures using bitwise CRC32C and COBS
# routines that do not call the Rust protocol implementation.

$ErrorActionPreference = 'Stop'

function Get-Crc32c([byte[]]$data) {
    [uint32]$crc = [uint32]::MaxValue
    foreach ($value in $data) {
        $crc = $crc -bxor [uint32]$value
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 1) -ne 0) { $crc = ($crc -shr 1) -bxor [Convert]::ToUInt32('82f63b78', 16) }
            else { $crc = $crc -shr 1 }
        }
    }
    return ($crc -bxor [uint32]::MaxValue)
}

function ConvertTo-Cobs([byte[]]$data) {
    $result = [System.Collections.Generic.List[byte]]::new()
    $result.Add(0)
    $codeIndex = 0
    [int]$code = 1
    foreach ($value in $data) {
        if ($value -eq 0) {
            $result[$codeIndex] = [byte]$code
            $codeIndex = $result.Count
            $result.Add(0)
            $code = 1
        } else {
            $result.Add($value)
            $code++
            if ($code -eq 255) {
                $result[$codeIndex] = 255
                $codeIndex = $result.Count
                $result.Add(0)
                $code = 1
            }
        }
    }
    $result[$codeIndex] = [byte]$code
    $result.Add(0)
    return ,$result.ToArray()
}

function New-Frame([byte]$kind, [byte[]]$payload, [uint64]$session = 5, [uint32]$seq = 1, [uint32]$generation = 2) {
    $body = [System.Collections.Generic.List[byte]]::new()
    $body.AddRange([byte[]](0x56, 0x4b, 1, $kind))
    $body.AddRange([BitConverter]::GetBytes([uint16]$payload.Length))
    $body.AddRange([byte[]](0, 0))
    $body.AddRange([BitConverter]::GetBytes($session))
    $body.AddRange([BitConverter]::GetBytes($seq))
    $body.AddRange([BitConverter]::GetBytes($generation))
    $body.AddRange($payload)
    $body.AddRange([BitConverter]::GetBytes((Get-Crc32c $body.ToArray())))
    return ,$body.ToArray()
}

$examples = @(
    @('hello-v1', 0x01, [byte[]](0,0,0,0,0,0), [uint64]0, [uint32]7, [uint32]0),
    @('caps', 0x02, [byte[]](0xa8,1,0x61,0x31,2,0x61,0x62,3,0,4,0,5,1,6,1,7,0,8,5), [uint64]5, [uint32]1, [uint32]2),
    @('session-open', 0x03, [byte[]](0xa2,1,0x61,0x31,2,0), [uint64]5, [uint32]1, [uint32]2),
    @('heartbeat', 0x10, [byte[]](42,0,0,0,0,0,0,0), [uint64]5, [uint32]1, [uint32]2),
    @('get-status', 0x11, [byte[]]@(), [uint64]5, [uint32]1, [uint32]2),
    @('status', 0x12, [byte[]](0xa5,1,0,2,0,3,0x80,4,0,5,2), [uint64]5, [uint32]1, [uint32]2),
    @('switch', 0x20, [byte[]](1,2,0,0,0,3,0,0,0), [uint64]5, [uint32]1, [uint32]2),
    @('release-all', 0x21, [byte[]]@(), [uint64]5, [uint32]1, [uint32]2),
    @('arm', 0x22, [byte[]](1,2,0,0,0), [uint64]5, [uint32]1, [uint32]2),
    @('key-state', 0x30, [byte[]](2,0,4,0,0,0,0,0), [uint64]5, [uint32]1, [uint32]2),
    @('pointer', 0x31, [byte[]](1,4,0,252,255,1,255), [uint64]5, [uint32]1, [uint32]2),
    @('consumer-state', 0x32, [byte[]](0,0), [uint64]5, [uint32]1, [uint32]2),
    @('pair-begin', 0x40, [byte[]](60,0), [uint64]5, [uint32]1, [uint32]2),
    @('pair-cancel', 0x41, [byte[]]@(), [uint64]5, [uint32]1, [uint32]2),
    @('forget-bond', 0x42, [byte[]](0xa1,1,0x50,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0), [uint64]5, [uint32]1, [uint32]2),
    @('pair-reply', 0x43, [byte[]](0xa3,1,1,2,0,3,0xf5), [uint64]5, [uint32]1, [uint32]2),
    @('device-select-request', 0x50, [byte[]](1,1,0,0,0), [uint64]5, [uint32]1, [uint32]2),
    @('update-prepare', 0x60, [byte[]]@(), [uint64]5, [uint32]1, [uint32]2),
    @('ack', 0x70, [byte[]](0x20,1,0,0,0,0,3,0,0,0), [uint64]5, [uint32]1, [uint32]2),
    @('nack', 0x71, [byte[]](0x20,1,0,0,0,4,2,0,0,0), [uint64]5, [uint32]1, [uint32]2),
    @('input-progress', 0x72, [byte[]](1,0,0,0,1,0,0,0), [uint64]5, [uint32]1, [uint32]2)
)

foreach ($entry in $examples) {
    $frame = New-Frame $entry[1] $entry[2] $entry[3] $entry[4] $entry[5]
    [IO.File]::WriteAllBytes((Join-Path $PSScriptRoot "$($entry[0]).cobs"), (ConvertTo-Cobs $frame))
}

$badLength = New-Frame 0x11 ([byte[]]@()) 5 3 2
$badLength[4] = 1
[IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'bad-length.cobs'), (ConvertTo-Cobs $badLength))
$badCrc = New-Frame 0x11 ([byte[]]@()) 5 3 2
$badCrc[$badCrc.Length - 1] = $badCrc[$badCrc.Length - 1] -bxor 1
[IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'bad-crc.cobs'), (ConvertTo-Cobs $badCrc))
