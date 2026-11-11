param(
    [string]$VcVars,
    [string]$SelectionBdf,
    [string]$SelectionBdfs,
    [switch]$EnableSingleTargetExecution,
    [switch]$SyntheticCompileProfile
)
$ErrorActionPreference = 'Stop'
if (-not $SelectionBdf -and -not $SelectionBdfs) {
    throw 'Active firmware build refused: supply -SelectionBdf or -SelectionBdfs. Only preparation-only artifacts are supported.'
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
    if ($EnableSingleTargetExecution -and $selection.Count -ne 1) {
        throw "Single-target execution requires exactly one explicit -SelectionBdf"
    }
    if ($SyntheticCompileProfile -and -not $EnableSingleTargetExecution) {
        throw 'SyntheticCompileProfile is only valid with EnableSingleTargetExecution'
    }

    $sources = @(
        'dxe_main.c',
        'boot_io.c',
        'msi_release.c',
        'unlock_execution.c',
        'port.c',
        'compat.c',
        'checks.c',
        'gpu_inventory.c',
        'gpu_discovery.c',
        'gpu_windows_path.c',
        'profile.c',
        'sha256.c',
        'runtime.c',
        'format.c',
        'vendor\unlock_610.c',
        'vendor\prep_610.c',
        'vendor\layout_610.c',
        'vendor\vbios_610.c',
        'vendor\gfw_eval.c'
    )

    $objFiles = @()
    $compileCmds = @(
        '@echo off',
        "call `"$VcVars`" >nul",
        'if errorlevel 1 exit /b 1',
        "cd /d `"$PSScriptRoot`""
    )

    $clFlags = '/nologo /O2 /Oi /GS- /Gs65536 /Zl /c /TC /D__KERNEL__ /DGU_UEFI_PORT /DGU_AD104_TRANSACTION /DGU_ENTRY_LATCHED_SS1_FULL /D_CRT_SECURE_NO_WARNINGS /I. /Ibuild_dxe /Icompat /Ivendor /Ivendor\edk2\Include /Ivendor\edk2\Include\X64'

    if ($selection.Count) {
        if ($EnableSingleTargetExecution) {
            # Opt-in source/build validation only. Live hardware activation
            # still depends on target-specific ownership and recovery evidence.
            $clFlags += ' /DGU_ACTIVE_SINGLE_TARGET'
            if ($SyntheticCompileProfile) {
                # Compile every source path; the EFI entrypoint refuses
                # before any firmware, console, disk or GPU operation.
                $clFlags += ' /DGU_HOST_TEST_PROFILE /DGU_SYNTHETIC_COMPILE_ONLY'
            }
        } else {
            $clFlags += ' /DGU_ACTIVE_SEQUENCE_PREPARE_ONLY /DGU_HOST_TEST_PROFILE'
        }
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
    $binaryName = if ($SyntheticCompileProfile) { 'GPU-ONE-TARGET-COMPILE-ONLY-DXE-' + $selection.Label + '.efi' } elseif ($EnableSingleTargetExecution) { 'GPU-ONE-TARGET-EXPERIMENTAL-DXE-' + $selection.Label + '.efi' } elseif ($selection.Count) { 'GPU-ACTIVE-PREP-DXE-' + $selection.Label + '.efi' } else { 'AdaUnlockDxe.efi' }
    $compileCmds += "link /nologo /subsystem:efi_boot_service_driver /entry:AdaUnlockDriverEntryPoint /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /out:build_dxe\$binaryName $linkObjs"
    $compileCmds += 'if errorlevel 1 exit /b 1'

    $batchFile = Join-Path $buildDir 'build-dxe.cmd'
    Set-Content -LiteralPath $batchFile -Value $compileCmds -Encoding ascii

    & $env:ComSpec /d /c $batchFile
    if ($LASTEXITCODE -ne 0) { throw 'DXE Compilation failed' }

    $binary = Join-Path $buildDir $binaryName
    if (-not (Test-Path -LiteralPath $binary)) { throw 'DXE EFI output missing' }
    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    Write-Output "STATUS=COMPLETE PATH=$binary SHA256=$hash SELECTED_COUNT=$($selection.Count) PREPARATION_ONLY=$([int]($selection.Count -gt 0 -and -not $EnableSingleTargetExecution)) SINGLE_TARGET_ACTIVE=$([int][bool]$EnableSingleTargetExecution) MULTI_GPU_MUTATION_ALLOWED=0 SYNTHETIC_PROFILE=$([int][bool]$SyntheticCompileProfile) NOT_FOR_DEPLOYMENT=$([int][bool]$SyntheticCompileProfile)"
} finally {
    Pop-Location
}
