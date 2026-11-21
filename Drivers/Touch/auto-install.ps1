$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'auto-install.log'
try {
    Start-Transcript -Path $log -Append
    & "$env:SystemRoot\System32\pnputil.exe" /add-driver (Join-Path $PSScriptRoot 'umi-touch.inf') /install
    if ($LASTEXITCODE -notin @(0, 3010)) {
        throw "pnputil exited with $LASTEXITCODE"
    }
    & "$env:SystemRoot\System32\pnputil.exe" /enum-devices /instanceid 'ACPI\UMI0003\0' /drivers
} finally {
    Stop-Transcript
}
