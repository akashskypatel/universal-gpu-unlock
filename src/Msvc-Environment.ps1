# Find the MSVC x64 environment. Keep the established explicit-path priority
# and the optional Visual Studio 2022 fallback search.
function Resolve-GuVcVars {
    param(
        [string]$VcVars,
        [switch]$SearchDefaultPaths
    )
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
    if (-not $VcVars -and $SearchDefaultPaths) {
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
        throw "Visual Studio x64 Build Tools not found. Please install Visual Studio 2022 C++ tools or pass -VcVars explicitly."
    }
    return $VcVars
}
