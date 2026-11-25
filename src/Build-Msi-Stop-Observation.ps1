param(
    [string]$VcVars,
    [switch]$AcceptUnboundedStopRisk,
    [switch]$Target3080,
    [switch]$BothNvidia,
    [string]$WindowsEspGuid,
    [switch]$AcceptWindowsHandoffRisk
)
$ErrorActionPreference='Stop'
# A separate one-card Stop experiment without unlock, flash, DMA or reset.
# Post-Stop Windows handoff is independently pinned and explicitly opted in.
if (-not $AcceptUnboundedStopRisk) {
    throw 'Explicit -AcceptUnboundedStopRisk switch required; original Stop may hang.'
}
if ($AcceptWindowsHandoffRisk -and -not $WindowsEspGuid) {
    throw '-AcceptWindowsHandoffRisk requires -WindowsEspGuid.'
}
if ($WindowsEspGuid -and -not $AcceptWindowsHandoffRisk) {
    throw '-WindowsEspGuid requires explicit -AcceptWindowsHandoffRisk.'
}
if ($BothNvidia -and $Target3080) {
    throw '-BothNvidia cannot be combined with -Target3080: dual mode has fixed 4090 then 3080 order.'
}
if ($BothNvidia -and (-not $AcceptWindowsHandoffRisk -or -not $WindowsEspGuid)) {
    throw '-BothNvidia requires explicit risk acceptance and pinned Windows handoff.'
}
. (Join-Path $PSScriptRoot 'Msvc-Environment.ps1')
$VcVars = Resolve-GuVcVars -VcVars $VcVars

Push-Location -LiteralPath $PSScriptRoot
try {
    $output=Join-Path $PSScriptRoot 'build_stop_observation'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $sources=@('msi_stop_observation.c','sha256.c','format.c','runtime.c')
    $flags='/nologo /O2 /Oi- /GS- /Gs65536 /Zl /c /TC /DGU_EXPERIMENTAL_STOP_OBSERVATION /D_CRT_SECURE_NO_WARNINGS /I. /Ibuild_stop_observation /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'
    if ($Target3080) { $flags+=' /DGU_STOP_TARGET_3080' }
    if ($BothNvidia) { $flags+=' /DGU_STOP_BOTH_NVIDIA' }
    if ($WindowsEspGuid) {
        . (Join-Path $PSScriptRoot 'Windows-Esp-Build.ps1')
        $parsed = ConvertTo-GuWindowsEspGuid $WindowsEspGuid
        $initializer = New-GuWindowsEspGuidInitializer $parsed
        Set-Content -LiteralPath (Join-Path $output 'windows-esp-guid.h') -Encoding ascii -Value ('#define GU_WINDOWS_ESP_GUID_INITIALIZER '+$initializer)
        $sources+=@('gpu_windows_handoff.c','gpu_windows_path.c')
        $flags+=' /DGU_STOP_WINDOWS_HANDOFF'
    }
    . (Join-Path $PSScriptRoot 'Gpu-Execution-Sources.ps1')
    $compilePlan = New-GuSourceCompilePlan -Sources $sources -CompilerFlags $flags -ObjectDirectory 'build_stop_observation' -VcVars $VcVars -SourceDirectory $PSScriptRoot -QuotePaths
    $commands = $compilePlan.Commands
    $objects = $compilePlan.Objects
    $targetName=if ($BothNvidia) { 'DUAL-NVIDIA' } elseif ($Target3080) { 'RTX3080' } else { 'RTX4090' }
    $binary=if ($WindowsEspGuid) { "MSI-$targetName-STOP-WINDOWS-HANDOFF-EXPERIMENT.EFI" }
        else { "MSI-$targetName-STOP-ONCE-EXPERIMENT.EFI" }
    $commands += New-GuLinkCommand -ImageFlags '/subsystem:efi_application /entry:UefiMain' -OutputFile "build_stop_observation\$binary" -Objects $objects -QuotePaths
    $commands+='if errorlevel 1 exit /b 1'
    $batch=Join-Path $output 'build-stop-only.cmd'
    Set-Content -LiteralPath $batch -Encoding ascii -Value $commands
    & $env:ComSpec /d /c $batch
    if ($LASTEXITCODE -ne 0) {throw "Stop observer build failed (exit=$LASTEXITCODE)"}
    $path=Join-Path $output $binary
    if (-not (Test-Path -LiteralPath $path)) {throw 'Stop observer EFI missing'}
    $sha=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $targetField=if ($BothNvidia) { 'TARGETS=0000:01:00.0,0000:04:00.0' } elseif ($Target3080) { 'TARGET=0000:04:00.0' } else { 'TARGET=0000:01:00.0' }
    $stopCount=if ($BothNvidia) { 2 } else { 1 }
    $peerStop=if ($BothNvidia) { 1 } else { 0 }
    Write-Output "STATUS=BUILT PATH=$path SHA256=$sha STOP_ONLY=1 $targetField STOP_COUNT=$stopCount NO_KEY_INPUT=1 UNLOCK=0 FLASH=0 RESET=0 PEER_STOP=$peerStop AMD_STOP=0 WINDOWS_HANDOFF=$([int][bool]$WindowsEspGuid) RECOVERY_VERIFIED=0 HARDWARE_TESTED=0"
} finally {
    Pop-Location
}
