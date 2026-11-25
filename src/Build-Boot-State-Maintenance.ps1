param(
    [string]$VcVars,
    [string]$StagePartitionGuid
)
$ErrorActionPreference='Stop'
$stagePinText = 'NONE'
if ($StagePartitionGuid) {
    . (Join-Path $PSScriptRoot 'Windows-Esp-Build.ps1')
    $stagePin = ConvertTo-GuWindowsEspGuid $StagePartitionGuid
    $stagePinText = $stagePin.ToString('D')
}
# Separate owner maintenance image. Build only; no signing, staging or boot entry.
. (Join-Path $PSScriptRoot 'Msvc-Environment.ps1')
$VcVars = Resolve-GuVcVars -VcVars $VcVars

Push-Location -LiteralPath $PSScriptRoot
try {
    $output=Join-Path $PSScriptRoot 'build_boot_state_maintenance'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $sources=@('boot_state_maintenance.c','boot_state.c','format.c','runtime.c')
    $flags='/nologo /O2 /Oi- /GS- /Gs65536 /Zl /c /TC /D_CRT_SECURE_NO_WARNINGS /I. /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'
    if ($StagePartitionGuid) {
        $octets = @($stagePin.ToByteArray() | ForEach-Object { '0x{0:x2}' -f $_ })
        Set-Content -LiteralPath (Join-Path $output 'stage-partition-pin.h') -Encoding ascii -Value ('#define GU_STAGE_PARTITION_PIN_BYTES {' + ($octets -join ',') + '}')
        $flags += ' /DGU_DEDICATED_STAGE_PIN /Ibuild_boot_state_maintenance'
        $sources += 'gpu_windows_path.c'
    }
    . (Join-Path $PSScriptRoot 'Gpu-Execution-Sources.ps1')
    $compilePlan = New-GuSourceCompilePlan -Sources $sources -CompilerFlags $flags -ObjectDirectory 'build_boot_state_maintenance' -VcVars $VcVars -SourceDirectory $PSScriptRoot -QuotePaths
    $commands = $compilePlan.Commands
    $objects = $compilePlan.Objects
    $binary='BOOT-STATE-MAINTENANCE.EFI'
    $path=Join-Path $output $binary
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    $commands += New-GuLinkCommand -ImageFlags '/subsystem:efi_application /entry:UefiMain' -OutputFile "build_boot_state_maintenance\$binary" -Objects $objects -QuotePaths
    $commands+='if errorlevel 1 exit /b 1'
    $batch=Join-Path $output 'build-maintenance.cmd'
    Set-Content -LiteralPath $batch -Encoding ascii -Value $commands
    & $env:ComSpec /d /c $batch
    if ($LASTEXITCODE -ne 0) {throw "Maintenance build failed (exit=$LASTEXITCODE)"}
    if (-not (Test-Path -LiteralPath $path)) {throw 'Maintenance EFI missing'}
    $sha=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    Write-Output "STATUS=BUILT PATH=$path SHA256=$sha MAINTENANCE_ONLY=1 GPU_ACTION=0 NVRAM_DELETE=EXPLICIT_REVIEW_ONLY NO_BOOT_ENTRY_CHANGE=1 NO_RESET=1 NOT_FOR_AUTOMATIC_DEPLOYMENT=1 HARDWARE_TESTED=0 STAGE_PARTITION_GUID=$stagePinText"
} finally {
    Pop-Location
}
