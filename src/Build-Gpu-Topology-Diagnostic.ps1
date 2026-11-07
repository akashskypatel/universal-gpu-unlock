param(
    [string]$VcVars,
    [switch]$SaveLog,
    [string]$WindowsEspGuid
)
$ErrorActionPreference = 'Stop'
# A separate, strict read-only PE/COFF EFI application. Never build the
# vendor unlock core, app.c, dxe_main.c, MSI release, DMA or persistence code.
if (-not $VcVars) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) { $VcVars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat' }
    }
}
if (-not $VcVars -or -not (Test-Path -LiteralPath $VcVars)) {
    throw 'MSVC x64 build environment not found; specify -VcVars.'
}
Push-Location -LiteralPath $PSScriptRoot
try {
    $output = Join-Path $PSScriptRoot 'build_diagnostic'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $sources = @('gpu_inventory.c','gpu_topology.c','gpu_topology_efi.c','gpu_windows_path.c','format.c','runtime.c')
    if ($WindowsEspGuid) {
        if (-not $SaveLog) { throw '-WindowsEspGuid requires -SaveLog to preserve diagnostic evidence before handoff.' }
        $parsed = [guid]::Empty
        if (-not [guid]::TryParse($WindowsEspGuid, [ref]$parsed) -or $parsed -eq [guid]::Empty) {
            throw 'Invalid or empty Windows ESP partition GUID.'
        }
        $parts = $parsed.ToString('D').Split('-')
        $last = $parts[3] + $parts[4]
        $octets = @(0..7 | ForEach-Object { '0x' + $last.Substring(2*$_, 2) })
        $initializer = '{0x' + $parts[0] + ',0x' + $parts[1] + ',0x' + $parts[2] + ',{' + ($octets -join ',') + '}}'
        $header = Join-Path $output 'windows-esp-guid.h'
        Set-Content -LiteralPath $header -Encoding ascii -Value ('#define GU_WINDOWS_ESP_GUID_INITIALIZER ' + $initializer)
        $sources += 'gpu_windows_handoff.c'
    }
    $flags = '/nologo /O2 /Oi- /GS- /Gs65536 /Zl /c /TC /DGU_GPU_DIAGNOSTIC_ONLY /D_CRT_SECURE_NO_WARNINGS /I. /Ibuild_diagnostic /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'
    if ($SaveLog) { $flags += ' /DGU_TOPO_SAVE_LOG' }
    if ($WindowsEspGuid) { $flags += ' /DGU_TOPO_WINDOWS_HANDOFF' }
    $commands = @('@echo off', "call `"$VcVars`" >nul", 'if errorlevel 1 exit /b 1', "cd /d `"$PSScriptRoot`"")
    $objects = @()
    foreach ($source in $sources) {
        $object = 'build_diagnostic\' + [System.IO.Path]::GetFileNameWithoutExtension($source) + '.obj'
        $objects += $object
        $commands += "cl $flags /Fo`"$object`" `"$source`""
        $commands += 'if errorlevel 1 exit /b 1'
    }
    $joined = ($objects | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $commands += "link /nologo /subsystem:efi_application /entry:UefiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /fixed:no /dynamicbase:no /nxcompat:no /out:build_diagnostic\GPU-TOPOLOGY-DIAGNOSTIC.EFI $joined"
    $commands += 'if errorlevel 1 exit /b 1'
    $batch = Join-Path $output 'build-diagnostic.cmd'
    Set-Content -LiteralPath $batch -Encoding ascii -Value $commands
    & $env:ComSpec /d /c $batch
    if ($LASTEXITCODE -ne 0) { throw "Diagnostic EFI build failed (exit=$LASTEXITCODE)" }
    $binary = Join-Path $output 'GPU-TOPOLOGY-DIAGNOSTIC.EFI'
    if (-not (Test-Path -LiteralPath $binary)) { throw 'No diagnostic EFI output produced' }
    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    Write-Output "STATUS=COMPLETE PATH=$binary SHA256=$hash READ_ONLY=1 SAVE_LOG=$([int]$SaveLog.IsPresent) WINDOWS_HANDOFF=$([int][bool]$WindowsEspGuid)"
} finally {
    Pop-Location
}
