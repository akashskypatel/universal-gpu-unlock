# Common translation units for the GPU application and DXE driver.
# Keep the existing object order and distinct entrypoints.
$executionCoreSources = @(
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
    'boot_state.c'
)
$executionSupportSources = @(
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

# Generate compiler commands for all image builders. Each caller retains its
# own source inventory, compiler flags, linker subsystem and entrypoint.
function New-GuSourceCompilePlan {
    param(
        [string[]]$Sources,
        [string]$CompilerFlags,
        [string]$ObjectDirectory,
        [string]$VcVars,
        [string]$SourceDirectory,
        [switch]$QuotePaths
    )
    $objects = @()
    $commands = @(
        '@echo off',
        "call `"$VcVars`" >nul",
        'if errorlevel 1 exit /b 1',
        "cd /d `"$SourceDirectory`""
    )
    foreach ($source in $Sources) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($source.Replace('\', '/'))
        if ($source -like 'vendor\*') { $base = 'vendor_' + $base }
        $object = "$ObjectDirectory\$base.obj"
        $objects += $object
        $flags = $CompilerFlags
        if ($source -eq 'runtime.c') {
            $flags = $CompilerFlags -replace '(?<!\S)/Oi(?!\S)', '/Oi-'
        }
        if ($QuotePaths) {
            $commands += "cl $flags /Fo`"$object`" `"$source`""
        } else {
            $commands += "cl $flags /Fo$object $source"
        }
        $commands += 'if errorlevel 1 exit /b 1'
    }
    return @{ Commands = $commands; Objects = $objects }
}

# Use the same linker options for every EFI image. The caller chooses the
# subsystem and entrypoint and supplies only the objects it intends to link.
function New-GuLinkCommand {
    param(
        [string]$ImageFlags,
        [string]$OutputFile,
        [string[]]$Objects,
        [switch]$QuotePaths
    )
    $joined = if ($QuotePaths) { ($Objects | ForEach-Object { '"' + $_ + '"' }) -join ' ' } else { $Objects -join ' ' }
    "link /nologo $ImageFlags /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /out:$OutputFile $joined"
}

# Shared active-image build admission. This is not a runtime hardware waiver.
# Keep these checks before toolchain discovery and generated-header writes.
function Assert-GuExecutionBuildOptions {
    param(
        [switch]$EnableSingleTargetExecution,
        [switch]$SyntheticCompileProfile,
        [switch]$AcceptUnverifiedActuation,
        [switch]$StopReleaseOnlyBench,
        [switch]$RuntimeSelection,
        [switch]$RuntimeEsp,
        [switch]$EspResident,
        [string]$SelectionBdf,
        [string]$SelectionBdfs
    )
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
}
