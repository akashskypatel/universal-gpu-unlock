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
