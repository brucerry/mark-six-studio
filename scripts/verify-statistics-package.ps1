param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$root = (Resolve-Path -LiteralPath $PackageDirectory).Path
$allowlist = Get-Content -LiteralPath (Join-Path $repo 'src/desktop/package-allowlist.txt') |
    Where-Object { $_.Length -gt 0 }
$expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($relative in $allowlist) {
    if (-not $expected.Add($relative)) { throw "Duplicate allowlisted file: $relative" }
}
$pending = [Collections.Generic.Stack[string]]::new()
$pending.Push($root)
$actual = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
while ($pending.Count) {
    $directory = $pending.Pop()
    if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Package contains reparse point: $directory"
    }
    foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Package contains reparse point: $($entry.FullName)"
        }
        $relative = [IO.Path]::GetRelativePath($root, $entry.FullName).Replace('\','/')
        if ($relative -eq '..' -or $relative.StartsWith('../')) {
            throw "Package entry escaped its root: $($entry.FullName)"
        }
        if ($entry.PSIsContainer) {
            $pending.Push($entry.FullName)
        } else {
            if ($relative -ceq 'manifest.json') { continue }
            if (-not $expected.Contains($relative)) { throw "Unexpected package file: $relative (root: $root; entry: $($entry.FullName))" }
            if (-not $actual.Add($relative)) { throw "Duplicate package path: $relative" }
            if ($entry.Length -eq 0) { throw "Empty package file: $relative" }
        }
    }
}
if ($actual.Count -ne $expected.Count) { throw "Package count mismatch: actual $($actual.Count), expected $($expected.Count)" }
foreach ($relative in $expected) {
    if (-not $actual.Contains($relative)) { throw "Missing package file: $relative" }
}
foreach ($required in @('mark-six-studio.exe','Qt6Core.dll','platforms/qwindows.dll',
    'tls/qschannelbackend.dll','qml/QtQuick/Controls/qmldir','qml/QtQuick/Dialogs/qmldir',
    'licenses/Qt-Notice.txt','licenses/LGPL-3.0-only.txt','licenses/GPL-3.0-only.txt',
    'licenses/SQLite.txt','sbom/qtbase-6.10.2.cdx.json','sbom/qtdeclarative-6.10.2.cdx.json')) {
    if (-not $actual.Contains($required)) { throw "Required runtime/resource/notice missing: $required" }
}
if (@($actual | Where-Object { $_ -match '(^|/)(PhysX|bgfx|node|electron|qmltooling)' -or
        $_ -match '\.(sqlite3|db|msx|mp4|ts)$' -or ($_ -match '\.exe$' -and $_ -cne 'mark-six-studio.exe') }).Count) {
    throw 'Obsolete runtime, source, or user data found in package.'
}
$manifestPath = Join-Path $root 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Missing package manifest.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.format -cne 'marksix-statistics-package-v1' -or $manifest.qtVersion -cne '6.10.2' -or
    @($manifest.files).Count -ne $expected.Count) { throw 'Invalid package manifest schema/count.' }
$manifestPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($file in $manifest.files) {
    $relative = [string]$file.path
    if (-not $expected.Contains($relative) -or -not $manifestPaths.Add($relative)) {
        throw "Unexpected/duplicate manifest entry: $relative"
    }
    $hash = (Get-FileHash -LiteralPath (Join-Path $root $relative) -Algorithm SHA256).Hash
    if ($hash -cne [string]$file.sha256) { throw "Package hash mismatch: $relative" }
}
Write-Output "PASS exact Qt statistics package: $($actual.Count) inventoried files, all hashes, no stray data/runtime or reparse points."
