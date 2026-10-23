param([Parameter(Mandatory)][string]$Installer)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Installation/removal fixture runs only on disposable GitHub Actions Windows runners.' }
$root = Join-Path $env:RUNNER_TEMP ('marksix-installer-' + [guid]::NewGuid().ToString('N'))
$install = Join-Path $root 'application'
$data = Join-Path $root 'retained-user-data'
New-Item -ItemType Directory -Path $data -Force | Out-Null
$sentinel = Join-Path $data 'preserve.txt'
Set-Content -LiteralPath $sentinel -Value 'Installer must retain this user data.' -Encoding utf8
function Install-App {
    $process = Start-Process -FilePath $Installer -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',"/DIR=`"$install`"") -Wait -PassThru
    if ($process.ExitCode -notin @(0,3010)) { throw "Installer failed: $($process.ExitCode)" }
}
function Smoke-App {
    $process = Start-Process -FilePath (Join-Path $install 'mark-six-studio.exe') -ArgumentList @('--smoke-load','--data-dir',"`"$data`"") -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Installed application failed: $($process.ExitCode)" }
}
function Data-Hashes {
    $out = @{}
    foreach ($file in Get-ChildItem -LiteralPath $data -Recurse -File) {
        $out[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $out
}
function Check-Data($Before) {
    $after = Data-Hashes
    if ($Before.Count -ne $after.Count) { throw 'Installer changed user data inventory.' }
    foreach ($key in $Before.Keys) { if ($Before[$key] -cne $after[$key]) { throw "Installer changed user data: $key" } }
}
Install-App
Smoke-App
$before = Data-Hashes
Install-App
Check-Data $before
Smoke-App
$before = Data-Hashes
$uninstaller = Join-Path $install 'unins000.exe'
$process = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -Wait -PassThru
if ($process.ExitCode -ne 0) { throw 'Uninstaller failed.' }
Check-Data $before
if (Test-Path -LiteralPath (Join-Path $install 'mark-six-studio.exe')) { throw 'Application remains after removal.' }
Install-App
Check-Data $before
Smoke-App
Write-Output 'PASS Windows install, reinstall, removal, retained database hashes and offline cached launch.'
