param(
    [string]$VcVars,
    [string]$SelectionBdf,
    [string]$SelectionBdfs,
    [switch]$EnableSingleTargetExecution,
    [switch]$SyntheticCompileProfile,
    [switch]$AcceptUnverifiedActuation,
    [switch]$StopReleaseOnlyBench,
    [switch]$RuntimeSelection,
    [switch]$RuntimeEsp,
    [switch]$EspResident
)
$ErrorActionPreference = 'Stop'
if ($StopReleaseOnlyBench -and -not $SelectionBdf -and -not $SelectionBdfs) {
    # No explicit BDFs means the default live-inventory select-all policy.
    $RuntimeSelection = $true
}
# The experimental original NVIDIA Stop and stock recovery baseline passed.
# The owner accepts cold-power recovery risk. Production hardware ownership
# and signed firmware acceptance remain unverified. Default outputs are
# compile-only; the waived StopReleaseOnlyBench profile defaults to runtime
# enumeration, with optional -SelectionBdfs for a specific ordered sequence.
if ($AcceptUnverifiedActuation -and -not $EnableSingleTargetExecution) {
    throw '-AcceptUnverifiedActuation requires -EnableSingleTargetExecution: the waiver only applies to ordered active execution builds'
}
if ($AcceptUnverifiedActuation -and $SyntheticCompileProfile) {
    throw '-AcceptUnverifiedActuation cannot be combined with -SyntheticCompileProfile: a compile-only image never actuates'
}
if ($EnableSingleTargetExecution -and -not $SyntheticCompileProfile -and -not $AcceptUnverifiedActuation) {
    throw 'Live GPU execution build refused: production hardware ownership and signed firmware acceptance remain unverified. Use -AcceptUnverifiedActuation for an explicitly unverified image, or -SyntheticCompileProfile only for compile validation.'
}
if ($EspResident -and -not $RuntimeEsp) {
    throw '-EspResident requires -RuntimeEsp: a compiled partition GUID would tie the installed copy to one machine'
}
if ($EspResident -and $SyntheticCompileProfile) {
    throw '-EspResident cannot be combined with -SyntheticCompileProfile: a compile-only image never opens the ESP'
}
if ($RuntimeEsp -and $SyntheticCompileProfile) {
    throw '-RuntimeEsp cannot be combined with -SyntheticCompileProfile: a compile-only image never opens the ESP'
}
if ($RuntimeSelection -and $SyntheticCompileProfile) {
    throw '-RuntimeSelection cannot be combined with -SyntheticCompileProfile: a compile-only image has no operator policy to read'
}
if ($RuntimeSelection -and ($SelectionBdf -or $SelectionBdfs)) {
    throw '-RuntimeSelection replaces the compiled target list; do not also pass -SelectionBdf or -SelectionBdfs'
}
if (-not $RuntimeSelection -and -not $SelectionBdf -and -not $SelectionBdfs) {
    throw 'Active firmware build refused: supply -SelectionBdf or -SelectionBdfs, or -RuntimeSelection for boot-time target selection. Only preparation-only artifacts are supported.'
}
if (-not $VcVars) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $candidate = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) { $VcVars = $candidate }
        }
    }
}
if (-not $VcVars) {
    $defaultPaths = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    )
    foreach ($p in $defaultPaths) {
        if (Test-Path -LiteralPath $p) { $VcVars = $p; break }
    }
}
if (-not $VcVars -or -not (Test-Path -LiteralPath $VcVars)) {
    throw "Visual Studio x64 Build Tools not found."
}

