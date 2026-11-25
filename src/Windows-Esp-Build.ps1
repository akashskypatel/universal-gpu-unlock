# SPDX-License-Identifier: GPL-2.0-only
# Validate an exact Windows EFI System Partition identifier for the builders
# and the signing wrapper. Firmware still verifies the live partition identity.
function ConvertTo-GuWindowsEspGuid {
    param([string]$Value)
    $parsed = [guid]::Empty
    if (-not [guid]::TryParse($Value, [ref]$parsed) -or $parsed -eq [guid]::Empty) {
        throw 'Invalid or empty Windows ESP partition GUID.'
    }
    return $parsed
}

# EFI_GUID has three integer fields and an eight-byte array. Do not copy the
# textual GUID as sequential bytes into a little-endian firmware structure.
function New-GuWindowsEspGuidInitializer {
    param([guid]$Guid)
    $parts = $Guid.ToString('D').Split('-')
    $last = $parts[3] + $parts[4]
    $octets = @(0..7 | ForEach-Object { '0x' + $last.Substring(2*$_, 2) })
    return '{0x' + $parts[0] + ',0x' + $parts[1] + ',0x' + $parts[2] + ',{' + ($octets -join ',') + '}}'
}
