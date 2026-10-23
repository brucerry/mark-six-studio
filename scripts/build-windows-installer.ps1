param([Parameter(Mandatory)][string]$PackageDirectory, [string]$OutputDirectory, [switch]$Signed)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($Signed -and -not $env:WINDOWS_SIGN_CERT_THUMBPRINT) { throw 'Signing credentials are required for a signed release.' }
& (Join-Path $PSScriptRoot 'verify-statistics-package.ps1') -PackageDirectory $PackageDirectory
& (Join-Path $PSScriptRoot 'fetch-windows-installer-tools.ps1')
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'release/windows-installer' }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Installer output directory already exists; choose a new directory.' }
$version = (Get-Item -LiteralPath (Join-Path $PackageDirectory 'mark-six-studio.exe')).VersionInfo.ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Unexpected application version metadata.' }
$options = @("/DAppVersion=$version","/DPackageDir=$([IO.Path]::GetFullPath($PackageDirectory))", "/DOutputDir=$([IO.Path]::GetFullPath($OutputDirectory))",
    "/DVCRedist=$(Join-Path $repo 'deps/installer-downloads/VC_redist.x64.exe')")
if ($Signed) {
    if ((Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageDirectory 'mark-six-studio.exe')).Status -ne 'Valid') {
        throw 'Package the application with -Signed before creating a signed installer.'
    }
    $options += '/DSignedBuild=1'
    $signScript = Join-Path $PSScriptRoot 'sign-windows-file.ps1'
    $options += ('/Smarksix=powershell.exe -NoProfile -ExecutionPolicy Bypass -File $q' + $signScript + '$q -Path $f')
}
& (Join-Path $repo 'deps/InnoSetup-7.1.0/ISCC.exe') @options (Join-Path $repo 'installer/windows.iss')
if ($LASTEXITCODE) { throw 'Windows installer compilation failed.' }
$installers = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter '*-setup.exe')
if ($installers.Count -ne 1) { throw 'Expected exactly one Windows installer.' }
$installer = $installers[0]
if ($Signed -and (Get-AuthenticodeSignature -LiteralPath $installer.FullName).Status -ne 'Valid') { throw 'Installer signature is invalid.' }
$hash = (Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath ($installer.FullName + '.sha256') -Value "$hash  $($installer.Name)" -Encoding ascii
Write-Output "Installer: $($installer.FullName)"
