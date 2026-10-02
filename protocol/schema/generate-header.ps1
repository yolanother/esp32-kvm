# Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
# Regenerates the firmware C constants from the Rust protocol kind enum so host
# and device builds share one version-one wire vocabulary.

$ErrorActionPreference = 'Stop'
$source = Get-Content (Join-Path $PSScriptRoot '../../crates/protocol/src/lib.rs') -Raw
$enum = [regex]::Match($source, '(?s)pub enum MessageKind \{(.*?)\}').Groups[1].Value
$names = [regex]::Matches($enum, '([A-Za-z][A-Za-z0-9]*)\s*=\s*(0x[0-9a-f]+),')
if ($names.Count -ne 21) { throw "Expected 21 message kinds; found $($names.Count)" }
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.')
$lines.Add(' * Generated version-one USB protocol identifiers and envelope limits for')
$lines.Add(' * ESP-IDF firmware. Regenerate with protocol/schema/generate-header.ps1. */')
$lines.Add('#ifndef ESP32_KVM_PROTOCOL_V1_H')
$lines.Add('#define ESP32_KVM_PROTOCOL_V1_H')
$lines.Add('#define KVM_PROTOCOL_MAGIC 0x4B56u')
$lines.Add('#define KVM_PROTOCOL_MAJOR 1u')
$lines.Add('#define KVM_PROTOCOL_HEADER_LEN 24u')
$lines.Add('#define KVM_PROTOCOL_MAX_PAYLOAD 512u')
$lines.Add('#define KVM_PROTOCOL_MAX_FRAME 540u')
foreach ($match in $names) {
    $name = [regex]::Replace($match.Groups[1].Value, '([a-z0-9])([A-Z])', '$1_$2').ToUpperInvariant()
    $lines.Add("#define KVM_MSG_$name $($match.Groups[2].Value)u")
}
$lines.Add('#endif /* ESP32_KVM_PROTOCOL_V1_H */')
[IO.File]::WriteAllLines((Join-Path $PSScriptRoot 'protocol_v1.h'), $lines)
