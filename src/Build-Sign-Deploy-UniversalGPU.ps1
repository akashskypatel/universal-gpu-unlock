#Requires -Version 5.1
<#
.SYNOPSIS
Build, sign and (optionally) USB-stage every EFI image kind this repository can
produce, using a personal Secure Boot certificate that is already enrolled in
the BIOS db and signed inside a WSL instance.

.DESCRIPTION
The catalogue is docs/image_variants.json: variants across five builders
(diagnostic, active preparation, DXE preparation, original vendor Stop, boot-state maintenance). This
script executes that catalogue, so "all possible combinations" is data, not a
hand-maintained list, and tests/test_image_variants.py keeps the catalogue
honest against the builders.

Signing never leaves WSL: the private key stays in the selected WSL user's home
(~/secureboot-signing/uefi-db.key) and sbsign runs there. Windows only ever
hands WSL base64-encoded paths inside a base64-encoded bash payload, because
path-shaped arguments across the Windows/WSL boundary corrupt backslashes on
some setups. The script proves four things before it will sign: the WSL cert's
DER form equals the .cer you enrolled in the BIOS, the WSL key matches that
cert, the cert has not expired, and the unsigned image has not changed. After
signing it proves the image verifies with sbverify and is not byte-identical to
its input. When Get-SecureBootUEFI is available it also proves the certificate
is actually present in db - the "personal signature loaded into the BIOS" claim -
and refuses to sign if it is not. -SkipDbCheck records db_enrolled=UNKNOWN
instead of pretending.

Nothing here writes a GPU, an NVRAM variable, a Secure Boot key, firmware or
Git. Building and signing are safe; the images they produce are not all safe.
The catalogue labels each variant:

  read-only               the diagnostic family; never writes a GPU register
  refuses-before-mutation  links the active source, refuses before GuInit
  compile-only            proves the sources compile for your selection
  maintenance             removes only the reviewed boot record after confirmation; no GPU writes
  stop-only               calls the original vendor Stop; may hang
  actuates                performs waived hardware mutation; Stop/release-only and firmware-transaction variants are distinct

One named application variant can be staged to EFI\BOOT\BOOTX64.EFI on a
verified writable non-system FAT32 USB stick, after a verified local backup of
the file being replaced. Stop-only/actuating images also require
-AcceptActuatingDeploy. Maintenance also requires explicit consent and isolated USB media. A DXE driver (subsystem 11) is never staged as
BOOTX64.EFI. For the waived one-target app, -StageDxeCompanion additionally
builds, signs and stages its matching DXE image at
EFI\Drivers\AdaUnlockDxe.efi for the app's explicit I path.

.PARAMETER All
Build and sign every variant the catalogue permits with the arguments given.
Variants that need a GPU selection or a reviewed ESP GUID are skipped, with the
reason recorded, when you do not supply one. -All cannot be combined with
-StageDrive: a matrix run never stages anything.

.PARAMETER Only
One canonical profile with typed options (see -List). Cannot be combined with
-All or -Family. -ResolveOnly reports the selected
configuration without compiling, signing or staging.

.PARAMETER Family
diagnostic, prepare, prepare-dxe, stop-observation or maintenance. Selects a build matrix family, not an individual configuration.

.PARAMETER StageDrive
The USB drive letter to stage onto. This is the switch that stages: it takes the
one image -Only selected, builds it, signs it and copies it to
EFI\BOOT\BOOTX64.EFI after a verified backup. Anything actuating also needs
-AcceptActuatingDeploy, and a matrix run (-All) can never stage.

.PARAMETER StageDxeCompanion
For -Only firmware-transaction -Operation Menu with one selected BDF and the pinned ESP policy: build and sign the matching
`dxe-actuate-one-target` image, stage it at EFI\Drivers\AdaUnlockDxe.efi,
then stage the application at EFI\BOOT\BOOTX64.EFI. Requires -StageDrive and
-AcceptActuatingDeploy; the paired stage is rolled back if application staging fails.

.PARAMETER CapturedPromReference
Carry the two captured per-card firmware references
(src/prom_reference.c, GU_PROM_REFERENCE_CAPTURED) into the image that asks for
them, so GuAd104PromMatches(...,TRUE) can answer ACCEPTED for these two cards and
UNKNOWN_CARD for every other. Prepare family only: nothing else links a PROM read
behind this table. The receipt records it under Switches, and the image still
prints SIGNATURE_AUTHENTICATED=0 - a digest of what was read is not a signature of
what will run.

.PARAMETER SkipDbCheck
Do not prove the certificate is in the BIOS db. Records db_enrolled=UNKNOWN in
the receipt, so a later reader can tell the difference.

.PARAMETER List
Print the five canonical profiles and exit. Nothing is built, signed or staged.

.PARAMETER ListConfigurations
Print the complete build configuration matrix and exit. Nothing is built, signed or staged.

.EXAMPLE
# What can this repository build?
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -List

.EXAMPLE
# Stage 1 of the donor session: read-only diagnostic that saves its log.
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -SignOnly

.EXAMPLE
# Everything buildable without a selection or an ESP GUID, signed, not staged.
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -All -SkipDbCheck -SignOnly

.EXAMPLE
# Everything, signed, nothing staged.
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -All -SelectionBdfs '0000:01:00.0,0000:04:00.0' `
    -WindowsEspGuid 'a005858c-4487-4ee2-976e-1710ff484c31' -SignOnly

# Stage one image on G: -StageDrive is the staging switch, -Only names the image.
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -StageDrive G

.EXAMPLE
# The dual preparation image, carrying the two captured references.
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -Probe Preflight `
    -SelectionBdfs '0000:01:00.0,0000:04:00.0' -CapturedPromReference -SignOnly

.EXAMPLE
# Bootable Stop/release-only test application; stages as EFI\BOOT\BOOTX64.EFI.
# Confirm G: is the intended USB. No BDFs selects every supported NVIDIA GPU at boot.
# Uses runtime ESP discovery (no private boot profile). Requires the personal WSL signing key, certificate and enrolled db.
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only driver-transition -Operation StopReattach `
    -StageDrive G -AcceptActuatingDeploy

.EXAMPLE
# Optional fixed target: one BDF via -SelectionBdf or -SelectionBdfs, or an
# ordered list via -SelectionBdfs. Replace these placeholders before running.
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only driver-transition -Operation StopReattach `
    -SelectionBdf '<BDF>' -StageDrive G -AcceptActuatingDeploy

.EXAMPLE
# Optional fixed ordered sequence of multiple targets.
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only driver-transition -Operation StopReattach `
    -SelectionBdfs '<BDF1>,<BDF2>' -StageDrive G -AcceptActuatingDeploy

.EXAMPLE
# The waived one-target image, staged deliberately, on a donor card.
.\\src\\Build-Sign-Deploy-UniversalGPU.ps1 -Only firmware-transaction -Operation Menu -SelectionBdf '0000:01:00.0' `
    -StageDrive G -AcceptActuatingDeploy -StageDxeCompanion

.NOTES
Signed images are authentic, not safe. Archived bench runs exist, but signing
alone does not make a build hardware-verified; the recent U/P/I captures do not
verify the fixes in the current source. The
Windows 617.42 Booter provenance is external evidence that no build here produces.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputDirectory,
    [string]$PublicCertificate,
    [ValidatePattern('^[D-Zd-z]$')]
    [string]$StageDrive,
    [string]$SelectionBdf,
    [string]$SelectionBdfs,
    [string]$WindowsEspGuid,
    [string]$WslDistribution,
    [string]$WslUser,
    [string]$VcVars,
    [string]$Only,
    [ValidateSet('Topology','GopTextCycle','MsiRom','MsiFuse','MsiSec2','VramBlocks','Preflight')]
    [string]$Probe,
    [switch]$SaveLog,
    [ValidateSet('StopReattach','ObserveStop','UnlockReattach','Unlock','UnlockWithResume','Menu','DxeTransaction','InstallDriver','RemoveDriver','ReviewBootState')]
    [string]$Operation,
    [ValidateSet('Application','Dxe')]
    [string]$ImageKind,
    [ValidateSet('Return','WindowsAfterSuccess','Halt')]
    [string]$Handoff,
    [ValidateSet('RTX4090','RTX3080','BothNvidia')]
    [string]$ObservationTarget,
    [ValidateSet('Pinned','Runtime','Resident')]
    [string]$EspPolicy,
    [switch]$ResolveOnly,
    [ValidateSet('diagnostic', 'prepare', 'prepare-dxe', 'stop-observation', 'maintenance')]
    [string]$Family,
    [switch]$All,
    [switch]$SignOnly,
    [switch]$AcceptActuatingDeploy,
    [switch]$StageDxeCompanion,
    [switch]$CapturedPromReference,
    [switch]$SkipDbCheck,
    [switch]$List,
    [switch]$ListConfigurations
)

Set-StrictMode -Version Latest
# Record explicit profile options before entering functions.
$script:ExplicitProfileOptions = @()
foreach ($optionName in @('Probe','SaveLog','Operation','ImageKind','Handoff','ObservationTarget','EspPolicy')) {
    if ($PSBoundParameters.ContainsKey($optionName)) { $script:ExplicitProfileOptions += $optionName }
}
$script:RequestedCanonicalProfile = $null
$ErrorActionPreference = 'Stop'

$KeyInWsl = 'secureboot-signing/uefi-db.key'
$CertInWsl = 'secureboot-signing/uefi-db.crt'
# a5c059a1-94e4-4aa7-87b5-ab155c2bf072 in EFI_SIGNATURE_LIST byte order.
$X509SignatureTypeGuidBytes = [byte[]](0xa1, 0x59, 0xc0, 0xa5, 0xe4, 0x94, 0xa7, 0x4a,
                                       0x87, 0xb5, 0xab, 0x15, 0x5c, 0x2b, 0xf0, 0x72)

function Say([string]$Text) {
    # Write to the process stdout rather than Write-Host or Write-Output.
    # Write-Host writes through the host's own stream, so in a redirected log its
    # lines land at a different moment than the builder's stdout and splice into
    # it - that is the "output misaligns after the first variant" report. A
    # bare carriage return from a native tool would restart the line, and
    # Write-Output would be captured by every function here that returns a value
    # (that is how the signing exit code once turned into an array). Console.Out
    # is the same handle the builders write to and never enters the pipeline.
    [Console]::Out.WriteLine(($Text -replace "`r", ''))
}

function To-B64([string]$Value) {
    return [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Value))
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Assert-NormalDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Required directory missing: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing reparse/junction directory: $Path"
    }
}

