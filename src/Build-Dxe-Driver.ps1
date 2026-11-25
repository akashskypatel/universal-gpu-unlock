param(
    [string]$VcVars,
    [string]$SelectionBdf,
    [string]$SelectionBdfs,
    [switch]$EnableSingleTargetExecution,
    [switch]$SyntheticCompileProfile,
    [switch]$AcceptUnverifiedActuation,
    [switch]$StopReleaseOnlyBench,
    [switch]$FileOnlyBootLog,
    [switch]$RuntimeSelection,
    [switch]$SelectAllGpus,
    [switch]$RuntimeEsp,
    [string]$StagePartitionGuid,
    [switch]$EspResident
)
$ErrorActionPreference = 'Stop'
$firmwareReattach = $AcceptUnverifiedActuation -and -not $StopReleaseOnlyBench
$stagePinText = 'NONE'
if ($StagePartitionGuid) {
    . (Join-Path $PSScriptRoot 'Windows-Esp-Build.ps1')
    $stagePin = ConvertTo-GuWindowsEspGuid $StagePartitionGuid
    $stagePinText = $stagePin.ToString('D')
}
if ($firmwareReattach -and $EspResident) { throw 'DXE firmware reattachment does not support Windows ESP resident mode.' }
if ($SelectAllGpus -and (-not $firmwareReattach -or -not $RuntimeSelection -or $SelectionBdf -or $SelectionBdfs)) { throw '-SelectAllGpus requires the runtime DXE firmware transaction without compiled targets.' }
if ($FileOnlyBootLog -and -not $StopReleaseOnlyBench) {
    throw '-FileOnlyBootLog requires -StopReleaseOnlyBench; ordinary menu images need console output.'
}
if ($StopReleaseOnlyBench -and -not $FileOnlyBootLog) {
    throw '-StopReleaseOnlyBench requires -FileOnlyBootLog to preserve the mandatory file-first no-menu contract.'
}
if ($StopReleaseOnlyBench -and -not $SelectionBdf -and -not $SelectionBdfs) {
    # No explicit BDFs means the default live-inventory select-all policy.
    $RuntimeSelection = $true
}
# The experimental original NVIDIA Stop and stock recovery baseline passed.
# The owner accepts cold-power recovery risk. Production hardware ownership
# and signed firmware acceptance remain unverified. Default outputs are
# compile-only; the waived StopReleaseOnlyBench profile defaults to runtime
# enumeration, with optional -SelectionBdfs for a specific ordered sequence.
. (Join-Path $PSScriptRoot 'Gpu-Execution-Sources.ps1')
Assert-GuExecutionBuildOptions -EnableSingleTargetExecution:$EnableSingleTargetExecution -SyntheticCompileProfile:$SyntheticCompileProfile -AcceptUnverifiedActuation:$AcceptUnverifiedActuation -StopReleaseOnlyBench:$StopReleaseOnlyBench -RuntimeSelection:$RuntimeSelection -RuntimeEsp:$RuntimeEsp -EspResident:$EspResident -SelectionBdf $SelectionBdf -SelectionBdfs $SelectionBdfs
. (Join-Path $PSScriptRoot 'Msvc-Environment.ps1')
$VcVars = Resolve-GuVcVars -VcVars $VcVars -SearchDefaultPaths

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
        if (-not $RuntimeSelection -and $selection.Count -lt 1) {
            throw '-StopReleaseOnlyBench with compiled targets requires at least one BDF; omit BDFs for live inventory selection.'
        }
    }

    $sources = @('dxe_main.c') + $executionCoreSources + @('driver_nvram.c') + $executionSupportSources
    if ($StopReleaseOnlyBench) {
        # The release-only image must not even link the firmware-unlock
        # implementation; unlock_execution.c is compiled to the dedicated
        # Stop/release/reconnect runner under GU_STOP_RELEASE_ONLY_BENCH.
        $sources = @($sources | Where-Object { $_ -ne 'vendor\unlock_610.c' })
    }

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

    if ($StagePartitionGuid) {
        $octets = @($stagePin.ToByteArray() | ForEach-Object { '0x{0:x2}' -f $_ })
        Set-Content -LiteralPath (Join-Path $buildDir 'stage-partition-pin.h') -Encoding ascii -Value ('#define GU_STAGE_PARTITION_PIN_BYTES {' + ($octets -join ',') + '}')
        $clFlags += ' /DGU_DEDICATED_STAGE_PIN'
    }
    if ($firmwareReattach) {
        $clFlags += ' /DGU_FIRMWARE_REATTACH /DGU_DXE_FIRMWARE_REATTACH'
        if ($SelectAllGpus) { $clFlags += ' /DGU_SELECT_ALL_GPUS' }
    }
    if ($StopReleaseOnlyBench) {
        # Explicit Stop/release-only image: keep file logging, waive ASPM admission
        # only, and compile out the unlock path; AMD console checks stay active.
        $clFlags += ' /DGU_UNVERIFIED_BENCH_CONDITIONS /DGU_STOP_RELEASE_ONLY_BENCH'
        if ($FileOnlyBootLog) { $clFlags += ' /DGU_DXE_FILE_ONLY_OUTPUT' }
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

    $compilePlan = New-GuSourceCompilePlan -Sources $sources -CompilerFlags $clFlags -ObjectDirectory 'build_dxe' -VcVars $VcVars -SourceDirectory $PSScriptRoot
    $compileCmds = $compilePlan.Commands
    $objFiles = $compilePlan.Objects

    $binaryName = if ($StopReleaseOnlyBench -and $RuntimeSelection) { 'GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-DXE.efi' } elseif ($StopReleaseOnlyBench) { 'GPU-SEQUENCE-STOP-RELEASE-BENCH-DXE-' + $selection.Label + '.efi' } elseif ($RuntimeSelection -and $AcceptUnverifiedActuation) { 'GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION-DXE.efi' } elseif ($RuntimeSelection) { 'GPU-RUNTIME-SELECTION-PREP-DXE.efi' } elseif ($AcceptUnverifiedActuation -and $selection.Count -gt 1) { 'GPU-SEQUENCE-UNVERIFIED-ACTUATION-DXE-' + $selection.Label + '.efi' } elseif ($AcceptUnverifiedActuation) { 'GPU-ONE-TARGET-UNVERIFIED-ACTUATION-DXE-' + $selection.Label + '.efi' } elseif ($SyntheticCompileProfile -and $selection.Count -gt 1) { 'GPU-SEQUENCE-COMPILE-ONLY-DXE-' + $selection.Label + '.efi' } elseif ($SyntheticCompileProfile) { 'GPU-ONE-TARGET-COMPILE-ONLY-DXE-' + $selection.Label + '.efi' } elseif ($EnableSingleTargetExecution) { 'GPU-ONE-TARGET-EXPERIMENTAL-DXE-' + $selection.Label + '.efi' } elseif ($selection.Count) { 'GPU-ACTIVE-PREP-DXE-' + $selection.Label + '.efi' } else { 'AdaUnlockDxe.efi' }
    $compileCmds += New-GuLinkCommand -ImageFlags '/subsystem:efi_boot_service_driver /entry:AdaUnlockDriverEntryPoint' -OutputFile "build_dxe\$binaryName" -Objects $objFiles
    $compileCmds += 'if errorlevel 1 exit /b 1'

    $batchFile = Join-Path $buildDir 'build-dxe.cmd'
    Set-Content -LiteralPath $batchFile -Value $compileCmds -Encoding ascii

    & $env:ComSpec /d /c $batchFile
    if ($LASTEXITCODE -ne 0) { throw 'DXE Compilation failed' }

    $binary = Join-Path $buildDir $binaryName
    if (-not (Test-Path -LiteralPath $binary)) { throw 'DXE EFI output missing' }
    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    $targetOrder = if (($StopReleaseOnlyBench -and $RuntimeSelection) -or $SelectAllGpus) { 'RUNTIME_SUPPORTED_NVIDIA_ENUMERATION' } elseif ($firmwareReattach -and $RuntimeSelection) { 'RUNTIME_SAVED_SELECTION' } else { $selection.Locations -join ',' }
    $reportedSelectedCount = if (($StopReleaseOnlyBench -or $firmwareReattach) -and $RuntimeSelection) { 'RUNTIME' } else { [string]$selection.Count }
    $singleTargetActive = if ($StopReleaseOnlyBench) { 0 } else { [int](($SyntheticCompileProfile -or $AcceptUnverifiedActuation) -and ($RuntimeSelection -or $selection.Count -eq 1)) }
    $stopOnlyAssertions = if ($StopReleaseOnlyBench) { 'FIRMWARE_ASSET_FILES_READ=0 PROM_READ=0 OS_HANDOFF=NONE' } else { '' }
    $stopConsolePolicy = if ($StopReleaseOnlyBench) { 'CONDITIONAL' } elseif ($firmwareReattach) { '1' } else { '0' }
    $s3Policy = if ($firmwareReattach) { 'OPTIONAL' } else { 'NONE' }
    Write-Output "STATUS=COMPLETE PATH=$binary SHA256=$hash SELECTED_COUNT=$reportedSelectedCount RUNTIME_SELECTION=$([int][bool]$RuntimeSelection) RUNTIME_ESP=$([int][bool]$RuntimeEsp) PREPARATION_ONLY=$([int]($selection.Count -gt 0 -and -not $SyntheticCompileProfile -and -not $AcceptUnverifiedActuation)) SINGLE_TARGET_ACTIVE=$singleTargetActive ORDERED_SEQUENCE_COMPILE_ONLY=$([int]($SyntheticCompileProfile -and $selection.Count -gt 1)) ORDERED_SEQUENCE=$([int](($AcceptUnverifiedActuation -and $selection.Count -gt 1) -or $StopReleaseOnlyBench -or ($firmwareReattach -and $RuntimeSelection))) MULTI_GPU_MUTATION_ALLOWED=$([int][bool]$AcceptUnverifiedActuation) SYNTHETIC_PROFILE=$([int][bool]$SyntheticCompileProfile) UNVERIFIED_ACTUATION=$([int][bool]$AcceptUnverifiedActuation) ACTUATION_VERIFIED=0 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0 WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION ASPM_ADMISSION_BYPASSED=$([int]($StopReleaseOnlyBench -or $firmwareReattach)) INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED=$stopConsolePolicy FILE_ONLY_OUTPUT=$([int][bool]$FileOnlyBootLog) STOP_RELEASE_ONLY=$([int][bool]$StopReleaseOnlyBench) $stopOnlyAssertions FIRMWARE_TRANSACTION=$([int]($AcceptUnverifiedActuation -and -not $StopReleaseOnlyBench)) UNLOCK=$([int]($AcceptUnverifiedActuation -and -not $StopReleaseOnlyBench)) REATTACH_ATTEMPTED=$([int]($StopReleaseOnlyBench -or $firmwareReattach)) REATTACH_VERIFIED=0 FIRMWARE_REATTACH=$([int]$firmwareReattach) S3_POLICY=$s3Policy TARGET_ORDER=$targetOrder STAGE_PARTITION_GUID=$stagePinText NOT_FOR_DEPLOYMENT=$([int]($SyntheticCompileProfile -or $AcceptUnverifiedActuation))"
} finally {
    Pop-Location
}
