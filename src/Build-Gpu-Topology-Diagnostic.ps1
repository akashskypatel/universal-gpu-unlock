param(
    [string]$VcVars,
    [switch]$SaveLog,
    [switch]$CycleGopText,
    [switch]$MsiB850RomVariables,
    [switch]$MsiB850FuseVersion,
    [switch]$MsiB850Sec2Ucode1,
    [switch]$VramBlockProbe,
    [string]$WindowsEspGuid,
    [string]$SelectionBdf,
    [string]$SelectionBdfs
)
$ErrorActionPreference = 'Stop'
# A separate, strict read-only PE/COFF EFI application. Never build the
# vendor unlock core, app.c, dxe_main.c, MSI release, DMA or persistence code.
. (Join-Path $PSScriptRoot 'Msvc-Environment.ps1')
$VcVars = Resolve-GuVcVars -VcVars $VcVars

Push-Location -LiteralPath $PSScriptRoot
try {
    $output = Join-Path $PSScriptRoot 'build_diagnostic'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    . (Join-Path $PSScriptRoot 'Gpu-Selection-Build.ps1')
    $selection = New-GuGpuSelectionSpec -SelectionBdf $SelectionBdf -SelectionBdfs $SelectionBdfs -OutputDirectory $output -MaximumTargets 128
    if ($CycleGopText -and -not $SaveLog) { throw '-CycleGopText requires -SaveLog.' }
    if ($CycleGopText -and $WindowsEspGuid) { throw '-CycleGopText cannot combine with Windows handoff.' }
    if ($CycleGopText -and @($MsiB850RomVariables.IsPresent, $MsiB850FuseVersion.IsPresent, $MsiB850Sec2Ucode1.IsPresent, $VramBlockProbe.IsPresent).Where({ $_ }).Count -gt 0) { throw '-CycleGopText cannot combine with MSI register probes.' }
    if ($MsiB850RomVariables -and -not $SaveLog) { throw '-MsiB850RomVariables requires -SaveLog.' }
    if ($MsiB850FuseVersion -and -not $SaveLog) { throw '-MsiB850FuseVersion requires -SaveLog.' }
    if ($MsiB850Sec2Ucode1 -and -not $SaveLog) { throw '-MsiB850Sec2Ucode1 requires -SaveLog.' }
    if ($VramBlockProbe -and -not $SaveLog) { throw '-VramBlockProbe requires -SaveLog to keep the block evidence.' }
    if (@($MsiB850RomVariables.IsPresent, $MsiB850FuseVersion.IsPresent, $MsiB850Sec2Ucode1.IsPresent, $VramBlockProbe.IsPresent).Where({ $_ }).Count -gt 1) { throw 'MSI B850 diagnostics must be built separately.' }
    if (($MsiB850RomVariables -or $MsiB850FuseVersion -or $MsiB850Sec2Ucode1 -or $VramBlockProbe) -and $selection.Count) { throw 'MSI B850 source diagnostics cannot select GPUs.' }
    if ($selection.Count -and $WindowsEspGuid) {
        throw 'Selection-only diagnostic cannot include Windows handoff.'
    }
    $sources = @('gpu_inventory.c','gpu_topology.c','gpu_pci_observation.c','gpu_topology_efi.c','gpu_windows_path.c','sha256.c','gpu_loaded_pe_text.c','format.c','runtime.c')
    if ($WindowsEspGuid) {
        if (-not $SaveLog) { throw '-WindowsEspGuid requires -SaveLog to preserve diagnostic evidence before handoff.' }
        . (Join-Path $PSScriptRoot 'Windows-Esp-Build.ps1')
        $parsed = ConvertTo-GuWindowsEspGuid $WindowsEspGuid
        $initializer = New-GuWindowsEspGuidInitializer $parsed
        $header = Join-Path $output 'windows-esp-guid.h'
        Set-Content -LiteralPath $header -Encoding ascii -Value ('#define GU_WINDOWS_ESP_GUID_INITIALIZER ' + $initializer)
        $sources += 'gpu_windows_handoff.c'
    }
    $flags = '/nologo /O2 /Oi- /GS- /Gs65536 /Zl /c /TC /DGU_GPU_DIAGNOSTIC_ONLY /D_CRT_SECURE_NO_WARNINGS /I. /Ibuild_diagnostic /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'
    if ($SaveLog) { $flags += ' /DGU_TOPO_SAVE_LOG' }
    if ($CycleGopText) { $flags += ' /DGU_TOPO_GOP_TEXT_CYCLE' }
    if ($MsiB850RomVariables) { $flags += ' /DGU_MSI_B850_ROM_VARIABLES' }
    if ($MsiB850FuseVersion) { $flags += ' /DGU_MSI_B850_FUSE_VERSION' }
    if ($MsiB850Sec2Ucode1) { $flags += ' /DGU_MSI_B850_SEC2_UCODE1' }
    if ($VramBlockProbe) { $flags += ' /DGU_VRAM_BLOCK_PROBE' }
    if ($selection.Count) { $flags += ' /DGU_TOPO_SELECTION_ENABLED' }
    $binaryName = if ($MsiB850RomVariables) {
        if ($WindowsEspGuid) { 'MSI-B850-ROM-VARIABLES-WINDOWS-HANDOFF-DIAGNOSTIC.EFI' }
        else { 'MSI-B850-ROM-VARIABLES-DIAGNOSTIC.EFI' }
    } elseif ($MsiB850FuseVersion) {
        if ($WindowsEspGuid) { 'MSI-B850-FUSE-VERSION-WINDOWS-HANDOFF-DIAGNOSTIC.EFI' }
        else { 'MSI-B850-FUSE-VERSION-DIAGNOSTIC.EFI' }
    } elseif ($MsiB850Sec2Ucode1) {
        if ($WindowsEspGuid) { 'MSI-B850-SEC2-UCODE1-WINDOWS-HANDOFF-DIAGNOSTIC.EFI' }
        else { 'MSI-B850-SEC2-UCODE1-DIAGNOSTIC.EFI' }
    } elseif ($VramBlockProbe) {
        if ($WindowsEspGuid) { 'VRAM-BLOCK-PROBE-WINDOWS-HANDOFF-DIAGNOSTIC.EFI' }
        else { 'VRAM-BLOCK-PROBE-DIAGNOSTIC.EFI' }
    } elseif ($CycleGopText) {
        'GPU-TOPOLOGY-GOP-TEXT-CYCLE-DIAGNOSTIC.EFI'
    } elseif ($selection.Count) {
        'GPU-SELECTION-' + $selection.Label + '-DIAGNOSTIC.EFI'
    } elseif ($WindowsEspGuid) {
        'GPU-TOPOLOGY-WINDOWS-HANDOFF-DIAGNOSTIC.EFI'
    } else { 'GPU-TOPOLOGY-DIAGNOSTIC.EFI' }
    if ($WindowsEspGuid) { $flags += ' /DGU_TOPO_WINDOWS_HANDOFF' }
    . (Join-Path $PSScriptRoot 'Gpu-Execution-Sources.ps1')
    $compilePlan = New-GuSourceCompilePlan -Sources $sources -CompilerFlags $flags -ObjectDirectory 'build_diagnostic' -VcVars $VcVars -SourceDirectory $PSScriptRoot -QuotePaths
    $commands = $compilePlan.Commands
    $objects = $compilePlan.Objects
    # Compile the imported PROM reader and EFI adapter for Windows ABI
    # coverage, but NEVER link them into a passive topology EFI. The original
    # BAR0 capture has not been approved for this dual-GPU hardware.
    foreach ($captureSource in @('gpu_prom_capture.c','gpu_prom_capture_efi.c')) {
        $captureObj = 'build_diagnostic\\' + [System.IO.Path]::GetFileNameWithoutExtension($captureSource) + '.obj'
        $commands += "cl $flags /Fo`"$captureObj`" `"$captureSource`""
        $commands += 'if errorlevel 1 exit /b 1'
    }
    $commands += New-GuLinkCommand -ImageFlags '/subsystem:efi_application /entry:UefiMain' -OutputFile "build_diagnostic\$binaryName" -Objects $objects -QuotePaths
    $commands += 'if errorlevel 1 exit /b 1'
    $batch = Join-Path $output 'build-diagnostic.cmd'
    Set-Content -LiteralPath $batch -Encoding ascii -Value $commands
    & $env:ComSpec /d /c $batch
    if ($LASTEXITCODE -ne 0) { throw "Diagnostic EFI build failed (exit=$LASTEXITCODE)" }
    $binary = Join-Path $output $binaryName
    if (-not (Test-Path -LiteralPath $binary)) { throw 'No diagnostic EFI output produced' }
    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    Write-Output "STATUS=COMPLETE PATH=$binary SHA256=$hash READ_ONLY=1 DISPLAY_TEXT_WRITE=$([int]$CycleGopText.IsPresent) GOP_TEXT_CYCLE=$([int]$CycleGopText.IsPresent) SELECTION_BDFS=$($selection.Locations -join ',') MSI_B850_FUSE_VERSION=$([int]$MsiB850FuseVersion.IsPresent) MSI_B850_SEC2_UCODE1=$([int]$MsiB850Sec2Ucode1.IsPresent) MSI_B850_ROM_VARIABLES=$([int]$MsiB850RomVariables.IsPresent) VRAM_BLOCK_PROBE=$([int]$VramBlockProbe.IsPresent) SELECTION_COUNT=$($selection.Count) CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0 SAVE_LOG=$([int]$SaveLog.IsPresent) WINDOWS_HANDOFF=$([int][bool]$WindowsEspGuid)"
} finally {
    Pop-Location
}
