param([switch]$Fetch)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$deps = Join-Path $repo 'deps'
$folder = Join-Path $deps 'sqlite-amalgamation-3530400'
$archive = Join-Path $deps 'sqlite-amalgamation-3530400.zip'
$url = 'https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip'
# Official SHA3-256 archive/source hashes verified on 2026-09-25 before recording
# these SHA-256 pins. SHA-256 keeps subsequent verification available on Windows 10
# and Windows PowerShell, without requiring newer platform SHA-3 support.
$archiveHash = '1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d'
$sourceHash = 'b1dd5d74ec7f29055a6684fa06fb3c2f6821c87dd38f9a458dfd2e8a1db28189'
if (-not (Test-Path -LiteralPath $folder)) {
    if (-not $Fetch) { throw 'Pinned SQLite missing. Run ./scripts/fetch-sqlite.ps1 -Fetch.' }
    New-Item -ItemType Directory -Force -Path $deps | Out-Null
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri $url -OutFile $archive -TimeoutSec 60 -MaximumRedirection 0
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $archiveHash) { throw 'SQLite archive hash mismatch; retained without extraction.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $expected = @('sqlite-amalgamation-3530400/','sqlite-amalgamation-3530400/sqlite3.c','sqlite-amalgamation-3530400/sqlite3.h',
            'sqlite-amalgamation-3530400/sqlite3ext.h','sqlite-amalgamation-3530400/shell.c')
        foreach ($entry in $zip.Entries) { if ($entry.FullName -notin $expected) { throw 'Unexpected SQLite archive entry.' } }
    } finally { $zip.Dispose() }
    # Destination does not exist; extraction never overwrites a user-edited dependency.
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $deps)
}
if ((Get-FileHash -LiteralPath (Join-Path $folder 'sqlite3.c') -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Pinned SQLite source changed; existing files were NOT replaced.' }
if ((Get-FileHash -LiteralPath (Join-Path $folder 'sqlite3.h') -Algorithm SHA256).Hash -ne '919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d') {
    throw 'Pinned SQLite header changed; existing files were NOT replaced.'
}
if (-not (Select-String -LiteralPath (Join-Path $folder 'sqlite3.h') -Pattern '^#define SQLITE_VERSION\s+"3\.53\.4"$' -Quiet)) {
    throw 'SQLite header version mismatch.'
}
Write-Host 'Verified SQLite 3.53.4 amalgamation (public-domain deliverable, no shell executable).'
