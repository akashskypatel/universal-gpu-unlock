param(
    [string]$VcVars,
    [string]$SelectionBdf,
    [string]$SelectionBdfs,
    [switch]$EnableSingleTargetExecution,
    [switch]$SyntheticCompileProfile,
    [switch]$AcceptUnverifiedActuation,
    [switch]$StopReleaseOnlyBench,
    [switch]$StopReleaseWindowsHandoff,
    [switch]$FirmwareReattach,
    [switch]$FileOnlyBootLog,
    [switch]$RuntimeSelection,
    [switch]$RuntimeEsp,
    [string]$WindowsEspGuid,
    [switch]$EspResident,
    [switch]$CapturedPromReference,
    [ValidateSet('Menu','Unlock','UnlockWithResume','InstallDriver','RemoveDriver')]
    [string]$ApplicationOperation = 'Menu'
)
$ErrorActionPreference = 'Stop'
$espPinText = 'NONE'
if ($WindowsEspGuid) {
    if (-not $FirmwareReattach -or -not $RuntimeEsp) { throw '-WindowsEspGuid requires -FirmwareReattach and -RuntimeEsp in this builder.' }
    . (Join-Path $PSScriptRoot 'Windows-Esp-Build.ps1')
    $espPin = ConvertTo-GuWindowsEspGuid $WindowsEspGuid
    $espPinText = $espPin.ToString('D')
}
if ($FirmwareReattach -and (-not $AcceptUnverifiedActuation -or -not $EnableSingleTargetExecution -or -not $RuntimeSelection -or -not $RuntimeEsp -or $EspResident -or $StopReleaseOnlyBench -or $SyntheticCompileProfile -or $ApplicationOperation -ne 'Unlock')) {
    throw '-FirmwareReattach requires the waived runtime Unlock application without Stop-only or resume policy.'
}
# Operation selection is independent of the existing target selection and
# admission gates. A fixed operation is only valid for the ordinary waived app.
if ($ApplicationOperation -ne 'Menu' -and (-not $AcceptUnverifiedActuation -or $StopReleaseOnlyBench -or $SyntheticCompileProfile)) {
    throw '-ApplicationOperation requires the ordinary -AcceptUnverifiedActuation application.'
}
$operationKeys = @{ Unlock = 85; UnlockWithResume = 80; InstallDriver = 73; RemoveDriver = 82 }
$fixedOperation = [int]($ApplicationOperation -ne 'Menu')
# ValidateSet accepts case-insensitive input. Canonicalize it for receipts.
if ($fixedOperation) {
    $ApplicationOperation = @($operationKeys.Keys | Where-Object { $_ -ieq $ApplicationOperation })[0]
} else { $ApplicationOperation = 'Menu' }
$firmwareTransaction = [int]($AcceptUnverifiedActuation -and -not $StopReleaseOnlyBench -and $ApplicationOperation -notin @('InstallDriver','RemoveDriver'))
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
if ($StopReleaseWindowsHandoff -and (-not $StopReleaseOnlyBench -or -not $RuntimeSelection -or -not $RuntimeEsp -or $EspResident -or $SelectionBdf -or $SelectionBdfs)) {
    throw '-StopReleaseWindowsHandoff requires the runtime Stop-only USB profile with runtime ESP discovery and no fixed BDFs.'
}
# The experimental original NVIDIA Stop and stock recovery baseline passed.
# The owner accepts cold-power recovery risk. Production hardware ownership
# and signed firmware acceptance remain unverified.
#
# Default behaviour is unchanged: compile-only images, or preparation-only
# artifacts that never touch the GPU. -AcceptUnverifiedActuation is the single
# explicit operator waiver. The StopReleaseOnlyBench profile is narrower: by
# default it selects all supported NVIDIA GPUs found at runtime, or it can embed
# an explicit multi-BDF order. It waives ASPM admission only and compiles out
# vendor/unlock_610.c. Nothing here verifies the result:
# CAN_DETACH/CAN_UNLOCK/CAN_FLASH/RECOVERY_VERIFIED stay false.
if ($AcceptUnverifiedActuation -and -not $EnableSingleTargetExecution) {
    throw '-AcceptUnverifiedActuation requires -EnableSingleTargetExecution: the waiver only applies to ordered active execution builds'
}
if ($AcceptUnverifiedActuation -and $SyntheticCompileProfile) {
    throw '-AcceptUnverifiedActuation cannot be combined with -SyntheticCompileProfile: a compile-only image never actuates'
}
if ($StopReleaseOnlyBench -and -not $AcceptUnverifiedActuation) {
    throw '-StopReleaseOnlyBench requires -AcceptUnverifiedActuation.'
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
. (Join-Path $PSScriptRoot 'Msvc-Environment.ps1')
$VcVars = Resolve-GuVcVars -VcVars $VcVars -SearchDefaultPaths

Push-Location -LiteralPath $PSScriptRoot
try {
    $buildDir = Join-Path $PSScriptRoot 'build'
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
    . (Join-Path $PSScriptRoot 'Gpu-Selection-Build.ps1')
    $selection = New-GuGpuSelectionSpec -SelectionBdf $SelectionBdf -SelectionBdfs $SelectionBdfs -OutputDirectory $buildDir
    if (-not $RuntimeSelection -and $EnableSingleTargetExecution -and $selection.Count -ne 1 -and -not $AcceptUnverifiedActuation) {
        throw "Single-target execution requires exactly one explicit -SelectionBdf"
    }
    if ($AcceptUnverifiedActuation -and -not $RuntimeSelection -and $selection.Count -lt 1) {
        throw '-AcceptUnverifiedActuation requires at least one explicit -SelectionBdf or -SelectionBdfs entry'
    }
    if ($StopReleaseOnlyBench -and -not $RuntimeSelection -and $selection.Count -lt 1) {
        throw '-StopReleaseOnlyBench with compiled targets requires at least one BDF; omit BDFs for live inventory selection.'
    }

    . (Join-Path $PSScriptRoot 'Gpu-Execution-Sources.ps1')
    $sources = @('app.c', 'driver_nvram.c') + $executionCoreSources + $executionSupportSources
    if ($StopReleaseOnlyBench) {
        # This application can Stop/release and reconnect only; do not link the
        # vendor implementation that performs the firmware-unlock transaction.
        $sources = @($sources | Where-Object { $_ -ne 'vendor\unlock_610.c' })
    }

    $clFlags = '/nologo /O2 /Oi /GS- /Gs65536 /Zl /c /TC /D__KERNEL__ /DGU_UEFI_PORT /DGU_AD104_TRANSACTION /DGU_ENTRY_LATCHED_SS1_FULL /D_CRT_SECURE_NO_WARNINGS /I. /Ibuild /Icompat /Ivendor /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'

    if ($selection.Count -or $RuntimeSelection) {
        if ($AcceptUnverifiedActuation) {
            # Explicit waiver: real transaction defines, no host-test or
            # synthetic profile, and the documented ordered-sequential model
            # (one card active at a time, each step gated on its predecessor).
            $clFlags += ' /DGU_ACTIVE_SINGLE_TARGET /DGU_ACTIVE_UNVERIFIED_ACTUATION'
        } elseif ($SyntheticCompileProfile) {
            # The selected execution source can compile for any valid order.
            # Both EFI entrypoints return before any firmware or GPU operation.
            $clFlags += ' /DGU_ACTIVE_SINGLE_TARGET /DGU_HOST_TEST_PROFILE /DGU_SYNTHETIC_COMPILE_ONLY'
        } else {
            $clFlags += ' /DGU_ACTIVE_SEQUENCE_PREPARE_ONLY /DGU_HOST_TEST_PROFILE'
        }
    }

    if ($FirmwareReattach) { $clFlags += ' /DGU_FIRMWARE_REATTACH' }
    if ($fixedOperation) {
        $clFlags += " /DGU_APP_OPERATION=$($operationKeys[$ApplicationOperation])"
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

    if ($WindowsEspGuid) {
        # .NET Guid.ToByteArray uses the EFI_GUID byte order carried in a GPT device path.
        $octets = @($espPin.ToByteArray() | ForEach-Object { '0x{0:x2}' -f $_ })
        $header = Join-Path $buildDir 'windows-esp-pin.h'
        Set-Content -LiteralPath $header -Encoding ascii -Value @(
            ('#define GU_WINDOWS_ESP_PIN_BYTES {' + ($octets -join ',') + '}'),
            ('#define GU_WINDOWS_ESP_PIN_TEXT "' + $espPinText + '"')
        )
        $clFlags += ' /DGU_RUNTIME_ESP_PIN'
    }

    if ($RuntimeSelection) {
        # Targets come from the "GuTargets" UEFI variable at boot, so the count
        # is unknown at build time. The runtime admission gates are unchanged.
        $clFlags += ' /DGU_RUNTIME_SELECTION'
    }

    if ($StopReleaseOnlyBench) {
        # The bootable application has the same tightly scoped ASPM waiver and
        # file-only routine logging as the bench DXE, but preserves console checks.
        $clFlags += ' /DGU_UNVERIFIED_BENCH_CONDITIONS /DGU_STOP_RELEASE_ONLY_BENCH'
        if ($FileOnlyBootLog) { $clFlags += ' /DGU_DXE_FILE_ONLY_OUTPUT' }
        if ($StopReleaseWindowsHandoff) { $clFlags += ' /DGU_STOP_RELEASE_WINDOWS_HANDOFF' }
    }

    if ($CapturedPromReference) {
        # Carry the two per-card firmware references this bench captured. Without
        # it the image ships an empty allowlist and GuAd104PromMatches(...,TRUE)
        # can only ever answer UNVERIFIED - which is the fail-closed default, not
        # a defect. The rows are digests of what was read, so the image still
        # logs SIGNATURE_AUTHENTICATED=0 on both paths.
        $clFlags += ' /DGU_PROM_REFERENCE_CAPTURED'
    }

    $compilePlan = New-GuSourceCompilePlan -Sources $sources -CompilerFlags $clFlags -ObjectDirectory 'build' -VcVars $VcVars -SourceDirectory $PSScriptRoot
    $compileCmds = $compilePlan.Commands
    $objFiles = $compilePlan.Objects

    $binaryName = if ($StopReleaseWindowsHandoff) { 'GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-WINDOWS-HANDOFF.EFI' } elseif ($StopReleaseOnlyBench -and $RuntimeSelection) { 'GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME.EFI' } elseif ($StopReleaseOnlyBench) { 'GPU-SEQUENCE-STOP-RELEASE-BENCH-' + $selection.Label + '.EFI' } elseif ($RuntimeSelection -and $AcceptUnverifiedActuation) { 'GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION.EFI' } elseif ($RuntimeSelection) { 'GPU-RUNTIME-SELECTION-PREP.EFI' } elseif ($AcceptUnverifiedActuation -and $selection.Count -gt 1) { 'GPU-SEQUENCE-UNVERIFIED-ACTUATION-' + $selection.Label + '.EFI' } elseif ($AcceptUnverifiedActuation) { 'GPU-ONE-TARGET-UNVERIFIED-ACTUATION-' + $selection.Label + '.EFI' } elseif ($SyntheticCompileProfile -and $selection.Count -gt 1) { 'GPU-SEQUENCE-COMPILE-ONLY-' + $selection.Label + '.EFI' } elseif ($SyntheticCompileProfile) { 'GPU-ONE-TARGET-COMPILE-ONLY-' + $selection.Label + '.EFI' } elseif ($EnableSingleTargetExecution) { 'GPU-ONE-TARGET-EXPERIMENTAL-' + $selection.Label + '.EFI' } elseif ($selection.Count) { 'GPU-ACTIVE-PREP-' + $selection.Label + '.EFI' } else { 'AD102-EXPERIMENTAL.EFI' }
    if ($FirmwareReattach) {
        $binaryName = 'GPU-RUNTIME-FIRMWARE-REATTACH.EFI'
    } elseif ($fixedOperation) {
        $operationLabel = ($ApplicationOperation -creplace '([a-z])([A-Z])','$1-$2').ToUpperInvariant()
        $binaryName = $binaryName -replace '\.EFI$',"-$operationLabel.EFI"
    }
    $compileCmds += New-GuLinkCommand -ImageFlags '/subsystem:efi_application /entry:UefiMain' -OutputFile "build\$binaryName" -Objects $objFiles
    $compileCmds += 'if errorlevel 1 exit /b 1'

    $batchFile = Join-Path $buildDir 'build-msvc.cmd'
    Set-Content -LiteralPath $batchFile -Value $compileCmds -Encoding ascii

    & $env:ComSpec /d /c $batchFile
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }

    $binary = Join-Path $buildDir $binaryName
    if (-not (Test-Path -LiteralPath $binary)) { throw 'EFI output missing' }
    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    $targetOrder = if ($FirmwareReattach) { 'RUNTIME_OPERATOR_SELECTION' } elseif ($StopReleaseOnlyBench -and $RuntimeSelection) { 'RUNTIME_SUPPORTED_NVIDIA_ENUMERATION' } else { $selection.Locations -join ',' }
    $reportedSelectedCount = if ($FirmwareReattach -or ($StopReleaseOnlyBench -and $RuntimeSelection)) { 'RUNTIME' } else { [string]$selection.Count }
    $singleTargetActive = if ($StopReleaseOnlyBench) { 0 } else { [int](($SyntheticCompileProfile -or $AcceptUnverifiedActuation) -and ($RuntimeSelection -or $selection.Count -eq 1)) }
    $stopOnlyAssertions = if ($StopReleaseWindowsHandoff) { 'FIRMWARE_ASSET_FILES_READ=0 PROM_READ=0 OS_HANDOFF=WINDOWS_AFTER_SUCCESS' } elseif ($StopReleaseOnlyBench) { 'FIRMWARE_ASSET_FILES_READ=0 PROM_READ=0 OS_HANDOFF=NONE' } else { '' }
    $stopConsolePolicy = if ($FirmwareReattach) { '1' } elseif ($StopReleaseOnlyBench) { 'CONDITIONAL' } else { '0' }
    Write-Output "STATUS=COMPLETE PATH=$binary SHA256=$hash SELECTED_COUNT=$reportedSelectedCount RUNTIME_SELECTION=$([int][bool]$RuntimeSelection) RUNTIME_ESP=$([int][bool]$RuntimeEsp) PREPARATION_ONLY=$([int]($selection.Count -gt 0 -and -not $SyntheticCompileProfile -and -not $AcceptUnverifiedActuation)) SINGLE_TARGET_ACTIVE=$singleTargetActive ORDERED_SEQUENCE_COMPILE_ONLY=$([int]($SyntheticCompileProfile -and $selection.Count -gt 1)) ORDERED_SEQUENCE=$([int](($AcceptUnverifiedActuation -and $selection.Count -gt 1) -or $StopReleaseOnlyBench -or $FirmwareReattach)) MULTI_GPU_MUTATION_ALLOWED=$([int][bool]$AcceptUnverifiedActuation) SYNTHETIC_PROFILE=$([int][bool]$SyntheticCompileProfile) UNVERIFIED_ACTUATION=$([int][bool]$AcceptUnverifiedActuation) ACTUATION_VERIFIED=0 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0 WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION ASPM_ADMISSION_BYPASSED=$([int][bool]($StopReleaseOnlyBench -or $FirmwareReattach)) INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED=$stopConsolePolicy FILE_ONLY_OUTPUT=$([int][bool]$FileOnlyBootLog) STOP_RELEASE_ONLY=$([int][bool]$StopReleaseOnlyBench) $stopOnlyAssertions APP_OPERATION=$ApplicationOperation FIXED_OPERATION=$fixedOperation FIRMWARE_TRANSACTION=$firmwareTransaction UNLOCK=$firmwareTransaction REATTACH_ATTEMPTED=$([int][bool]($StopReleaseOnlyBench -or $FirmwareReattach)) REATTACH_VERIFIED=0 FIRMWARE_REATTACH=$([int][bool]$FirmwareReattach) TARGET_ORDER=$targetOrder WINDOWS_ESP_GUID=$espPinText NOT_FOR_DEPLOYMENT=$([int]($SyntheticCompileProfile -or $AcceptUnverifiedActuation))"
} finally {
    Pop-Location
}
