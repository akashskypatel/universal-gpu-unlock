# SPDX-License-Identifier: GPL-2.0-only
# Canonical PCI location selection for diagnostic and active preparation EFIs.
# No board, model, subsystem, or slot is hardcoded.
function New-GuGpuSelectionSpec {
    param(
        [string]$SelectionBdf,
        [string]$SelectionBdfs,
        [Parameter(Mandatory=$true)][string]$OutputDirectory
    )
    if ($SelectionBdf -and $SelectionBdfs) {
        throw '-SelectionBdf and -SelectionBdfs cannot be combined.'
    }
    $requested = @()
    if ($SelectionBdf) { $requested = @($SelectionBdf) }
    elseif ($SelectionBdfs) {
        $requested = @($SelectionBdfs.Split(','))
        if ($requested.Count -lt 2 -or $requested.Count -gt 16) {
            throw '-SelectionBdfs requires 2-16 comma-separated PCI locations.'
        }
    }
    $canonical = @()
    $entries = @()
    foreach ($location in $requested) {
        $value = $location.Trim()
        if ($value -notmatch '^(?<seg>[0-9a-fA-F]{4}):(?<bus>[0-9a-fA-F]{2}):(?<dev>[0-9a-fA-F]{2})\.(?<fun>[0-7])$') {
            throw 'Each target must be PCI location SSSS:BB:DD.F.'
        }
        $segment = [Convert]::ToUInt32($Matches['seg'],16)
        $bus = [Convert]::ToUInt32($Matches['bus'],16)
        $device = [Convert]::ToUInt32($Matches['dev'],16)
        $function = [Convert]::ToUInt32($Matches['fun'],10)
        if ($device -gt 31) { throw 'PCI device number must be 00-1f.' }
        $key = '{0:X4}:{1:X2}:{2:X2}.{3}' -f $segment,$bus,$device,$function
        if ($canonical -contains $key) {
            throw 'Duplicate PCI target in selection sequence.'
        }
        $canonical += $key
        $entries += "{ $($segment)U, $($bus)U, $($device)U, $($function)U }"
    }
    $label = ''
    if ($canonical.Count) {
        $header = Join-Path $OutputDirectory 'gpu-selection.h'
        @(
            "#define GU_GPU_SELECTION_COUNT $($canonical.Count)U",
            "#define GU_GPU_SELECTION_LOCATIONS { $($entries -join ', ') }"
        ) | Set-Content -LiteralPath $header -Encoding ascii
        if ($canonical.Count -le 2) {
            $label = ($canonical | ForEach-Object { $_ -replace '[:.]','-' }) -join '--'
        } else {
            # Short, deterministic filesystem-safe label for larger layouts.
            $hash = [System.Security.Cryptography.SHA256]::Create()
            try {
                $bytes = [System.Text.Encoding]::ASCII.GetBytes(($canonical -join ','))
                $digest = $hash.ComputeHash($bytes)
            } finally { $hash.Dispose() }
            $shortHash = ([BitConverter]::ToString($digest)).Replace('-','').Substring(0,12)
            $label = "N$($canonical.Count)-$shortHash"
        }
    }
    return [pscustomobject]@{
        Count = $canonical.Count
        Locations = $canonical
        Label = $label
    }
}
