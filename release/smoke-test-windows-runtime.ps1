param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [int]$StartupTimeoutSeconds = 4
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable not found: $Executable"
}

Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;

namespace AegisyRuntimeSmokeTest
{
    public static class NativeMethods
    {
        [DllImport("kernel32.dll")]
        public static extern uint SetErrorMode(uint mode);
    }
}
"@

$executableItem = Get-Item -LiteralPath $Executable
$process = $null
$previousErrorMode = [AegisyRuntimeSmokeTest.NativeMethods]::SetErrorMode(0x0001 -bor 0x0002)
try {
    $process = Start-Process `
        -FilePath $executableItem.FullName `
        -WorkingDirectory $executableItem.DirectoryName `
        -WindowStyle Hidden `
        -PassThru

    if ($process.WaitForExit($StartupTimeoutSeconds * 1000)) {
        throw "Application exited during runtime smoke test with code $($process.ExitCode). Check the staged DLL dependencies."
    }

    Write-Host "Runtime smoke test passed: $($executableItem.FullName)"
}
finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit()
    }
    [void][AegisyRuntimeSmokeTest.NativeMethods]::SetErrorMode($previousErrorMode)
}
