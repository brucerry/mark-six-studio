param([Parameter(Mandatory)][string]$Installer)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Installation/removal fixture runs only on disposable GitHub Actions Windows runners.' }
$fixture = Join-Path $PSScriptRoot '../build/statistics-desktop/MarkSixUninstallFixture.exe'
if (-not (Test-Path -LiteralPath $fixture -PathType Leaf)) { throw 'Build MarkSixUninstallFixture before the disposable installation test.' }
$root = Join-Path $env:RUNNER_TEMP ('marksix-installer-' + [guid]::NewGuid().ToString('N'))
$install = Join-Path $root 'application'
$data = Join-Path $root 'retained-user-data'
New-Item -ItemType Directory -Path $data -Force | Out-Null
$sentinel = Join-Path $data 'preserve.txt'
Set-Content -LiteralPath $sentinel -Value 'Installer must retain this user data.' -Encoding utf8
function Install-App {
    if (-not (Test-Path -LiteralPath $install)) {
        # This custom CI install location must have the same protected payload
        # boundary as Program Files; an editable temp manifest is not authority.
        $acl = [Security.AccessControl.DirectorySecurity]::new()
        $acl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)')
        [void][IO.Directory]::CreateDirectory($install)
        Set-Acl -LiteralPath $install -AclObject $acl
    }
    $process = Start-Process -FilePath $Installer -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',"/DIR=`"$install`"") -Wait -PassThru
    if ($process.ExitCode -notin @(0,3010)) { throw "Installer failed: $($process.ExitCode)" }
}
function Smoke-App {
    $process = Start-Process -FilePath (Join-Path $install 'mark-six-studio.exe') -ArgumentList @('--smoke-load','--data-dir',"`"$data`"") -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Installed application failed: $($process.ExitCode)" }
    $proof = Join-Path $root 'native-install-proof.txt'
    $verification = Start-Process -FilePath (Join-Path $install 'mark-six-maintenance.exe') -ArgumentList '--verify-installation-state' -Wait -PassThru -RedirectStandardOutput $proof
    if (Test-Path -LiteralPath $proof) { Get-Content -LiteralPath $proof }
    if ($verification.ExitCode) { throw 'Native installation completion proof is not valid.' }
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
$secondHome = Join-Path $root 'second-profile'
$secondData = Join-Path $secondHome 'AppData/Local/MarkSixEmulator'
$secondLocal = Join-Path $secondHome 'AppData/Local'
$secondDownloads = Join-Path $secondHome 'Downloads'
New-Item -ItemType Directory -Path (Join-Path $secondData 'statistics'),$secondDownloads -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $data 'statistics') -Filter '*.sqlite3' | Copy-Item -Destination (Join-Path $secondData 'statistics')
$legacy = Join-Path $data 'legacy-保留.MSX'
$otherLegacy = Join-Path $secondData 'other-account.msx'
$installedLegacy = Join-Path $install 'retained-recording.MSX'
$foreign = Join-Path $secondData 'personal.txt'
foreach ($file in @($legacy,$otherLegacy,$installedLegacy,$foreign)) { [IO.File]::WriteAllText($file, 'Opaque isolated sentinel; never imported or parsed.') }
$sentinels = @{}
foreach ($file in @($legacy,$otherLegacy,$installedLegacy,$foreign)) { $sentinels[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
$older = Join-Path $secondDownloads 'MarkSixStudio-0.1.0-windows-x64-unsigned-setup.exe'
Invoke-WebRequest -Uri ('https://github.com/brucerry/mark-six-studio/releases/download/v0.1.0/' + [IO.Path]::GetFileName($older)) -OutFile $older
# Native profile/loaded-known-folder metadata fixtures. These are synthetic
# SIDs, not an account login or evidence of alternate-credential UAC acceptance.
$sid = 'S-1-5-21-1891342281-1282348759-2141736843-' + (Get-Random -Minimum 60000 -Maximum 65000)
$profile = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\' + $sid
$hive = 'Registry::HKEY_USERS\' + $sid
if ((Test-Path -LiteralPath $profile) -or (Test-Path -LiteralPath $hive)) { throw 'Refusing an existing native profile fixture.' }
$sourceName = 'Software\MarkSixStudioProfileFixture-' + [guid]::NewGuid().ToString('N')
$source = 'Registry::HKEY_CURRENT_USER\' + $sourceName
$hiveFile = Join-Path $root 'isolated-profile.hiv'
$reg = Join-Path $env:SystemRoot 'System32/reg.exe'
$loaded = $false
$offlineHives = @()
try {
    # The disposable runner image also has an offline installer account. Load
    # its real existing hive so native known-folder discovery has authority;
    # do not invent redirected paths or silently exclude that OS profile.
    # This is test setup only. The product never mounts profile hives.
    foreach ($entry in Get-ChildItem 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList') {
        $nativeSid = $entry.PSChildName
        if ($nativeSid -notmatch '^S-1-(?:5-21|12-1)-[0-9-]+$' -or (Test-Path -LiteralPath ('Registry::HKEY_USERS\' + $nativeSid))) { continue }
        $nativeHome = [Environment]::ExpandEnvironmentVariables((Get-ItemProperty -LiteralPath $entry.PSPath).ProfileImagePath)
        $nativeHive = Join-Path $nativeHome 'NTUSER.DAT'
        if (-not (Test-Path -LiteralPath $nativeHive -PathType Leaf)) { throw "Offline CI profile has no inspectable hive: $nativeSid" }
        & $reg load ('HKU\' + $nativeSid) $nativeHive
        if ($LASTEXITCODE) { throw "Could not load offline CI profile: $nativeSid" }
        $offlineHives += $nativeSid
    }
    New-Item -Path $profile -Force | Out-Null
    New-ItemProperty -Path $profile -Name ProfileImagePath -Value $secondHome -PropertyType ExpandString | Out-Null
    $folders = $source + '\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders'
    New-Item -Path $folders -Force | Out-Null
    New-ItemProperty -Path $folders -Name 'Local AppData' -Value $secondLocal -PropertyType ExpandString | Out-Null
    New-ItemProperty -Path $folders -Name '{374DE290-123F-4565-9164-39C4925E467B}' -Value $secondDownloads -PropertyType ExpandString | Out-Null
    # HKEY_USERS accepts loaded hives, not arbitrary new root keys. Save only
    # this private fixture key, then mount that generated hive on disposable CI.
    # Production discovery never loads or creates NTUSER.DAT.
    & $reg save ('HKCU\' + $sourceName) $hiveFile /y
    if ($LASTEXITCODE) { throw 'Could not save the isolated native profile fixture.' }
    & $reg load ('HKU\' + $sid) $hiveFile
    if ($LASTEXITCODE) { throw 'Could not mount the isolated native profile fixture.' }
    $loaded = $true
    & $fixture $install $data $legacy
    if ($LASTEXITCODE) { throw "Native full-uninstall/retry fixture failed: $LASTEXITCODE" }
    if ((Test-Path -LiteralPath $older) -or @(Get-ChildItem -LiteralPath (Join-Path $secondData 'statistics') -Filter '*.sqlite3' -ErrorAction SilentlyContinue).Count) {
        throw 'Other-profile records or older downloaded installer remains.'
    }
    foreach ($file in $sentinels.Keys) {
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -cne $sentinels[$file]) { throw "Native uninstall changed preserved sentinel: $file" }
    }
} finally {
    if (Test-Path -LiteralPath $profile) { Remove-Item -LiteralPath $profile -Recurse -Force }
    if ($loaded) { & $reg unload ('HKU\' + $sid) }
    foreach ($nativeSid in $offlineHives) { & $reg unload ('HKU\' + $nativeSid) }
    if (Test-Path -LiteralPath $source) { Remove-Item -LiteralPath $source -Recurse -Force }
}
Write-Output 'PASS ordinary retention plus native full uninstall, second-profile/download removal, preserved legacy/foreign hashes and standalone retry.'
