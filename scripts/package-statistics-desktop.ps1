param([string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$qt = Join-Path $repo 'deps/Qt-6.10.2-msvc2022_64'
$application = Join-Path $repo 'build/statistics-desktop/mark-six-studio.exe'
if (-not $PackageDirectory) { $PackageDirectory = Join-Path $repo 'release/mark-six-studio' }
$destination = [IO.Path]::GetFullPath($PackageDirectory)
if (Test-Path -LiteralPath $destination) { throw "Package target already exists; choose a new directory: $destination" }
if (-not (Test-Path -LiteralPath $application -PathType Leaf)) { throw 'Build the statistics desktop first.' }
$deploy = Join-Path $qt 'bin/windeployqt.exe'
if (-not (Test-Path -LiteralPath $deploy -PathType Leaf)) { throw 'Pinned Qt deployment tool is missing.' }
$licenseSource = Join-Path $qt 'redistribution-licenses'
$licenseHashes = @{
    'LGPL-3.0-only.txt' = 'DA7EABB7BAFDF7D3AE5E9F223AA5BDC1EECE45AC569DC21B3B037520B4464768'
    'GPL-3.0-only.txt' = '8CEB4B9EE5ADEDDE47B31E975C1D90C73AD27B6B165A1DCD80C7C545EB65B903'
}
foreach ($name in $licenseHashes.Keys) {
    $path = Join-Path $licenseSource $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $licenseHashes[$name]) {
        throw "Pinned Qt 6.10.2 license copy missing/changed: $path. Fetch the exact Qt v6.10.2 LICENSES file."
    }
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath $application -Destination $destination
$previousPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $qt 'bin');$env:PATH"
    $excludeDebug = 'qmldbg_debugger,qmldbg_inspector,qmldbg_local,qmldbg_messages,qmldbg_native,qmldbg_nativedebugger,qmldbg_preview,qmldbg_profiler,qmldbg_quickprofiler,qmldbg_server,qmldbg_tcp'
    & $deploy --release --no-translations --no-compiler-runtime --exclude-plugins $excludeDebug `
        --qmldir (Join-Path $repo 'src/desktop') --dir $destination (Join-Path $destination 'mark-six-studio.exe') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }
} finally { $env:PATH = $previousPath }
New-Item -ItemType Directory -Path (Join-Path $destination 'licenses') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $destination 'sbom') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'src/desktop/package-readme.md') -Destination (Join-Path $destination 'README.md')
Copy-Item -LiteralPath (Join-Path $repo 'src/desktop/qt-redistribution-notice.txt') -Destination (Join-Path $destination 'licenses/Qt-Notice.txt')
Copy-Item -LiteralPath (Join-Path $repo 'src/resources/sqlite-notice.txt') -Destination (Join-Path $destination 'licenses/SQLite.txt')
foreach ($name in $licenseHashes.Keys) {
    Copy-Item -LiteralPath (Join-Path $licenseSource $name) -Destination (Join-Path $destination "licenses/$name")
}
foreach ($name in 'qtbase-6.10.2.cdx.json','qtdeclarative-6.10.2.cdx.json') {
    Copy-Item -LiteralPath (Join-Path $qt "sbom/$name") -Destination (Join-Path $destination "sbom/$name")
}
$expected = @(Get-Content -LiteralPath (Join-Path $repo 'src/desktop/package-allowlist.txt') |
    Where-Object { $_.Length -gt 0 })
$actual = @(Get-ChildItem -LiteralPath $destination -Recurse -File |
    ForEach-Object { $_.FullName.Substring($destination.TrimEnd('\','/').Length + 1).Replace('\','/') })
$extra = @($actual | Where-Object { $_ -cnotin $expected })
$missing = @($expected | Where-Object { $_ -cnotin $actual })
if ($extra.Count -or $missing.Count) {
    throw "Qt package inventory changed. Extra: $($extra -join ', '); missing: $($missing -join ', '). Inspect the pinned deployment before updating the allowlist."
}
$files = foreach ($relative in $expected) {
    $role = if ($relative -eq 'mark-six-studio.exe') { 'application' } elseif ($relative.StartsWith('licenses/')) { 'license' } `
        elseif ($relative.StartsWith('sbom/')) { 'sbom' } elseif ($relative.EndsWith('.dll')) { 'runtime' } else { 'resource' }
    [ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath (Join-Path $destination $relative) -Algorithm SHA256).Hash; role = $role }
}
$manifest = [ordered]@{ format = 'marksix-statistics-package-v1'; qtVersion = '6.10.2'; files = @($files) }
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'manifest.json') -Encoding utf8
& (Join-Path $PSScriptRoot 'verify-statistics-package.ps1') -PackageDirectory $destination
if ($LASTEXITCODE -ne 0) { throw 'Statistics package verification failed.' }
Write-Output "Package: $destination"