function Assert-EfiImage([string]$Path, [int]$ExpectedSubsystem) {
    # x64 PE32+ subsystem 10 (EFI application) or 11 (boot-service driver).
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        try {
            if ($stream.Length -lt 256 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Missing DOS MZ header.' }
            [void]$stream.Seek(0x3c, [IO.SeekOrigin]::Begin)
            $pe = [int64]$reader.ReadUInt32()
            if ($pe -lt 64 -or $pe + 24 + 112 -gt $stream.Length) { throw 'Invalid PE header offset.' }
            [void]$stream.Seek($pe, [IO.SeekOrigin]::Begin)
            if ($reader.ReadUInt32() -ne 0x00004550) { throw 'Invalid PE signature.' }
            if ($reader.ReadUInt16() -ne 0x8664) { throw 'Not an AMD64 EFI image.' }
            [void]$stream.Seek($pe + 24, [IO.SeekOrigin]::Begin)
            if ($reader.ReadUInt16() -ne 0x20b) { throw 'Not a PE32+ x64 image.' }
            [void]$stream.Seek($pe + 24 + 68, [IO.SeekOrigin]::Begin)
            $subsystem = [int]$reader.ReadUInt16()
            if ($subsystem -ne $ExpectedSubsystem) {
                throw "EFI PE subsystem mismatch: expected=$ExpectedSubsystem observed=$subsystem"
            }
        } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}

function Assert-UsbVolume([string]$Drive) {
    $root = '{0}:\' -f $Drive.ToUpperInvariant()
    $partitions = @(Get-Partition -DriveLetter $Drive -ErrorAction Stop)
    $volumes = @(Get-Volume -DriveLetter $Drive -ErrorAction Stop)
    if ($partitions.Count -ne 1 -or $volumes.Count -ne 1) {
        throw "Expected exactly one partition/volume at $root"
    }
    $part = $partitions[0]
    $volume = $volumes[0]
    $disk = Get-Disk -Number $part.DiskNumber -ErrorAction Stop
    if ($disk.BusType -ne 'USB' -or $volume.FileSystem -ne 'FAT32' -or
        $part.IsBoot -or $part.IsSystem -or $disk.IsBoot -or $disk.IsSystem -or
        $disk.IsReadOnly -or $disk.IsOffline) {
        throw "Refusing unsafe volume $root (requires writable, non-system FAT32 USB data drive)."
    }
    Assert-NormalDirectory $root
    Say ("Target USB: {0} Disk={1} Model={2} FileSystem={3} Size={4}" -f
         $root, $part.DiskNumber, $disk.FriendlyName, $volume.FileSystem, $volume.Size)
    return $root
}

function Get-ExistingEfi([string]$Root) {
    # Walk explicitly: never recurse through a junction/reparse point.
    $stack = New-Object 'System.Collections.Generic.Stack[string]'
    $files = New-Object 'System.Collections.Generic.List[System.IO.FileInfo]'
    $stack.Push($Root)
    while ($stack.Count -gt 0) {
        $current = $stack.Pop()
        Assert-NormalDirectory $current
        foreach ($item in @(Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing USB reparse point (avoid following an external target): $($item.FullName)"
            }
            if ($item.PSIsContainer) { $stack.Push($item.FullName) }
            elseif ($item.Extension -ieq '.efi') { $files.Add($item) }
        }
    }
    return @($files.ToArray() | Sort-Object -Property FullName)
}

function Get-DbCertificateDer {
    # Read the firmware db and return the DER blobs of its X509 entries, or $null
    # when the platform cannot be asked (no Secure Boot, no admin, no cmdlet).
    try {
        $bytes = $null
        try {
            $bytes = Get-SecureBootUEFI -Name db -OutputAsByteArray -ErrorAction Stop
        } catch {
            $variable = Get-SecureBootUEFI -Name db -ErrorAction Stop
            if ($variable -and $variable.Bytes) { $bytes = [byte[]]$variable.Bytes }
        }
        if (-not $bytes -or $bytes.Length -lt 28) { return $null }
        $bytes = [byte[]]$bytes
        # EFI_SIGNATURE_LIST: type GUID, list size, header size, signature size.
        # The X509 type GUID is stored little-endian per field, so compare bytes.
        $x509Bytes = $X509SignatureTypeGuidBytes
        $entries = New-Object 'System.Collections.Generic.List[byte[]]'
        $offset = 0
        while ($offset + 28 -le $bytes.Length) {
            $typeBytes = New-Object byte[] 16
            [Array]::Copy($bytes, $offset, $typeBytes, 0, 16)
            $isX509 = $true
            for ($b = 0; $b -lt 16; $b++) {
                if ($typeBytes[$b] -ne $x509Bytes[$b]) { $isX509 = $false; break }
            }
            $listSize = [int][BitConverter]::ToUInt32($bytes, $offset + 16)
            $headerSize = [int][BitConverter]::ToUInt32($bytes, $offset + 20)
            $signatureSize = [int][BitConverter]::ToUInt32($bytes, $offset + 24)
            if ($listSize -le 28 -or $offset + $listSize -gt $bytes.Length) { break }
            if ($isX509 -and $signatureSize -gt 16) {
                $cursor = $offset + 28 + $headerSize
                $end = $offset + $listSize
                while ($cursor + $signatureSize -le $end) {
                    $der = New-Object byte[] ($signatureSize - 16)
                    [Array]::Copy($bytes, $cursor + 16, $der, 0, $der.Length)
                    $entries.Add($der)
                    $cursor += $signatureSize
                }
            }
            $offset += $listSize
        }
        return ,$entries
    } catch {
        return $null
    }
}

function Test-CertificateEnrolled([byte[]]$Der) {
    $entries = Get-DbCertificateDer
    if ($null -eq $entries) { return 'UNKNOWN' }
    foreach ($candidate in $entries) {
        if ($candidate.Length -ne $Der.Length) { continue }
        $same = $true
        for ($i = 0; $i -lt $Der.Length; $i++) {
            if ($candidate[$i] -ne $Der[$i]) { $same = $false; break }
        }
        if ($same) { return 'TRUE' }
    }
    return 'FALSE'
}

function Invoke-WslSign {
    param(
        [string]$SourceEfi,
        [string]$SignedEfi,
        [string]$ExpectedSha,
        [string]$PublicCer
    )
    # Paths cross the boundary only as base64 inside a base64 script: see the
    # .DESCRIPTION. The key and cert are read from the WSL user's home and are
    # never copied to Windows.
    $bashCode = @'
set -euo pipefail
for tool in base64 wslpath sha256sum openssl cmp sbsign sbverify; do
  command -v "$tool" >/dev/null || { echo "Missing WSL tool: $tool" >&2; exit 2; }
done
resolve_winpath() {
  local value="$1" label="$2" mapped
  if ! mapped="$(wslpath -u "$value" 2>/dev/null)" || [ -z "$mapped" ]; then
    echo "WSL cannot translate $label path: $value. Check WSL drive automount and /etc/wsl.conf." >&2
    exit 3
  fi
  printf '%s' "$mapped"
}
source_efi="$(resolve_winpath "$source_windows" 'unsigned EFI')"
output_efi="$(resolve_winpath "$output_windows" 'signed output')"
public_efi="$(resolve_winpath "$public_windows" 'public certificate')"
key="$HOME/__KEY__"
cert="$HOME/__CERT__"
[ -f "$source_efi" ] || { echo "WSL cannot access unsigned EFI: $source_efi" >&2; exit 3; }
[ -d "$(dirname "$output_efi")" ] || { echo "WSL cannot access signed output directory: $(dirname "$output_efi")" >&2; exit 3; }
[ -f "$key" ] || { echo "Missing WSL signing key: $key" >&2; exit 4; }
[ -f "$cert" ] || { echo "Missing WSL signing certificate: $cert" >&2; exit 4; }
[ -f "$public_efi" ] || { echo "WSL cannot access public signing certificate: $public_efi" >&2; exit 4; }
openssl x509 -in "$cert" -outform DER | cmp -s - "$public_efi" || { echo 'Public certificate mismatch; signing refused' >&2; exit 4; }
openssl x509 -in "$cert" -noout -checkend 0 >/dev/null || { echo 'Certificate invalid/expired' >&2; exit 4; }
key_sha="$(openssl pkey -in "$key" -pubout -outform DER | sha256sum | cut -d ' ' -f 1)"
cert_sha="$(openssl x509 -in "$cert" -pubkey -noout | openssl pkey -pubin -outform DER | sha256sum | cut -d ' ' -f 1)"
[ "$key_sha" = "$cert_sha" ] || { echo 'Key does not match certificate' >&2; exit 4; }
actual_sha="$(sha256sum "$source_efi" | cut -d ' ' -f 1)"
[ "$actual_sha" = "$expected_sha" ] || { echo 'Unsigned input changed before signing' >&2; exit 5; }
[ ! -e "$output_efi" ] || { echo 'Signed destination already exists' >&2; exit 5; }
sbsign --key "$key" --cert "$cert" --output "$output_efi" "$source_efi"
sbverify --cert "$cert" "$output_efi"
actual_sha="$(sha256sum "$source_efi" | cut -d ' ' -f 1)"
[ "$actual_sha" = "$expected_sha" ] || { echo 'Unsigned input changed during signing' >&2; exit 5; }
openssl x509 -in "$cert" -noout -subject -fingerprint -sha256
'@
    $bashCode = $bashCode.Replace('__KEY__', $KeyInWsl).Replace('__CERT__', $CertInWsl)
    $preamble = @(
        'source_windows=$(printf %s ' + (To-B64 $SourceEfi) + ' | base64 -d)'
        'output_windows=$(printf %s ' + (To-B64 $SignedEfi) + ' | base64 -d)'
        ('expected_sha=$(printf %s ' + (To-B64 $ExpectedSha) + ' | base64 -d)')
        'public_windows=$(printf %s ' + (To-B64 $PublicCer) + ' | base64 -d)'
    ) -join "`n"
    $payload = ("set -euo pipefail`n" + $preamble + "`n" + $bashCode).Replace("`r`n", "`n").Replace("`r", "`n")
    if ($payload.Contains("`r")) { throw 'WSL signing payload still contains carriage returns.' }
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($payload))
    $command = 'set -o pipefail; printf %s ' + $encoded + ' | base64 -d | bash'
    $prefix = @()
    if ($WslDistribution) { $prefix += @('-d', $WslDistribution) }
    if ($WslUser) { $prefix += @('-u', $WslUser) }
    # wsl.exe writes sbverify and openssl output to the success stream. Capture
    # it, print it as host output, and return ONLY the exit code: a `return
    # $LASTEXITCODE` next to a live pipeline gives the caller an array such as
    # @('Signature verification OK', 'subject=...', 0), which never equals zero
    # and makes a good signature look like a failure.
    # Windows PowerShell 5.1 turns a native command's stderr into error records,
    # and $ErrorActionPreference='Stop' then terminates on the first one. sbsign
    # prints its "Signing Unsigned original image" banner on stderr, so a good
    # signature used to abort the variant with that banner as the message. This
    # function reports failure through the exit code alone.
    $restorePreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $wslOutput = & wsl.exe @prefix --exec bash -c $command 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $restorePreference
    }
    foreach ($line in @($wslOutput)) { Say ([string]$line) }
    if ($null -eq $exitCode) { throw 'wsl.exe reported no exit code, so the signature was never proven.' }
    return [int]$exitCode
}

