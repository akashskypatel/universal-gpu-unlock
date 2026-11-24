param(
    [string]$VcVars
)
$ErrorActionPreference='Stop'
# Separate owner maintenance image. Build only; no signing, staging or boot entry.
. (Join-Path $PSScriptRoot 'Msvc-Environment.ps1')
$VcVars = Resolve-GuVcVars -VcVars $VcVars

Push-Location -LiteralPath $PSScriptRoot
try {
    $output=Join-Path $PSScriptRoot 'build_boot_state_maintenance'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $sources=@('boot_state_maintenance.c','boot_state.c','format.c','runtime.c')
    $flags='/nologo /O2 /Oi- /GS- /Gs65536 /Zl /c /TC /D_CRT_SECURE_NO_WARNINGS /I. /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'
    $commands=@('@echo off',('call "'+$VcVars+'" >nul'),'if errorlevel 1 exit /b 1',('cd /d "'+$PSScriptRoot+'"'))
    $objects=@()
    foreach ($source in $sources) {
        $obj='build_boot_state_maintenance\' + [IO.Path]::GetFileNameWithoutExtension($source) + '.obj'
        $objects+=$obj
        $commands+=('cl '+$flags+' /Fo"'+$obj+'" "'+$source+'"')
        $commands+='if errorlevel 1 exit /b 1'
    }
    $joined=($objects | ForEach-Object { '"'+$_+'"' }) -join ' '
    $binary='BOOT-STATE-MAINTENANCE.EFI'
    $path=Join-Path $output $binary
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    $commands+=('link /nologo /subsystem:efi_application /entry:UefiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /out:build_boot_state_maintenance\'+$binary+' '+$joined)
    $commands+='if errorlevel 1 exit /b 1'
    $batch=Join-Path $output 'build-maintenance.cmd'
    Set-Content -LiteralPath $batch -Encoding ascii -Value $commands
    & $env:ComSpec /d /c $batch
    if ($LASTEXITCODE -ne 0) {throw "Maintenance build failed (exit=$LASTEXITCODE)"}
    if (-not (Test-Path -LiteralPath $path)) {throw 'Maintenance EFI missing'}
    $sha=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    Write-Output "STATUS=BUILT PATH=$path SHA256=$sha MAINTENANCE_ONLY=1 GPU_ACTION=0 NVRAM_DELETE=EXPLICIT_REVIEW_ONLY NO_BOOT_ENTRY_CHANGE=1 NO_RESET=1 NOT_FOR_AUTOMATIC_DEPLOYMENT=1 HARDWARE_TESTED=0"
} finally {
    Pop-Location
}
