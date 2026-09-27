param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = (Resolve-Path -LiteralPath $PackageDirectory).Path
$fixture = Join-Path $env:TEMP ('marksix-package-negative-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
foreach ($entry in Get-ChildItem -LiteralPath $source -Force) {
    Copy-Item -LiteralPath $entry.FullName -Destination $fixture -Recurse
}
$verify = Join-Path $PSScriptRoot 'verify-statistics-package.ps1'
function Expect-Rejection([string]$Label) {
    $rejected = $false
    try { & $verify -PackageDirectory $fixture | Out-Null }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Verifier accepted negative fixture: $Label" }
    Write-Output "PASS reject $Label"
}
& $verify -PackageDirectory $fixture | Out-Null
$runtime = Join-Path $fixture 'node.exe'
New-Item -ItemType File -Path $runtime | Out-Null
Expect-Rejection 'stray runtime'
Remove-Item -LiteralPath $runtime
$database = Join-Path $fixture 'results.sqlite3'
New-Item -ItemType File -Path $database | Out-Null
Expect-Rejection 'stray database'
Remove-Item -LiteralPath $database
$readme = Join-Path $fixture 'README.md'
Add-Content -LiteralPath $readme -Value 'tamper fixture'
Expect-Rejection 'modified content hash'
Copy-Item -LiteralPath (Join-Path $source 'README.md') -Destination $readme -Force
$outside = Join-Path $env:TEMP ('marksix-package-link-target-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $outside | Out-Null
New-Item -ItemType Junction -Path (Join-Path $fixture 'linked-data') -Target $outside | Out-Null
Expect-Rejection 'reparse point'
Write-Output "Negative fixture retained at $fixture (safe temporary test copy; no user data copied into it)."