function Test-WslSigningReady {
    param([string]$PublicCer)
    # One round-trip before anything is built: which distribution, which user,
    # which home, and whether every tool and both key files are actually there.
    # A wrong -WslDistribution or -WslUser used to surface as a failed signature
    # on the first image, after every build in the run had already happened.
    # No `set -e`: every gap is reported, not just the first one.
    $bashCode = @'
set -o pipefail
status=0
tools=""
for tool in base64 sha256sum openssl cmp sbsign sbverify; do
  if command -v "$tool" >/dev/null 2>&1; then
    tools="$tools $tool"
  else
    echo "WSL_SIGNING_MISSING tool=$tool"
    status=2
  fi
done
key="$HOME/__KEY__"
cert="$HOME/__CERT__"
key_ok=no
cert_ok=no
cer_match=no
cert_valid=no
if [ -f "$key" ]; then key_ok=yes; else echo "WSL_SIGNING_MISSING key=$key"; status=4; fi
if [ -f "$cert" ]; then cert_ok=yes; else echo "WSL_SIGNING_MISSING cert=$cert"; status=4; fi
if [ "$cert_ok" = yes ]; then
  if [ -n "${public_efi:-}" ] && openssl x509 -in "$cert" -outform DER 2>/dev/null | cmp -s - "$public_efi"; then
    cer_match=yes
  else
    echo "WSL_SIGNING_MISMATCH cer=$public_windows is not the DER form of $cert"
    status=4
  fi
  if openssl x509 -in "$cert" -noout -checkend 0 >/dev/null 2>&1; then
    cert_valid=yes
  else
    echo "WSL_SIGNING_MISMATCH cert=$cert is expired or unreadable"
    status=4
  fi
fi
if [ "$key_ok" = yes ] && [ "$cert_ok" = yes ]; then
  key_sha="$(openssl pkey -in "$key" -pubout -outform DER 2>/dev/null | sha256sum | cut -d ' ' -f 1)"
  cert_sha="$(openssl x509 -in "$cert" -pubkey -noout 2>/dev/null | openssl pkey -pubin -outform DER 2>/dev/null | sha256sum | cut -d ' ' -f 1)"
  if [ -n "$key_sha" ] && [ "$key_sha" = "$cert_sha" ]; then
    key_ok=yes
  else
    echo "WSL_SIGNING_MISMATCH key=$key does not match $cert"
    key_ok=no
    status=4
  fi
fi
if [ "$status" -eq 0 ]; then verdict=WSL_SIGNING_READY; else verdict=WSL_SIGNING_BROKEN; fi
echo "$verdict distro=${WSL_DISTRO_NAME:-unknown} user=$(id -un 2>/dev/null || echo unknown) home=$HOME key=$key cert=$cert cer_match=$cer_match key_match=$key_ok cert_valid=$cert_valid tools=${tools# }"
exit "$status"
'@
    $bashCode = $bashCode.Replace('__KEY__', $KeyInWsl).Replace('__CERT__', $CertInWsl)
    $preamble = @(
        'public_windows=$(printf %s ' + (To-B64 $PublicCer) + ' | base64 -d)'
        'public_efi=$(wslpath -u "$public_windows" 2>/dev/null || true)'
    ) -join "`n"
    $payload = ($preamble + "`n" + $bashCode).Replace("`r`n", "`n").Replace("`r", "`n")
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($payload))
    $command = 'set -o pipefail; printf %s ' + $encoded + ' | base64 -d | bash'
    $prefix = @()
    if ($WslDistribution) { $prefix += @('-d', $WslDistribution) }
    if ($WslUser) { $prefix += @('-u', $WslUser) }
    # Same capture rule as the signing call: local 'Continue' so a stderr line
    # cannot terminate the run, and the exit code is read on its own.
    $restorePreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & wsl.exe @prefix --exec bash -c $command 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $restorePreference
    }
    foreach ($line in @($output)) { Say ([string]$line) }
    if ($null -eq $code) { throw 'wsl.exe reported no exit code during the signing preflight.' }
    if ([int]$code -ne 0) {
        $notReady = ('WSL signing is not ready (exit {0}): fix the WSL_SIGNING_* lines above. The key and ' +
                     'certificate live in the home of the user that signs, so pass -WslDistribution and ' +
                     '-WslUser to name that user, or put {1} and {2} in the default user''s home.')
        throw ($notReady -f [int]$code, $KeyInWsl, $CertInWsl)
    }
    return [int]$code
}

