param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OpenSslRoot,
    [Parameter(Mandatory = $true)][string]$OpenSslDllDir,
    [Parameter(Mandatory = $true)][string]$DistributionDir
)

$ErrorActionPreference = "Stop"

function Resolve-Directory([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path.TrimEnd('\')
}

function Get-PeMachine([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $Path" }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Invalid PE signature: $Path" }
        return $reader.ReadUInt16()
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

$root = Resolve-Directory $OpenSslRoot "OPENSSL_ROOT_DIR"
$dllDir = Resolve-Directory $OpenSslDllDir "OpenSSL DLL directory"
$dist = Resolve-Directory $DistributionDir "Distribution directory"
$exe = (Resolve-Path -LiteralPath $Executable).Path
if (-not ($dllDir.Equals($root, [StringComparison]::OrdinalIgnoreCase)
        -or $dllDir.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase))) {
    throw "OpenSSL DLL directory must belong to OPENSSL_ROOT_DIR. root=$root dllDir=$dllDir"
}

$sourceSsl = @(Get-ChildItem -LiteralPath $dllDir -File |
    Where-Object { $_.Name -match '^libssl.*\.dll$' })
$sourceCrypto = @(Get-ChildItem -LiteralPath $dllDir -File |
    Where-Object { $_.Name -match '^libcrypto.*\.dll$' })
if ($sourceSsl.Count -ne 1 -or $sourceCrypto.Count -ne 1) {
    throw "Expected exactly one libssl DLL and one libcrypto DLL in $dllDir"
}
$sslSuffix = $sourceSsl[0].Name -replace '^libssl', ''
$cryptoSuffix = $sourceCrypto[0].Name -replace '^libcrypto', ''
if (-not $sslSuffix.Equals($cryptoSuffix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OpenSSL runtime pair has mismatched names: $($sourceSsl[0].Name), $($sourceCrypto[0].Name)"
}

$stagedSsl = @(Get-ChildItem -LiteralPath $dist -File |
    Where-Object { $_.Name -match '^libssl.*\.dll$' })
$stagedCrypto = @(Get-ChildItem -LiteralPath $dist -File |
    Where-Object { $_.Name -match '^libcrypto.*\.dll$' })
if ($stagedSsl.Count -ne 1 -or $stagedCrypto.Count -ne 1) {
    throw "Distribution must contain exactly one OpenSSL runtime pair"
}
foreach ($pair in @(
    @($sourceSsl[0].FullName, $stagedSsl[0].FullName),
    @($sourceCrypto[0].FullName, $stagedCrypto[0].FullName))) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $pair[0]).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $pair[1]).Hash) {
        throw "Staged TLS DLL does not match selected OpenSSL root: $($pair[1])"
    }
    if ((Get-PeMachine $pair[1]) -ne 0x8664) {
        throw "TLS DLL is not x64: $($pair[1])"
    }
}

$legacy = @(Get-ChildItem -LiteralPath $dist -File |
    Where-Object { $_.Name -match '^(ssleay32|libeay32)\.dll$' })
if ($legacy.Count -gt 0) {
    throw "Legacy OpenSSL DLLs must not be mixed into the distribution: $($legacy.Name -join ', ')"
}
if (-not (Test-Path -LiteralPath (Join-Path $dist 'Qt6Network.dll'))
        -and -not (Test-Path -LiteralPath (Join-Path $dist 'Qt5Network.dll'))) {
    throw "Qt Network runtime is missing from the distribution"
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction Stop
$imports = & $dumpbin.Source /dependents $exe
$tlsImports = @([Regex]::Matches(($imports -join "`n"),
    '(?im)^\s*(lib(?:ssl|crypto)[^\s]*\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value })
if ($tlsImports.Count -gt 0) {
    foreach ($dll in @($sourceSsl[0].Name, $sourceCrypto[0].Name)) {
        if ($imports -notmatch [Regex]::Escape($dll)) {
            throw "Executable import table does not reference selected runtime DLL: $dll"
        }
    }
} else {
    Write-Warning "Executable has no dynamic OpenSSL imports; the live Qt TLS probe is authoritative."
}

Write-Host "TLS runtime pair verified: $($sourceSsl[0].Name), $($sourceCrypto[0].Name)"