Push-Location -LiteralPath $PSScriptRoot
try {
    $buildDir = Join-Path $PSScriptRoot 'build_dxe'
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
    . (Join-Path $PSScriptRoot 'Gpu-Selection-Build.ps1')
    $selection = New-GuGpuSelectionSpec -SelectionBdf $SelectionBdf -SelectionBdfs $SelectionBdfs -OutputDirectory $buildDir
    if (-not $RuntimeSelection -and $EnableSingleTargetExecution -and $selection.Count -ne 1 -and -not $AcceptUnverifiedActuation) {
        throw "Single-target execution requires exactly one explicit -SelectionBdf"
    }
    if ($AcceptUnverifiedActuation -and -not $RuntimeSelection -and $selection.Count -lt 1) {
        throw '-AcceptUnverifiedActuation requires at least one explicit -SelectionBdf or -SelectionBdfs entry'
    }
    if ($StopReleaseOnlyBench) {
        if (-not $AcceptUnverifiedActuation) {
            throw '-StopReleaseOnlyBench requires -AcceptUnverifiedActuation.'
        }
        if (-not $RuntimeSelection -and $selection.Count -lt 1) {
            throw '-StopReleaseOnlyBench with compiled targets requires at least one BDF; omit BDFs for live inventory selection.'
        }
    }

    $sources = @(
        'dxe_main.c',
        'boot_io.c',
        'msi_release.c',
        'stop_contract.c',
        'launch_source.c',
        'unlock_execution.c',
        'port.c',
        'compat.c',
        'checks.c',
        'gpu_inventory.c',
        'gpu_discovery.c',
        'gpu_selection.c',
        'boot_state.c',
        'driver_nvram.c',
        'asset_install.c',
        'gpu_execution_state.c',
        'gpu_windows_path.c',
        'profile.c',
        'prom_reference.c',
        'gpu_sku_geometry.c',
        'sha256.c',
        'runtime.c',
        'format.c',
        'vendor\unlock_610.c',
        'vendor\prep_610.c',
        'vendor\layout_610.c',
        'vendor\vbios_610.c',
        'vendor\gfw_eval.c'
    )
    if ($StopReleaseOnlyBench) {
        # The release-only image must not even link the firmware-unlock
        # implementation; unlock_execution.c is compiled to the dedicated
        # Stop/release/reconnect runner under GU_STOP_RELEASE_ONLY_BENCH.
        $sources = @($sources | Where-Object { $_ -ne 'vendor\unlock_610.c' })
    }

    $objFiles = @()
    $compileCmds = @(
        '@echo off',
        "call `"$VcVars`" >nul",
        'if errorlevel 1 exit /b 1',
        "cd /d `"$PSScriptRoot`""
    )

    $clFlags = '/nologo /O2 /Oi /GS- /Gs65536 /Zl /c /TC /D__KERNEL__ /DGU_UEFI_PORT /DGU_AD104_TRANSACTION /DGU_ENTRY_LATCHED_SS1_FULL /D_CRT_SECURE_NO_WARNINGS /I. /Ibuild_dxe /Icompat /Ivendor /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'

    if ($selection.Count -or $RuntimeSelection) {
        if ($AcceptUnverifiedActuation) {
            $clFlags += ' /DGU_ACTIVE_SINGLE_TARGET /DGU_ACTIVE_UNVERIFIED_ACTUATION'
        } elseif ($SyntheticCompileProfile) {
            # The selected execution source can compile for any valid order.
            # Both EFI entrypoints return before any firmware or GPU operation.
            $clFlags += ' /DGU_ACTIVE_SINGLE_TARGET /DGU_HOST_TEST_PROFILE /DGU_SYNTHETIC_COMPILE_ONLY'
        } else {
            $clFlags += ' /DGU_ACTIVE_SEQUENCE_PREPARE_ONLY /DGU_HOST_TEST_PROFILE'
        }
    }

    if ($StopReleaseOnlyBench) {
        # Explicit Stop/release-only image: keep file logging, waive ASPM admission
        # only, and compile out the unlock path; AMD console checks stay active.
        $clFlags += ' /DGU_UNVERIFIED_BENCH_CONDITIONS /DGU_DXE_FILE_ONLY_OUTPUT /DGU_STOP_RELEASE_ONLY_BENCH'
    }

    if ($EspResident) {
        # Run from the Windows ESP itself; every launch gate is checked at boot.
        $clFlags += ' /DGU_ESP_RESIDENT'
    }

    if ($RuntimeEsp) {
        # Identify the Windows ESP from the live device tree instead of a
        # compiled partition GUID. The strict single-ESP refusal is unchanged.
        $clFlags += ' /DGU_RUNTIME_ESP'
    }

    if ($RuntimeSelection) {
        # Targets come from the "GuTargets" UEFI variable at boot, so the count
        # is unknown at build time. The runtime admission gates are unchanged.
        $clFlags += ' /DGU_RUNTIME_SELECTION'
    }

    foreach ($src in $sources) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($src)
        if ($src -like 'vendor\*') { $base = 'vendor_' + $base }
        $obj = "build_dxe\$base.obj"
        $objFiles += $obj
        $flags = $clFlags
        if ($src -eq 'runtime.c') {
            $flags = $clFlags.Replace('/Oi', '/Oi-')
        }
        $compileCmds += "cl $flags /Fobuild_dxe\$base.obj $src"
        $compileCmds += 'if errorlevel 1 exit /b 1'
    }

    $linkObjs = $objFiles -join ' '
    $binaryName = if ($StopReleaseOnlyBench -and $RuntimeSelection) { 'GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-DXE.efi' } elseif ($StopReleaseOnlyBench) { 'GPU-SEQUENCE-STOP-RELEASE-BENCH-DXE-' + $selection.Label + '.efi' } elseif ($RuntimeSelection -and $AcceptUnverifiedActuation) { 'GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION-DXE.efi' } elseif ($RuntimeSelection) { 'GPU-RUNTIME-SELECTION-PREP-DXE.efi' } elseif ($AcceptUnverifiedActuation -and $selection.Count -gt 1) { 'GPU-SEQUENCE-UNVERIFIED-ACTUATION-DXE-' + $selection.Label + '.efi' } elseif ($AcceptUnverifiedActuation) { 'GPU-ONE-TARGET-UNVERIFIED-ACTUATION-DXE-' + $selection.Label + '.efi' } elseif ($SyntheticCompileProfile -and $selection.Count -gt 1) { 'GPU-SEQUENCE-COMPILE-ONLY-DXE-' + $selection.Label + '.efi' } elseif ($SyntheticCompileProfile) { 'GPU-ONE-TARGET-COMPILE-ONLY-DXE-' + $selection.Label + '.efi' } elseif ($EnableSingleTargetExecution) { 'GPU-ONE-TARGET-EXPERIMENTAL-DXE-' + $selection.Label + '.efi' } elseif ($selection.Count) { 'GPU-ACTIVE-PREP-DXE-' + $selection.Label + '.efi' } else { 'AdaUnlockDxe.efi' }
    $compileCmds += "link /nologo /subsystem:efi_boot_service_driver /entry:AdaUnlockDriverEntryPoint /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /out:build_dxe\$binaryName $linkObjs"
    $compileCmds += 'if errorlevel 1 exit /b 1'

    $batchFile = Join-Path $buildDir 'build-dxe.cmd'
    Set-Content -LiteralPath $batchFile -Value $compileCmds -Encoding ascii

    & $env:ComSpec /d /c $batchFile
    if ($LASTEXITCODE -ne 0) { throw 'DXE Compilation failed' }

    $binary = Join-Path $buildDir $binaryName
    if (-not (Test-Path -LiteralPath $binary)) { throw 'DXE EFI output missing' }
    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    $targetOrder = if ($StopReleaseOnlyBench -and $RuntimeSelection) { 'RUNTIME_SUPPORTED_NVIDIA_ENUMERATION' } else { $selection.Locations -join ',' }
    $reportedSelectedCount = if ($StopReleaseOnlyBench -and $RuntimeSelection) { 'RUNTIME' } else { [string]$selection.Count }
    $singleTargetActive = if ($StopReleaseOnlyBench) { 0 } else { [int](($SyntheticCompileProfile -or $AcceptUnverifiedActuation) -and ($RuntimeSelection -or $selection.Count -eq 1)) }
    $stopOnlyAssertions = if ($StopReleaseOnlyBench) { 'FIRMWARE_ASSET_FILES_READ=0 PROM_READ=0 OS_HANDOFF=NONE' } else { '' }
    Write-Output "STATUS=COMPLETE PATH=$binary SHA256=$hash SELECTED_COUNT=$reportedSelectedCount RUNTIME_SELECTION=$([int][bool]$RuntimeSelection) RUNTIME_ESP=$([int][bool]$RuntimeEsp) PREPARATION_ONLY=$([int]($selection.Count -gt 0 -and -not $SyntheticCompileProfile -and -not $AcceptUnverifiedActuation)) SINGLE_TARGET_ACTIVE=$singleTargetActive ORDERED_SEQUENCE_COMPILE_ONLY=$([int]($SyntheticCompileProfile -and $selection.Count -gt 1)) ORDERED_SEQUENCE=$([int](($AcceptUnverifiedActuation -and $selection.Count -gt 1) -or $StopReleaseOnlyBench)) MULTI_GPU_MUTATION_ALLOWED=$([int][bool]$AcceptUnverifiedActuation) SYNTHETIC_PROFILE=$([int][bool]$SyntheticCompileProfile) UNVERIFIED_ACTUATION=$([int][bool]$AcceptUnverifiedActuation) ACTUATION_VERIFIED=0 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0 WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION ASPM_ADMISSION_BYPASSED=$([int][bool]$StopReleaseOnlyBench) INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED=$([int][bool]$StopReleaseOnlyBench) FILE_ONLY_OUTPUT=$([int][bool]$StopReleaseOnlyBench) STOP_RELEASE_ONLY=$([int][bool]$StopReleaseOnlyBench) $stopOnlyAssertions FIRMWARE_TRANSACTION=$([int]($AcceptUnverifiedActuation -and -not $StopReleaseOnlyBench)) UNLOCK=$([int]($AcceptUnverifiedActuation -and -not $StopReleaseOnlyBench)) REATTACH_ATTEMPTED=$([int][bool]$StopReleaseOnlyBench) REATTACH_VERIFIED=0 TARGET_ORDER=$targetOrder NOT_FOR_DEPLOYMENT=$([int]($SyntheticCompileProfile -or $AcceptUnverifiedActuation))"
} finally {
    Pop-Location
}