function Stage-SignedEfi {
    param([string]$Drive, [string]$Root, [string]$SignedFile, [string]$SignedSha256,
          [string]$LocalOutput, [string]$ExpectedVolumeId, [switch]$MaintenanceOnly)
    if ((Assert-UsbVolume $Drive) -ne $Root) { throw 'Drive letter identity changed.' }
    if ([string](Get-Volume -DriveLetter $Drive -ErrorAction Stop).UniqueId -ne $ExpectedVolumeId) {
        throw 'Target USB volume identity changed before staging.'
    }
    if ([IO.Path]::GetPathRoot($SignedFile).TrimEnd('\') -ieq $Root.TrimEnd('\') -or
        [IO.Path]::GetPathRoot($LocalOutput).TrimEnd('\') -ieq $Root.TrimEnd('\')) {
        throw 'Signed source and local backups must not reside on the USB destination drive.'
    }
    if ((Get-Sha256 $SignedFile) -ne $SignedSha256) { throw 'Signed EFI hash changed before staging.' }

    $efiRoot = Join-Path $Root 'EFI'
    $bootDir = Join-Path $efiRoot 'BOOT'
    foreach ($dir in @($efiRoot, $bootDir)) {
        if (-not (Test-Path -LiteralPath $dir)) { [void](New-Item -ItemType Directory -Path $dir -ErrorAction Stop) }
        Assert-NormalDirectory $dir
    }
    $destination = Join-Path $bootDir 'BOOTX64.EFI'
    if ((Test-Path -LiteralPath $destination) -and -not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        throw "Refusing non-file EFI destination: $destination"
    }
    $allEfi = @(Get-ExistingEfi $Root)
    $oldBootPresent = Test-Path -LiteralPath $destination -PathType Leaf
    $otherEfiCount = @($allEfi | Where-Object { $_.FullName -ine $destination }).Count
    if ($MaintenanceOnly -and $otherEfiCount -ne 0) {
        throw 'Maintenance requires isolated USB media: other EFI files exist. Use separate media; no files were removed.'
    }
    Say "Other EFI files preserved on USB: $otherEfiCount"
    Say "Install only: $destination"

    $backup = $null; $oldSha = $null; $savedOld = $null
    if ($oldBootPresent) {
        Say 'Existing BOOTX64.EFI will be replaced after a local verified backup.'
        $answer = Read-Host 'Type YES/Yes/yes/y/Y to replace only USB BOOTX64.EFI (all other EFI files stay)'
        if ($answer -cne 'YES' -and $answer -cne 'Yes' -and $answer -cne 'yes' -and $answer -cne 'y' -and $answer -cne 'Y') {
            throw 'BOOTX64.EFI replacement cancelled; no existing USB EFI was removed.'
        }
        $backup = Join-Path $LocalOutput ('efi-usb-backup-{0}-{1}' -f
            [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfff'), [guid]::NewGuid().ToString('N'))
        [void](New-Item -ItemType Directory -Path $backup -ErrorAction Stop)
        $savedOld = Join-Path $backup 'BOOTX64.EFI'
        $oldSha = Get-Sha256 $destination
        Copy-Item -LiteralPath $destination -Destination $savedOld -ErrorAction Stop
        if ((Get-Sha256 $savedOld) -ne $oldSha -or (Get-Sha256 $destination) -ne $oldSha) {
            throw 'Existing BOOTX64.EFI backup verification failed; existing EFI not removed.'
        }
        Say "Verified original USB BOOTX64.EFI backup: $savedOld"
    }

    $temporary = Join-Path $bootDir ('.new-efi-{0}.tmp' -f [guid]::NewGuid().ToString('N'))
    $replacementStarted = $false
    try {
        if ((Assert-UsbVolume $Drive) -ne $Root -or
            [string](Get-Volume -DriveLetter $Drive -ErrorAction Stop).UniqueId -ne $ExpectedVolumeId) {
            throw 'Target USB identity changed during staging.'
        }
        if ($MaintenanceOnly -and @(Get-ExistingEfi $Root | Where-Object { $_.FullName -ine $destination }).Count -ne 0) {
            throw 'Maintenance media is no longer isolated; no original EFI was removed.'
        }
        if ((Get-Sha256 $SignedFile) -ne $SignedSha256) { throw 'Signed EFI hash changed before USB copy.' }
        Copy-Item -LiteralPath $SignedFile -Destination $temporary -ErrorAction Stop
        if ((Get-Sha256 $temporary) -ne $SignedSha256) {
            throw 'Staged temporary EFI hash mismatch; original BOOTX64.EFI preserved.'
        }
        $bootStillPresent = Test-Path -LiteralPath $destination -PathType Leaf
        if ([bool]$bootStillPresent -ne [bool]$oldBootPresent) {
            throw 'USB BOOTX64.EFI presence changed during staging; no original removed.'
        }
        if ($oldBootPresent -and (Get-Sha256 $destination) -ne $oldSha) {
            throw 'USB BOOTX64.EFI changed during staging; original preserved.'
        }
        $replacementStarted = $true
        if ($oldBootPresent) { Remove-Item -LiteralPath $destination -Force -ErrorAction Stop }
        Move-Item -LiteralPath $temporary -Destination $destination -ErrorAction Stop
        if ((Get-Sha256 $destination) -ne $SignedSha256) { throw 'Deployed BOOTX64.EFI hash mismatch.' }
        return [pscustomobject]@{
            Status = 'DEPLOYED'; Path = $destination; ReplacedBootEfi = [bool]$oldBootPresent
            PreviousEfiCount = $allEfi.Count; PreservedOtherEfiCount = $otherEfiCount
            BackupDirectory = $backup; SignedSha256 = $SignedSha256
        }
    } catch {
        $originalError = $_.Exception.Message
        if ($replacementStarted) {
            $recoveryErrors = New-Object 'System.Collections.Generic.List[string]'
            try {
                if (Test-Path -LiteralPath $destination -PathType Leaf) {
                    Remove-Item -LiteralPath $destination -Force -ErrorAction Stop
                }
            } catch { $recoveryErrors.Add("new BOOTX64.EFI removal: $($_.Exception.Message)") }
            if ($oldBootPresent) {
                try {
                    Copy-Item -LiteralPath $savedOld -Destination $destination -ErrorAction Stop
                    if ((Get-Sha256 $destination) -ne $oldSha) { throw 'Restored BOOTX64.EFI hash mismatch.' }
                } catch { $recoveryErrors.Add("BOOTX64.EFI restore: $($_.Exception.Message)") }
            }
            if ($recoveryErrors.Count) {
                throw "USB staging error: $originalError. ROLLBACK INCOMPLETE: $($recoveryErrors -join '; '). Backup: $backup"
            }
            throw "USB staging error: $originalError. Original BOOTX64.EFI restored when present; local backup: $backup"
        }
        throw
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
    }
}

# --- catalogue -------------------------------------------------------------

$cataloguePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'docs\image_variants.json'
$cataloguePath = [IO.Path]::GetFullPath($cataloguePath)
if (-not (Test-Path -LiteralPath $cataloguePath -PathType Leaf)) {
    throw "Variant catalogue missing: $cataloguePath"
}
$catalogue = Get-Content -LiteralPath $cataloguePath -Raw -Encoding UTF8 | ConvertFrom-Json
$familyById = @{}
foreach ($familySpec in $catalogue.families) { $familyById[$familySpec.id] = $familySpec }
$script:CanonicalProfiles = @($catalogue.profiles | ForEach-Object { $_.id })

# Translate only supported combinations to existing catalogue entries.
# No new firmware policy, builder waiver or runtime operation is introduced here.
function Assert-ProfileOptions([string[]]$Allowed) {
    foreach ($optionName in $script:ExplicitProfileOptions) {
        if ($Allowed -notcontains $optionName) {
            throw "Profile $script:RequestedCanonicalProfile does not accept -$optionName."
        }
    }
}

# Select by the exact declared build contract. Configuration IDs are output
# metadata only; a missing or ambiguous declaration must refuse the request.
function Find-ProfileConfiguration {
    param([string]$ProfileName, [string]$BuilderFamily, [string]$Safety,
          [hashtable]$Switches, [string]$Selection = '',
          [switch]$RequiresLocalBootProfile)
    $matches = @($catalogue.variants | Where-Object {
        $candidate = $_
        if ($candidate.profile -ne $ProfileName -or $candidate.family -ne $BuilderFamily -or
            $candidate.safety -ne $Safety) { return $false }
        $selectionProperty = $candidate.PSObject.Properties['requires_selection']
        $selectionMode = if ($selectionProperty) { [string]$selectionProperty.Value } else { '' }
        $espProperty = $candidate.PSObject.Properties['requires_esp_guid']
        $requiresEsp = $null -ne $espProperty -and $espProperty.Value -eq $true
        $localProperty = $candidate.PSObject.Properties['requires_local_boot_profile']
        $requiresLocal = $null -ne $localProperty -and $localProperty.Value -eq $true
        if ($selectionMode -ne $Selection -or $requiresEsp -ne $Switches.ContainsKey('WindowsEspGuid') -or
            $requiresLocal -ne [bool]$RequiresLocalBootProfile) { return $false }
        $properties = @($candidate.switches.PSObject.Properties | ForEach-Object { $_ })
        if ($properties.Count -ne $Switches.Count) { return $false }
        foreach ($key in $Switches.Keys) {
            $property = $candidate.switches.PSObject.Properties[$key]
            if ($null -eq $property -or $null -eq $property.Value -or
                $property.Value.GetType() -ne $Switches[$key].GetType() -or
                $property.Value -cne $Switches[$key]) { return $false }
        }
        return $true
    })
    if ($matches.Count -ne 1) {
        throw "Profile $ProfileName must resolve to exactly one build configuration; found $($matches.Count)."
    }
    return $matches[0]
}

function Resolve-ProfileVariant([string]$ProfileName) {
    $targetCount = Get-SelectionCount
    $isDxe = $ImageKind -eq 'Dxe'
    $builderFamily = if ($isDxe) { 'prepare-dxe' } else { 'prepare' }
    $hasEsp = [bool]$WindowsEspGuid
    $selection = if ($targetCount -eq 0) { '' } elseif ($targetCount -eq 1) { 'single' } else { 'sequence' }
    switch ($ProfileName) {
        'diagnostic' {
            if ($Probe -eq 'Preflight') {
                Assert-ProfileOptions @('Probe','SaveLog','ImageKind')
                if (-not $targetCount -or $hasEsp -or ($isDxe -and $CapturedPromReference) -or
                    ($script:ExplicitProfileOptions -contains 'SaveLog' -and -not $SaveLog)) {
                    throw 'Preflight requires explicit GPU selection and file logging, without Windows handoff. Firmware references require the application.'
                }
                return (Find-ProfileConfiguration $ProfileName $builderFamily 'refuses-before-mutation' @{} -Selection 'any')
            }
            Assert-ProfileOptions @('Probe','SaveLog')
            if ($CapturedPromReference) { throw 'Diagnostic does not accept firmware reference options.' }
            $probeMode = if ($Probe) { $Probe } else { 'Topology' }
            if ($script:ExplicitProfileOptions -contains 'SaveLog' -and -not $SaveLog) {
                if ($probeMode -ne 'Topology' -or $targetCount -or $hasEsp) { throw 'Console-only diagnostic supports topology without selection or handoff.' }
                return (Find-ProfileConfiguration $ProfileName 'diagnostic' 'read-only' @{})
            }
            $switches = @{SaveLog=$true}
            if ($probeMode -eq 'Topology') {
                if ($targetCount -and $hasEsp) { throw 'Selected diagnostic cannot include Windows handoff.' }
                if ($targetCount) { return (Find-ProfileConfiguration $ProfileName 'diagnostic' 'read-only' $switches -Selection 'any') }
            } else {
                if ($targetCount) { throw 'Special diagnostic probes do not accept a compiled GPU selection.' }
                if ($probeMode -eq 'GopTextCycle' -and $hasEsp) { throw 'GOP text cycle cannot include Windows handoff.' }
                $probeSwitch = @{GopTextCycle='CycleGopText'; MsiRom='MsiB850RomVariables'; MsiFuse='MsiB850FuseVersion'; MsiSec2='MsiB850Sec2Ucode1'; VramBlocks='VramBlockProbe'}[$probeMode]
                $switches[$probeSwitch] = $true
            }
            if ($hasEsp) { $switches['WindowsEspGuid'] = '<esp>' }
            return (Find-ProfileConfiguration $ProfileName 'diagnostic' 'read-only' $switches)
        }
        'compile-check' {
            Assert-ProfileOptions @('ImageKind')
            if (-not $targetCount -or $hasEsp -or $CapturedPromReference) {
                throw 'Compile-check requires a compiled GPU selection and no ESP or firmware reference options.'
            }
            return (Find-ProfileConfiguration $ProfileName $builderFamily 'compile-only' @{SyntheticCompileProfile=$true} -Selection $selection)
        }
        'driver-transition' {
            if ($CapturedPromReference) { throw 'Driver transition does not accept firmware reference options.' }
            if ($Operation -eq 'StopReattach') {
                Assert-ProfileOptions @('Operation','ImageKind','Handoff')
                if ($hasEsp -or $Handoff -eq 'Halt') { throw 'StopReattach supports Return or WindowsAfterSuccess, without a compiled ESP GUID.' }
                $switches = @{EnableSingleTargetExecution=$true; AcceptUnverifiedActuation=$true; StopReleaseOnlyBench=$true; FileOnlyBootLog=$true; RuntimeSelection=$true}
                if (-not $isDxe) { $switches['RuntimeEsp'] = $true }
                if ($Handoff -eq 'WindowsAfterSuccess') {
                    if ($isDxe -or $targetCount) { throw 'StopReattach Windows handoff requires the live-all application.' }
                    $switches['StopReleaseWindowsHandoff'] = $true
                }
                return (Find-ProfileConfiguration $ProfileName $builderFamily 'actuates' $switches -RequiresLocalBootProfile:$isDxe)
            }
            if ($Operation -eq 'ObserveStop') {
                Assert-ProfileOptions @('Operation','ImageKind','Handoff','ObservationTarget')
                if ($isDxe -or $targetCount -or -not $ObservationTarget) { throw 'Stop observation requires an application and an explicit observation target, not a BDF list.' }
                if ($Handoff -eq 'Return' -or ($Handoff -eq 'Halt' -and $hasEsp) -or ($Handoff -eq 'WindowsAfterSuccess' -and -not $hasEsp)) {
                    throw 'Stop observation handoff requires a pinned ESP GUID. Halt does not accept an ESP GUID.'
                }
                $switches = @{AcceptUnboundedStopRisk=$true}
                if ($ObservationTarget -eq 'BothNvidia') {
                    if (-not $hasEsp) { throw 'Dual Stop observation requires pinned Windows handoff.' }
                    $switches['BothNvidia'] = $true
                } elseif ($ObservationTarget -eq 'RTX3080') {
                    $switches['Target3080'] = $true
                }
                if ($hasEsp) {
                    $switches['WindowsEspGuid'] = '<esp>'
                    $switches['AcceptWindowsHandoffRisk'] = $true
                }
                return (Find-ProfileConfiguration $ProfileName 'stop-observation' 'stop-only' $switches)
            }
            throw 'Driver transition requires -Operation StopReattach or ObserveStop.'
        }
        'firmware-transaction' {
            Assert-ProfileOptions @('Operation','ImageKind','EspPolicy')
            $switches = @{EnableSingleTargetExecution=$true; AcceptUnverifiedActuation=$true}
            if ($Operation -in @('UnlockReattach','Unlock','UnlockWithResume')) {
                if ($isDxe -or $targetCount -or ($EspPolicy -and $EspPolicy -ne 'Runtime')) {
                    throw 'Fixed firmware operations require the runtime-selection application and runtime ESP policy.'
                }
                if ($hasEsp -and $Operation -ne 'UnlockReattach') { throw 'Only UnlockReattach accepts an explicit ESP GUID.' }
                $switches['RuntimeSelection'] = $true
                $switches['RuntimeEsp'] = $true
                $switches['ApplicationOperation'] = if ($Operation -eq 'UnlockReattach') { 'Unlock' } else { $Operation }
                if ($Operation -eq 'UnlockReattach') { $switches['FirmwareReattach'] = $true }
                return (Find-ProfileConfiguration $ProfileName $builderFamily 'actuates' $switches)
            }
            if (($Operation -eq 'Menu' -and -not $isDxe) -or ($Operation -eq 'DxeTransaction' -and $isDxe)) {
                if ($hasEsp -or $EspPolicy -eq 'Runtime' -or ($isDxe -and ($EspPolicy -eq 'Resident' -or $CapturedPromReference))) {
                    throw 'This firmware configuration does not support the requested ESP or reference options.'
                }
                if (-not $targetCount) { $switches['RuntimeSelection'] = $true }
                if ($EspPolicy -eq 'Resident') {
                    if ($targetCount -gt 1) { throw 'Resident sequence is not an existing supported configuration.' }
                    $switches['RuntimeEsp'] = $true
                    $switches['EspResident'] = $true
                }
                return (Find-ProfileConfiguration $ProfileName $builderFamily 'actuates' $switches -Selection $selection -RequiresLocalBootProfile:($EspPolicy -ne 'Resident'))
            }
            throw 'Firmware transaction requires an explicit supported operation and image kind.'
        }
        'maintenance' {
            Assert-ProfileOptions @('Operation','ImageKind')
            if ($isDxe -or $targetCount -or $hasEsp -or $CapturedPromReference -or $StageDxeCompanion) {
                throw 'Maintenance requires an application without compiled selection, ESP pin, references or companion staging.'
            }
            if ($Operation -eq 'ReviewBootState') { return (Find-ProfileConfiguration $ProfileName 'maintenance' 'maintenance' @{}) }
            if ($Operation -in @('InstallDriver','RemoveDriver')) {
                return (Find-ProfileConfiguration $ProfileName $builderFamily 'actuates' @{RuntimeSelection=$true; RuntimeEsp=$true; EnableSingleTargetExecution=$true; AcceptUnverifiedActuation=$true; ApplicationOperation=$Operation})
            }
            throw 'Maintenance requires an explicit supported operation.'
        }
    }
    throw "Unknown canonical profile: $ProfileName"
}

function Select-Variants {
    $selected = @($catalogue.variants)
    if ($Only) {
        if ($Family) { throw 'Select exactly one canonical profile without -Family.' }
        if ($script:CanonicalProfiles -notcontains $Only) {
            throw "Unknown profile: $Only (see -List)."
        }
        $script:RequestedCanonicalProfile = $Only
        $selected = @(Resolve-ProfileVariant $Only)
    } elseif ($script:ExplicitProfileOptions.Count) {
        throw 'Profile options require -Only <profile>.'
    }
    if ($Family) { $selected = @($selected | Where-Object { $_.family -eq $Family }) }
    if (-not $selected.Count) { throw 'No variant matched the selection.' }
    return $selected
}

function Get-SelectionCount {
    return $script:SelectionSpec.Count
}

# Behavior follows the configured builder switches, not the configuration ID.
function Test-VariantSwitch($Variant, [string]$Name) {
    $property = $Variant.switches.PSObject.Properties[$Name]
    return ($null -ne $property -and $property.Value -is [bool] -and $property.Value)
}

# Paired deployment is restricted to the original pinned, single-target,
# waived transaction. No additional builder operation switches are permitted.
function Test-PairedTransactionVariant($Variant, [string]$Family) {
    return ($Variant.family -eq $Family -and
            $Variant.profile -eq 'firmware-transaction' -and
            $Variant.safety -eq 'actuates' -and
            $Variant.PSObject.Properties.Name -contains 'requires_selection' -and
            $Variant.requires_selection -eq 'single' -and
            $Variant.PSObject.Properties.Name -contains 'requires_local_boot_profile' -and
            $Variant.requires_local_boot_profile -eq $true -and
            @($Variant.switches.PSObject.Properties.Name).Count -eq 2 -and
            (Test-VariantSwitch $Variant 'EnableSingleTargetExecution') -and
            (Test-VariantSwitch $Variant 'AcceptUnverifiedActuation'))
}

function Test-CompiledStopSelection($Variant) {
    return ((Get-SelectionCount) -ge 1 -and
            (Test-VariantSwitch $Variant 'StopReleaseOnlyBench') -and
            (Test-VariantSwitch $Variant 'RuntimeSelection') -and
            -not (Test-VariantSwitch $Variant 'StopReleaseWindowsHandoff'))
}

function Test-VariantSelection($Variant) {
    # Returns a skip reason when the operator's arguments cannot build this
    # variant, or $null when they can. A single-target variant under a two-target
    # selection is a skip, not a failure: the builder would produce the sequence
    # image under a name this catalogue gives to a different variant.
    $count = Get-SelectionCount
    if ((Test-VariantSwitch $Variant 'StopReleaseWindowsHandoff') -and $count -gt 0) { return 'NEEDS_RUNTIME_SELECTION' }
    $needs = $null
    if ($Variant.PSObject.Properties.Name -contains 'requires_selection') { $needs = $Variant.requires_selection }
    if ($needs -eq 'single' -and $count -ne 1) {
        if ($count -eq 0) { return 'NEEDS_SELECTION' }
        return 'NEEDS_EXACTLY_ONE_BDF'
    }
    if ($needs -eq 'sequence' -and $count -lt 2) { return 'NEEDS_TWO_OR_MORE_BDFS' }
    if ($needs -eq 'any' -and $count -lt 1) { return 'NEEDS_SELECTION' }
    $needsEsp = $false
    if ($Variant.PSObject.Properties.Name -contains 'requires_esp_guid') { $needsEsp = [bool]$Variant.requires_esp_guid }
    if ($needsEsp -and -not $WindowsEspGuid) { return 'NEEDS_ESP_GUID' }
    # A waived build cannot use the synthetic host-test GUID: src/boot_profile.h
    # includes the private local/boot_profile.h and stops with an #error when it
    # is absent. That is a setup step, not a defect in the image.
    $needsPinnedProfile = $false
    if ($Variant.PSObject.Properties.Name -contains 'requires_local_boot_profile') {
        $needsPinnedProfile = [bool]$Variant.requires_local_boot_profile
    }
    if ($needsPinnedProfile -and -not $script:HasLocalBootProfile) { return 'NEEDS_LOCAL_BOOT_PROFILE' }
    return $null
}

function Get-VariantBinary($Variant) {
    if (Test-CompiledStopSelection $Variant) {
        $label = [string]$script:SelectionSpec.Label
        if ($Variant.family -eq 'prepare-dxe') {
            return "GPU-SEQUENCE-STOP-RELEASE-BENCH-DXE-$label.efi"
        }
        return "GPU-SEQUENCE-STOP-RELEASE-BENCH-$label.EFI"
    }
    # The label is whatever src/Gpu-Selection-Build.ps1 would compute, so the
    # expectation and the builder cannot use different spellings.
    $label = ''
    if ($null -ne $script:SelectionSpec) { $label = [string]$script:SelectionSpec.Label }
    return $Variant.binary.Replace('<label>', $label)
}

function Get-ResolvedAttestations($Variant) {
    foreach ($declared in $Variant.attestations) {
        $attestation = $declared.Replace('<selection-count>', [string](Get-SelectionCount))
        if (Test-CompiledStopSelection $Variant) {
            $attestation = $attestation.Replace('RUNTIME_SELECTION=1', 'RUNTIME_SELECTION=0')
            $attestation = $attestation.Replace('TARGET_ORDER=RUNTIME_SUPPORTED_NVIDIA_ENUMERATION',
                ('TARGET_ORDER=' + ($script:SelectionSpec.Locations -join ',')))
        }
        if ($attestation -match '<[^>]+>') {
            throw "Unknown attestation placeholder for $($Variant.id): $declared"
        }
        $attestation
    }
    if (Test-VariantSwitch $Variant 'FirmwareReattach') {
        $expectedEsp = if ($WindowsEspGuid) { (ConvertTo-GuWindowsEspGuid $WindowsEspGuid).ToString('D') } else { 'NONE' }
        "WINDOWS_ESP_GUID=$expectedEsp"
    }
}

function Build-VariantArguments($Variant) {
    $arguments = @{}
    foreach ($property in $Variant.switches.PSObject.Properties) {
        $name = $property.Name
        $value = $property.Value
        if ($value -is [string] -and $value -eq '<esp>') {
            if (-not $WindowsEspGuid) { throw 'ESPGUID_REQUIRED' }
            $arguments[$name] = $WindowsEspGuid
        } elseif ($value -is [bool]) {
            if ($value) { $arguments[$name] = $true }
        } else {
            $arguments[$name] = $value
        }
    }
    if (Test-CompiledStopSelection $Variant) {
        [void]$arguments.Remove('RuntimeSelection')
        if ($SelectionBdf) { $arguments['SelectionBdf'] = $SelectionBdf }
        else { $arguments['SelectionBdfs'] = $SelectionBdfs }
    }
    if ($Variant.PSObject.Properties.Name -contains 'requires_selection' -and $Variant.requires_selection) {
        if ($SelectionBdf -and $SelectionBdfs) { throw '-SelectionBdf and -SelectionBdfs are mutually exclusive.' }
        if ($SelectionBdf) { $arguments['SelectionBdf'] = $SelectionBdf }
        elseif ($SelectionBdfs) { $arguments['SelectionBdfs'] = $SelectionBdfs }
        else { throw 'SELECTION_REQUIRED' }
    }
    if ((Test-VariantSwitch $Variant 'FirmwareReattach') -and $WindowsEspGuid) {
        $arguments['WindowsEspGuid'] = $WindowsEspGuid
    }
    if ($CapturedPromReference) {
        # The two per-card firmware references this bench captured, compiled into
        # the image that asks for them. Only the active preparation builder links
        # src/prom_reference.c with a PROM read behind it; refusing here is
        # cheaper than signing an image whose receipt would claim something its
        # sources cannot do.
        if ($Variant.family -ne 'prepare') {
            throw ("-CapturedPromReference applies to the prepare family only, not {0}" -f $Variant.family)
        }
        $arguments['CapturedPromReference'] = $true
    }
    if ($VcVars) { $arguments['VcVars'] = $VcVars }
    return $arguments
}

if ($List -or $ListConfigurations) {
    foreach ($profileSpec in $catalogue.profiles) {
        Say ("PROFILE {0} {1}" -f $profileSpec.id, $profileSpec.usage)
        Say ("  {0}" -f $profileSpec.note)
    }
    if (-not $ListConfigurations) {
        Say 'Use -ListConfigurations for the build configuration matrix. Use -ResolveOnly with -Only to inspect a configuration without building.'
        exit 0
    }
    Say ('CATALOGUE {0} variants={1} families={2}' -f $cataloguePath, @($catalogue.variants).Count, @($catalogue.families).Count)
    foreach ($familySpec in $catalogue.families) {
        Say ("FAMILY {0} BUILDER={1} SUBSYSTEM={2}" -f $familySpec.id, $familySpec.builder, $familySpec.subsystem)
        foreach ($variant in @($catalogue.variants | Where-Object { $_.family -eq $familySpec.id })) {
            $needs = @()
            if ($variant.PSObject.Properties.Name -contains 'requires_selection' -and $variant.requires_selection) { $needs += "SELECTION=$($variant.requires_selection)" }
            if ($variant.PSObject.Properties.Name -contains 'requires_esp_guid' -and $variant.requires_esp_guid) { $needs += 'ESP_GUID' }
            Say ("VARIANT {0} PROFILE={1} SAFETY={2} BINARY={3} {4}" -f $variant.id, $variant.profile, $variant.safety, $variant.binary, ($needs -join ' '))
        }
    }
    foreach ($blocked in $catalogue.not_buildable) {
        Say ("NOT_BUILDABLE {0} WHY={1}" -f $blocked.id, $blocked.why)
    }
    exit 0
}

function Stage-SignedDxeCompanion {
    param([string]$Drive, [string]$Root, [string]$SignedFile, [string]$SignedSha256,
          [string]$LocalOutput, [string]$ExpectedVolumeId)
    if ((Assert-UsbVolume $Drive) -ne $Root) { throw 'Drive letter identity changed.' }
    if ([string](Get-Volume -DriveLetter $Drive -ErrorAction Stop).UniqueId -ne $ExpectedVolumeId) {
        throw 'Target USB volume identity changed before DXE staging.'
    }
    if ([IO.Path]::GetPathRoot($SignedFile).TrimEnd('\') -ieq $Root.TrimEnd('\') -or
        [IO.Path]::GetPathRoot($LocalOutput).TrimEnd('\') -ieq $Root.TrimEnd('\')) {
        throw 'Signed source and local backups must not reside on the USB destination drive.'
    }
    if ((Get-Sha256 $SignedFile) -ne $SignedSha256) { throw 'Signed DXE hash changed before staging.' }

    $efiRoot = Join-Path $Root 'EFI'
    $driversDir = Join-Path $efiRoot 'Drivers'
    foreach ($dir in @($efiRoot, $driversDir)) {
        if (-not (Test-Path -LiteralPath $dir)) { [void](New-Item -ItemType Directory -Path $dir -ErrorAction Stop) }
        Assert-NormalDirectory $dir
    }
    $destination = Join-Path $driversDir 'AdaUnlockDxe.efi'
    if ((Test-Path -LiteralPath $destination) -and -not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        throw "Refusing non-file DXE destination: $destination"
    }
    $oldPresent = Test-Path -LiteralPath $destination -PathType Leaf
    $oldSha = $null; $backup = $null
    if ($oldPresent) {
        $answer = Read-Host 'Type YES/Yes/yes/y/Y to replace only USB EFI\Drivers\AdaUnlockDxe.efi'
        if ($answer -cne 'YES' -and $answer -cne 'Yes' -and $answer -cne 'yes' -and $answer -cne 'y' -and $answer -cne 'Y') {
            throw 'DXE companion replacement cancelled; existing driver preserved.'
        }
        $backup = Join-Path $LocalOutput ('dxe-usb-backup-{0}-{1}' -f
            [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfff'), [guid]::NewGuid().ToString('N'))
        [void](New-Item -ItemType Directory -Path $backup -ErrorAction Stop)
        $saved = Join-Path $backup 'AdaUnlockDxe.efi'
        $oldSha = Get-Sha256 $destination
        Copy-Item -LiteralPath $destination -Destination $saved -ErrorAction Stop
        if ((Get-Sha256 $saved) -ne $oldSha -or (Get-Sha256 $destination) -ne $oldSha) {
            throw 'DXE backup verification failed; existing driver preserved.'
        }
        Say "Verified original USB DXE backup: $saved"
    }
    $temporary = Join-Path $driversDir ('.new-dxe-{0}.tmp' -f [guid]::NewGuid().ToString('N'))
    try {
        if ((Assert-UsbVolume $Drive) -ne $Root -or
            [string](Get-Volume -DriveLetter $Drive -ErrorAction Stop).UniqueId -ne $ExpectedVolumeId) {
            throw 'Target USB identity changed during DXE staging.'
        }
        Copy-Item -LiteralPath $SignedFile -Destination $temporary -ErrorAction Stop
        if ((Get-Sha256 $temporary) -ne $SignedSha256) { throw 'Staged DXE temporary hash mismatch.' }
        if ([bool](Test-Path -LiteralPath $destination -PathType Leaf) -ne [bool]$oldPresent) {
            throw 'DXE destination presence changed during staging.'
        }
        if ($oldPresent -and (Get-Sha256 $destination) -ne $oldSha) {
            throw 'DXE destination changed during staging; existing driver preserved.'
        }
        if ($oldPresent) { Remove-Item -LiteralPath $destination -Force -ErrorAction Stop }
        Move-Item -LiteralPath $temporary -Destination $destination -ErrorAction Stop
        if ((Get-Sha256 $destination) -ne $SignedSha256) { throw 'Deployed DXE hash mismatch.' }
    } catch {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
        if ($oldPresent -and $backup -and (Test-Path -LiteralPath (Join-Path $backup 'AdaUnlockDxe.efi'))) {
            Copy-Item -LiteralPath (Join-Path $backup 'AdaUnlockDxe.efi') -Destination $destination -Force -ErrorAction SilentlyContinue
        } elseif (-not $oldPresent -and (Test-Path -LiteralPath $destination -PathType Leaf)) {
            Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
        }
        throw
    }
    return [pscustomobject]@{ Path = $destination; BackupDirectory = $backup; SignedSha256 = $SignedSha256;
                             Replaced = [bool]$oldPresent; PreviousSha256 = $oldSha }
}

function Restore-DxeCompanionDeployment($Deployment, [string]$Drive, [string]$Root, [string]$ExpectedVolumeId) {
    if (-not $Deployment) { return }
    if ((Assert-UsbVolume $Drive) -ne $Root -or
        [string](Get-Volume -DriveLetter $Drive -ErrorAction Stop).UniqueId -ne $ExpectedVolumeId) {
        throw 'Cannot roll back DXE companion: target USB identity changed.'
    }
    if ($Deployment.Replaced) {
        $saved = Join-Path $Deployment.BackupDirectory 'AdaUnlockDxe.efi'
        if ((Get-Sha256 $saved) -ne $Deployment.PreviousSha256) { throw 'DXE rollback backup hash mismatch.' }
        Copy-Item -LiteralPath $saved -Destination $Deployment.Path -Force -ErrorAction Stop
        if ((Get-Sha256 $Deployment.Path) -ne $Deployment.PreviousSha256) { throw 'DXE rollback restore hash mismatch.' }
    } elseif (Test-Path -LiteralPath $Deployment.Path -PathType Leaf) {
        Remove-Item -LiteralPath $Deployment.Path -Force -ErrorAction Stop
    }
}

# --- preflight ------------------------------------------------------------

try {
    $stageVariantId = $null
    if ($All -and $Only) { throw '-All and -Only are mutually exclusive.' }
    if ($CapturedPromReference -and -not $Only) {
        throw ('-CapturedPromReference applies to the prepare family only, so it needs ' +
               '-Only naming the variant it is for (e.g. -Only diagnostic -Probe Preflight). A matrix run ' +
               'would apply it to image kinds whose sources have no PROM read behind it.')
    }
    if ($All -and $StageDrive) { throw '-All must not stage: a matrix run signs only. Name one image with -Only <profile> -StageDrive <letter>.' }
    if ($SignOnly -and $StageDrive) { throw '-SignOnly and -StageDrive are mutually exclusive: -SignOnly never touches a stick. Drop -StageDrive, or drop -SignOnly to stage the selected image.' }
    if ($SelectionBdf -and $SelectionBdfs) { throw '-SelectionBdf and -SelectionBdfs are mutually exclusive.' }
    # Use the builder's parser before resolving a profile. Inspection never
    # creates a build directory or writes a selection header.
    $selectionHelper = Join-Path (Join-Path $RepoRoot 'src') 'Gpu-Selection-Build.ps1'
    if (-not (Test-Path -LiteralPath $selectionHelper -PathType Leaf)) {
        throw "GPU selection helper missing: $selectionHelper"
    }
    . $selectionHelper
    . (Join-Path (Join-Path $RepoRoot 'src') 'Windows-Esp-Build.ps1')
    $script:SelectionSpec = New-GuGpuSelectionSpec -SelectionBdf $SelectionBdf -SelectionBdfs $SelectionBdfs
    $localProfilePath = Join-Path (Join-Path $RepoRoot 'src') 'local\boot_profile.h'
    $script:HasLocalBootProfile = Test-Path -LiteralPath $localProfilePath -PathType Leaf
    $variants = @(Select-Variants)
    if ($ResolveOnly) {
        if (-not $Only -or $variants.Count -ne 1 -or $StageDrive -or $StageDxeCompanion) {
            throw '-ResolveOnly requires one explicit selection and does not permit deployment.'
        }
        $resolvedVariant = $variants[0]
        $resolutionPrerequisite = Test-VariantSelection $resolvedVariant
        if ($resolutionPrerequisite -and $resolutionPrerequisite -ne 'NEEDS_LOCAL_BOOT_PROFILE') {
            throw "Cannot resolve $($resolvedVariant.id): $resolutionPrerequisite"
        }
        $resolvedArguments = Build-VariantArguments $resolvedVariant
        if ($resolvedArguments.ContainsKey('WindowsEspGuid')) {
            [void](ConvertTo-GuWindowsEspGuid ([string]$resolvedArguments['WindowsEspGuid']))
        }
        Say ([ordered]@{
            CanonicalProfile = $resolvedVariant.profile
            RequestedCanonicalProfile = $script:RequestedCanonicalProfile
            VariantId = $resolvedVariant.id
            Safety = $resolvedVariant.safety
            Subsystem = $familyById[$resolvedVariant.family].subsystem
            Builder = $familyById[$resolvedVariant.family].builder
            Switches = $resolvedArguments
            Binary = (Get-VariantBinary $resolvedVariant)
            Selection = @(if ($resolvedArguments.ContainsKey('SelectionBdf') -or $resolvedArguments.ContainsKey('SelectionBdfs')) { $script:SelectionSpec.Locations })
            BuildPrerequisite = $resolutionPrerequisite
            ExpectedAttestations = @($resolvedVariant.attestations)
            ResolvedAttestations = @(Get-ResolvedAttestations $resolvedVariant)
        } | ConvertTo-Json -Depth 6 -Compress)
        exit 0
    }
    if ($StageDxeCompanion) {
        if (-not $StageDrive) { throw '-StageDxeCompanion requires -StageDrive <letter>.' }
        if ($variants.Count -ne 1 -or (Get-SelectionCount) -ne 1 -or
            -not (Test-PairedTransactionVariant $variants[0] 'prepare')) {
            throw '-StageDxeCompanion requires firmware-transaction -Operation Menu, one selected BDF and the pinned application configuration.'
        }
        if (-not $AcceptActuatingDeploy) {
            throw '-StageDxeCompanion requires -AcceptActuatingDeploy; it stages the waived app and its matching DXE driver.'
        }
    }
    if ($StageDrive) {
        if (-not $Only) { throw 'Staging requires an explicit -Only <profile> request.' }
        # The selection is what names the image; a stick holds one BOOTX64.EFI, so
        # staging more than one variant is not a thing.
        if ($variants.Count -ne 1) {
            $notOne = ('-StageDrive stages exactly one image, but {0} variants are selected. ' +
                       'Name it with -Only <profile> (see -List).')
            throw ($notOne -f $variants.Count)
        }
        $deployVariant = $variants[0]
        $stageVariantId = $deployVariant.id
        $safety = $catalogue.safety_classes.($deployVariant.safety)
        if ($deployVariant.binary -match 'DXE' -or $familyById[$deployVariant.family].subsystem -eq 11) {
            throw 'A DXE boot-service driver cannot be staged as BOOTX64.EFI. Sign it and install it as a Driver#### entry.'
        }
        if ($safety.may_be_staged -eq $false) {
            $message = ('Refusing to stage {0}: safety={1} writes_gpu={2}, so staging it would only ' +
                        'cost a boot. Sign it, or build a variant that observes something.')
            throw ($message -f $deployVariant.id, $deployVariant.safety, $safety.writes_gpu)
        }
        if ($safety.may_be_staged -ne $true -and -not $AcceptActuatingDeploy) {
            throw ("Refusing to stage {0}: safety={1} writes_gpu={2}. Add -AcceptActuatingDeploy if the donor card, the recovery toolkit and the session plan are ready." -f
                   $deployVariant.id, $deployVariant.safety, $safety.writes_gpu)
        }
        if ($deployVariant.safety -eq 'maintenance') {
            if ($script:RequestedCanonicalProfile -ne 'maintenance' -or $Operation -ne 'ReviewBootState') {
                throw 'Maintenance staging requires -Only maintenance -Operation ReviewBootState.'
            }
            if ($SelectionBdf -or $SelectionBdfs -or $WindowsEspGuid -or $CapturedPromReference) {
                throw 'Maintenance does not accept GPU selection, Windows ESP or firmware reference options.'
            }
            Say 'WARNING: maintenance can remove GuBootState only after explicit REARM confirmation. Use isolated media and disconnect other experimental boot media before booting.'
        }
        if ($safety.writes_gpu) {
            Say ("WARNING: staging ACTUATING_IMAGE={0} SAFETY={1}; it may write the GPU. Recovery toolkit on the bench is not optional." -f
                 $deployVariant.id, $deployVariant.safety)
        }
    }

    $dxeStageVariantId = $null
    if ($StageDxeCompanion) {
        $companion = @($catalogue.variants | Where-Object { Test-PairedTransactionVariant $_ 'prepare-dxe' })
        if ($companion.Count -ne 1) { throw 'Catalogue must contain exactly one pinned single-target DXE transaction companion.' }
        $companionSafety = $catalogue.safety_classes.($companion[0].safety)
        if ($companion[0].safety -ne 'actuates' -or $familyById[$companion[0].family].subsystem -ne 11 -or
            -not $companionSafety.writes_gpu) {
            throw 'The catalogue companion must remain an actuating subsystem-11 DXE image.'
        }
        $dxeStageVariantId = [string]$companion[0].id
        # Build/sign the resident driver first; the boot application is staged
        # only after its companion has been prepared successfully.
        $variants = @($companion[0]) + @($variants)
    }

    $RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
    if (-not (Test-Path -LiteralPath $RepoRoot -PathType Container)) { throw "Repository missing: $RepoRoot" }
    Assert-NormalDirectory $RepoRoot

    if (-not $OutputDirectory) { $OutputDirectory = Join-Path $RepoRoot 'artifacts\signed' }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    [void](New-Item -ItemType Directory -Path $OutputDirectory -Force -ErrorAction Stop)
    Assert-NormalDirectory $OutputDirectory

    if (-not $PublicCertificate) { $PublicCertificate = Join-Path $PSScriptRoot 'uefi-db.cer' }
    if (-not (Test-Path -LiteralPath $PublicCertificate -PathType Leaf)) {
        throw "Public signing certificate not found: $PublicCertificate (export the DER .cer you enrolled in the BIOS db)."
    }
    $PublicCertificate = (Resolve-Path -LiteralPath $PublicCertificate).ProviderPath
    $certificateSha = Get-Sha256 $PublicCertificate
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        throw 'WSL not found. Windows PowerShell with WSL, sbsign, sbverify and openssl is required.'
    }

    $drive = $null; $usbRoot = $null; $targetVolumeId = $null
    if ($stageVariantId) {
        $drive = $StageDrive.ToUpperInvariant()
        $usbRoot = Assert-UsbVolume $drive
        $targetVolumeId = [string](Get-Volume -DriveLetter $drive -ErrorAction Stop).UniqueId
        if (-not $targetVolumeId) { throw 'USB volume identity unavailable; refusing staging.' }
        if ([IO.Path]::GetPathRoot($OutputDirectory).TrimEnd('\') -ieq $usbRoot.TrimEnd('\')) {
            throw 'Local signed outputs/backups must be on a different drive from target USB.'
        }
    }

    # The BIOS db check: signing with a certificate the firmware does not trust
    # produces an image that will not load, and hides the real problem.
    $cerBytes = [IO.File]::ReadAllBytes($PublicCertificate)
    $dbEnrolled = if ($SkipDbCheck) { 'UNKNOWN' } else { Test-CertificateEnrolled $cerBytes }
    if (-not $SkipDbCheck -and $dbEnrolled -eq 'FALSE') {
        throw ('The certificate {0} is not present in the BIOS db. Enroll it first, or re-run with -SkipDbCheck to sign anyway and record db_enrolled=UNKNOWN.' -f $PublicCertificate)
    }
    Say ("SIGNING_CERTIFICATE path={0} SHA256={1} DB_ENROLLED={2}" -f $PublicCertificate, $certificateSha, $dbEnrolled)
    if ($CapturedPromReference) {
        Say 'PROM_REFERENCE=CAPTURED ROWS=2 SOURCE=src/prom_reference.c AUTHENTICATION=0'
    }
    if ($dbEnrolled -eq 'UNKNOWN' -and -not $SkipDbCheck) {
        Say ('WARNING: the BIOS db could not be read (the Secure Boot cmdlets need an elevated ' +
             'console, and there is nothing to read when Secure Boot is off). Signing continues, ' +
             'and every receipt records DB_ENROLLED=UNKNOWN: re-run elevated to prove enrollment.')
    }

    # Prove the WSL target can sign before spending a whole matrix of builds on it.
    [void](Test-WslSigningReady -PublicCer $PublicCertificate)

    if ($script:SelectionSpec.Count) {
        Say ("SELECTION label={0} count={1} bdfs={2}" -f $script:SelectionSpec.Label,
             $script:SelectionSpec.Count, ($script:SelectionSpec.Locations -join ','))
    }

    $runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfff')
    $results = New-Object 'System.Collections.Generic.List[object]'
    $signedCount = 0
    $failedCount = 0
    $skippedCount = 0
    $variantTotal = $variants.Count
    $variantIndex = 0
    $stagedPath = $null
    $pendingBootImage = $null
    $pendingDxeImage = $null
    $localProfileSkips = 0

    foreach ($variant in $variants) {
        $variantIndex++
        $familySpec = $familyById[$variant.family]
        $variantId = $variant.id
        # A count, so a long -All run reads as progress instead of a loop.
        Say ("VARIANT_BEGIN [{0}/{1}] id={2} family={3} safety={4} builder={5}" -f
             $variantIndex, $variantTotal, $variantId, $variant.family, $variant.safety, $familySpec.builder)
        try {
            try {
                $skipReason = Test-VariantSelection $variant
                if ($skipReason) {
                    Say ("VARIANT_SKIPPED id={0} REASON={1}" -f $variantId, $skipReason)
                    if ($skipReason -eq 'NEEDS_LOCAL_BOOT_PROFILE') { $localProfileSkips++ }
                    if ($Only) {
                        $named = ('Cannot build {0}: {1}. -Only asked for this image specifically, so ' +
                                  'this is an error rather than a skip; -All records it and carries on.')
                        $message = $named -f $variantId, $skipReason
                        # The one skip that is a setup step rather than a wrong argument
                        # says what to create. A waived image compiles against the reviewed
                        # local profile, and -WindowsEspGuid is not a substitute: that switch
                        # belongs to the diagnostic handoff builders, and the prepare family
                        # includes src/local/boot_profile.h directly.
                        if ($skipReason -eq 'NEEDS_LOCAL_BOOT_PROFILE') {
                            $message += (' REMEDY=create src\local\boot_profile.h from src\boot_profile.example.h ' +
                                         'holding the reviewed ESP unique partition GUID; the directory is ' +
                                         'gitignored, the file is per machine, and -WindowsEspGuid does not ' +
                                         'stand in for it in the prepare family. ' +
                                         'VERIFY=python3 tools\session_preflight.py --profile src\local\boot_profile.h')
                        }
                        throw $message
                    }
                    $skippedCount++
                    $results.Add([pscustomobject]@{
                        id = $variantId; family = $variant.family; safety = $variant.safety
                        status = 'SKIPPED'; reason = $skipReason
                    })
                    continue
                }
                $buildArguments = Build-VariantArguments $variant
            } catch {
                $reason = $_.Exception.Message
                if ($reason -eq 'ESPGUID_REQUIRED' -or $reason -eq 'SELECTION_REQUIRED') {
                    Say ("VARIANT_SKIPPED id={0} REASON={1}" -f $variantId, $reason)
                    $skippedCount++
                    $results.Add([pscustomobject]@{
                        id = $variantId; family = $variant.family; safety = $variant.safety
                        status = 'SKIPPED'; reason = $reason
                    })
                    continue
                }
                throw
            }

            $buildScript = Join-Path $RepoRoot $familySpec.builder
            if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
                throw "Builder missing for $($variant.family): $buildScript"
            }
            $binaryName = Get-VariantBinary $variant
            # The builders write into their own output directories, so take the
            # path from the builder's own report instead of guessing it.
            # Stream the builder instead of buffering it: a builder that throws
            # after the compiler rejected a file used to discard every line it
            # had printed, so the run reported "Compilation failed" and nothing
            # else. Now the compiler's own message reaches the log, and the
            # failure names the last line the builder produced.
            $buildOutput = New-Object 'System.Collections.Generic.List[string]'
            $buildError = $null
            try {
                & $buildScript @buildArguments 2>&1 | ForEach-Object {
                    $buildText = [string]$_
                    Say $buildText
                    $buildOutput.Add($buildText)
                }
            } catch {
                $buildError = $_
            }
            if ($null -ne $buildError) {
                $printed = @($buildOutput.ToArray() | Where-Object { $_.Trim() })
                $lastLine = if ($printed.Count) { $printed[$printed.Count - 1] } else { 'the builder printed nothing' }
                $failed = ('Builder failed for {0}: {1}. Last builder line: {2}')
                throw ($failed -f $variantId, $buildError.Exception.Message, $lastLine)
            }
            $statusPattern = '^' + [regex]::Escape($familySpec.status_token) + ' PATH='
            $statusLines = @($buildOutput.ToArray() | Where-Object { [string]$_ -match $statusPattern })
            if ($statusLines.Count -ne 1) {
                throw "Builder $variantId did not emit exactly one $($familySpec.status_token) PATH= report."
            }
            $statusLine = [string]$statusLines[0]
            if ($statusLine -notmatch ('^' + [regex]::Escape($familySpec.status_token) +
                                       ' PATH=(.+?) SHA256=([0-9a-fA-F]{64})(?:\s|$)')) {
                throw "Builder report for $variantId is malformed."
            }
            $builtImage = [IO.Path]::GetFullPath($Matches[1])
            $sourceHash = $Matches[2].ToLowerInvariant()
            if ((Split-Path -Leaf $builtImage) -ine $binaryName) {
                $mismatch = ("Builder produced '{0}' but the catalogue expects '{1}'." -f $builtImage, $binaryName)
                throw ($mismatch + ' Fix docs/image_variants.json; this script never renames an image.')
            }
            $verifiedAttestations = @()
            foreach ($attestation in @(Get-ResolvedAttestations $variant)) {
                if ($statusLine -notmatch ('(?:^|\s)' + [regex]::Escape($attestation) + '(?:\s|$)')) {
                    if ((Test-VariantSwitch $variant 'FirmwareReattach') -and $attestation -like 'WINDOWS_ESP_GUID=*') {
                        throw 'Combined image Windows ESP GUID attestation mismatch.'
                    }
                    throw "Build attestation missing or wrong for $($variantId): $attestation"
                }
                $verifiedAttestations += $attestation
            }
            Assert-EfiImage $builtImage ([int]$familySpec.subsystem)
            if ((Get-Sha256 $builtImage) -ne $sourceHash) {
                throw "Reported SHA-256 does not match the built image for $variantId."
            }

            $signedEfi = Join-Path $OutputDirectory ('UniversalGPU-{0}-{1}-{2}-signed.efi' -f
                            $variantId, $runId, $sourceHash.Substring(0, 12))
            if (Test-Path -LiteralPath $signedEfi) { throw "Signed output already exists: $signedEfi" }
            Say "Unsigned: $builtImage"
            Say "Unsigned SHA-256: $sourceHash"
            $signExit = Invoke-WslSign -SourceEfi $builtImage -SignedEfi $signedEfi `
                            -ExpectedSha $sourceHash -PublicCer $PublicCertificate
            if ($signExit -isnot [int]) {
                $leak = ('Internal: the signing call returned {0} separate values instead of ' +
                         'one exit code. Nothing was staged; report this with the output above.')
                throw ($leak -f @($signExit).Count)
            }
            if ($signExit -ne 0) {
                if (Test-Path -LiteralPath $signedEfi) {
                    Remove-Item -LiteralPath $signedEfi -Force -ErrorAction SilentlyContinue
                }
                throw "WSL signing/verification failed for $($variantId): exit $signExit"
            }
            if (-not (Test-Path -LiteralPath $signedEfi -PathType Leaf)) {
                throw "Signing reported success without an output for $variantId."
            }
            if ((Get-Sha256 $builtImage) -ne $sourceHash) {
                throw "Unsigned source changed after signing ($variantId)."
            }
            Assert-EfiImage $signedEfi ([int]$familySpec.subsystem)
            $signedHash = Get-Sha256 $signedEfi
            if ($signedHash -eq $sourceHash) { throw "Signed image is byte-identical to its input ($variantId)." }
            Say "Verified signed SHA-256: $signedHash"

            $receiptPath = "$signedEfi.json"
            $deployment = $null
            if ($StageDxeCompanion -and $stageVariantId -eq $variantId) {
                $pendingBootImage = [pscustomobject]@{ Path = $signedEfi; Sha256 = $signedHash; Receipt = $receiptPath }
            } elseif ($StageDxeCompanion -and $dxeStageVariantId -eq $variantId) {
                $pendingDxeImage = [pscustomobject]@{ Path = $signedEfi; Sha256 = $signedHash; Receipt = $receiptPath }
            } elseif ($stageVariantId -and $stageVariantId -eq $variantId) {
                $deployment = Stage-SignedEfi -Drive $drive -Root $usbRoot -SignedFile $signedEfi `
                                  -SignedSha256 $signedHash -LocalOutput $OutputDirectory `
                                  -ExpectedVolumeId $targetVolumeId -MaintenanceOnly:($variant.safety -eq 'maintenance')
                $stagedPath = $deployment.Path
            }

            [ordered]@{
                VariantId = $variantId
                CanonicalProfile = $variant.profile
                RequestedCanonicalProfile = $script:RequestedCanonicalProfile
                Family = $variant.family
                Safety = $variant.safety
                WritesGpu = [bool]$catalogue.safety_classes.($variant.safety).writes_gpu
                RequiresSelection = $(if ($variant.PSObject.Properties.Name -contains 'requires_selection') { $variant.requires_selection } else { $null })
                Selection = $(if ($buildArguments.ContainsKey('SelectionBdfs')) { $buildArguments['SelectionBdfs'] } elseif ($buildArguments.ContainsKey('SelectionBdf')) { $buildArguments['SelectionBdf'] } else { $null })
                Switches = $buildArguments
                ExpectedAttestations = @($variant.attestations)
                AttestationsVerified = $verifiedAttestations
                Subsystem = [int]$familySpec.subsystem
                Repository = $RepoRoot
                SourceEfi = $builtImage
                SourceSha256 = $sourceHash
                SignedEfi = $signedEfi
                SignedSha256 = $signedHash
                PublicCertificate = $PublicCertificate
                CertificateSha256 = $certificateSha
                DbEnrolled = $dbEnrolled
                VerifiedBySbverify = $true
                Destination = $(if ($deployment) { $deployment.Path } else { $null })
                DxeCompanionStaged = [bool]($dxeStageVariantId -and $variantId -eq $dxeStageVariantId -and $deployment)
                DxeCompanionPath = $null
                DxeCompanionSha256 = $null
                ReplacedBootEfi = $(if ($deployment -and $variantId -eq $stageVariantId) { $deployment.ReplacedBootEfi } else { $false })
                PreservedOtherEfiCount = $(if ($deployment) { $deployment.PreservedOtherEfiCount } else { 0 })
                BackupDirectory = $(if ($deployment) { $deployment.BackupDirectory } else { $null })
                HardwareVerified = $false
                Windows61742BooterProven = $false
                DateUtc = [DateTime]::UtcNow.ToString('o')
            } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8 -ErrorAction Stop

            $signedCount++
            $results.Add([pscustomobject]@{
                id = $variantId; family = $variant.family; safety = $variant.safety
                status = 'SIGNED'; binary = $binaryName
                source = $builtImage; signed = $signedEfi
                source_sha256 = $sourceHash; signed_sha256 = $signedHash
                receipt = $receiptPath
                destination = $(if ($deployment) { $deployment.Path } else { $null })
            })
        } catch {
            $failedCount++
            Say ("VARIANT_FAILED id={0} DETAIL={1}" -f $variantId, $_.Exception.Message)
            $results.Add([pscustomobject]@{
                id = $variantId; family = $variant.family; safety = $variant.safety
                status = 'FAILED'; reason = $_.Exception.Message
            })
            if (($stageVariantId -and $stageVariantId -eq $variantId) -or ($dxeStageVariantId -and $dxeStageVariantId -eq $variantId)) { throw }
        }
    }

    if ($StageDxeCompanion) {
        if (-not $pendingDxeImage -or -not $pendingBootImage -or $failedCount) {
            throw 'Paired deployment not staged: both signed companion artifacts must build and verify first.'
        }
        $dxeDeployment = Stage-SignedDxeCompanion -Drive $drive -Root $usbRoot `
            -SignedFile $pendingDxeImage.Path -SignedSha256 $pendingDxeImage.Sha256 `
            -LocalOutput $OutputDirectory -ExpectedVolumeId $targetVolumeId
        try {
            $bootDeployment = Stage-SignedEfi -Drive $drive -Root $usbRoot `
                -SignedFile $pendingBootImage.Path -SignedSha256 $pendingBootImage.Sha256 `
                -LocalOutput $OutputDirectory -ExpectedVolumeId $targetVolumeId
        } catch {
            $stageError = $_.Exception.Message
            try { Restore-DxeCompanionDeployment $dxeDeployment $drive $usbRoot $targetVolumeId }
            catch { throw "Application staging failed: $stageError. DXE companion rollback failed: $($_.Exception.Message)" }
            throw "Application staging failed; DXE companion was rolled back: $stageError"
        }
        $stagedPath = $bootDeployment.Path
        Say ("STAGED_DXE_COMPANION PATH={0} DRIVE={1}: SHA256={2}" -f
             $dxeDeployment.Path, $drive.ToUpperInvariant(), $pendingDxeImage.Sha256)
        $dxeReceipt = Get-Content -LiteralPath $pendingDxeImage.Receipt -Raw | ConvertFrom-Json
        $dxeReceipt.Destination = $dxeDeployment.Path
        $dxeReceipt.DxeCompanionStaged = $true
        $dxeReceipt.BackupDirectory = $dxeDeployment.BackupDirectory
        $dxeReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $pendingDxeImage.Receipt -Encoding UTF8 -ErrorAction Stop
        $appReceipt = Get-Content -LiteralPath $pendingBootImage.Receipt -Raw | ConvertFrom-Json
        $appReceipt.Destination = $bootDeployment.Path
        $appReceipt.DxeCompanionStaged = $true
        $appReceipt.DxeCompanionPath = $dxeDeployment.Path
        $appReceipt.DxeCompanionSha256 = $pendingDxeImage.Sha256
        $appReceipt.ReplacedBootEfi = $bootDeployment.ReplacedBootEfi
        $appReceipt.BackupDirectory = $bootDeployment.BackupDirectory
        $appReceipt.PreservedOtherEfiCount = $bootDeployment.PreservedOtherEfiCount
        $appReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $pendingBootImage.Receipt -Encoding UTF8 -ErrorAction Stop
        foreach ($result in $results) {
            if ($result.id -eq $dxeStageVariantId) { $result.destination = $dxeDeployment.Path }
            if ($result.id -eq $stageVariantId) { $result.destination = $bootDeployment.Path }
        }
    }

    $matrixPath = Join-Path $OutputDirectory ("matrix-{0}.json" -f $runId)
    [ordered]@{
        RunId = $runId
        Catalogue = $cataloguePath
        Repository = $RepoRoot
        PublicCertificate = $PublicCertificate
        CertificateSha256 = $certificateSha
        DbEnrolled = $dbEnrolled
        Selection = $(if ($SelectionBdfs) { $SelectionBdfs } elseif ($SelectionBdf) { $SelectionBdf } else { $null })
        WindowsEspGuid = $(if ($WindowsEspGuid) { $WindowsEspGuid } else { $null })
        Signed = $signedCount
        Failed = $failedCount
        Skipped = $skippedCount
        HardwareVerified = $false
        Variants = $results.ToArray()
        DateUtc = [DateTime]::UtcNow.ToString('o')
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $matrixPath -Encoding UTF8 -ErrorAction Stop

    if ($skippedCount) {
        $hint = ('SKIPPED_HINT {0} variants needed different arguments: -SelectionBdf builds the ' +
                 'single-target images, -SelectionBdfs accepts one or more and two or more builds the sequence ones, ' +
                 '-WindowsEspGuid builds the handoff ones. Run -All again with the other selection.')
        Say ($hint -f $skippedCount)
    }
    if ($localProfileSkips) {
        $setup = ('SETUP_HINT {0} variants need a reviewed ESP GUID compiled in, and src\local\boot_profile.h ' +
                  'does not exist. Copy src\boot_profile.example.h to {1} and put the GUID of this machine''s ' +
                  'EFI system partition in it, or build the -RuntimeEsp variant instead. Nothing was staged.')
        Say ($setup -f $localProfileSkips, $localProfilePath)
    }
    Say ("STATUS={0} SIGNED={1} FAILED={2} SKIPPED={3} DB_ENROLLED={4} HARDWARE_VERIFIED=0 MATRIX={5}" -f
        $(if ($failedCount) { 'INCOMPLETE' } else { 'COMPLETE' }),
        $signedCount, $failedCount, $skippedCount, $dbEnrolled, $matrixPath)
    if ($stageVariantId) {
        # Reaching this line means the copy and its hash check succeeded; a
        # staging failure throws out of the loop.
        Say (("STAGED {0} PATH={1} DRIVE={2}:") -f $stageVariantId, $stagedPath, $drive)
    }
    Say 'Signing proves authenticity only: the certificate is the one the firmware trusts, and the image is the one that was built.'
    Say 'It does not establish firmware or GPU compatibility, a Windows device start, or the 617.42 Booter provenance.'
    if ($failedCount) { exit 1 }
    exit 0
} catch {
    Say ("STATUS=ERROR DETAIL={0}" -f $_.Exception.Message)
    exit 1
}
