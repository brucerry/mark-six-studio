param([switch]$Fetch)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$deps = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../deps'))
$kit = Join-Path $deps 'Qt-6.10.2-msvc2022_64'
$cache = Join-Path $deps 'qt-downloads/6.10.2-msvc2022_64'
$base = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6102/qt6_6102/qt.qt6.6102.win64_msvc2022_64/'
$archives = @(
    @('6.10.2-0-202601261212qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z',
      'C4CEDCC54D2036AB20B193DB113CF324D90F390647F831A522952CC9B158F38B'),
    @('6.10.2-0-202601261212qtdeclarative-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z',
      'C3EA627A3944601B78F54EE6F0315759B35550015AF47F89CD432D96FD1AA873')
)
$config = Join-Path $kit 'lib/cmake/Qt6/Qt6Config.cmake'
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
    if (Test-Path -LiteralPath $kit) { throw "Incomplete Qt kit exists; inspect it before retrying: $kit" }
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    foreach ($item in $archives) {
        $name,$hash = $item
        $file = Join-Path $cache $name
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            if (-not $Fetch) { throw "Missing pinned Qt archive: $file. Run with -Fetch to download." }
            Invoke-WebRequest -Uri ($base + $name) -OutFile $file
        }
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -cne $hash) {
            throw "Pinned Qt archive hash mismatch: $file"
        }
    }
    $staging = Join-Path $deps ('qt-extract-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    $sevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
    if (-not $sevenZip) {
        $installedSevenZip = Join-Path $env:ProgramFiles '7-Zip/7z.exe'
        if (Test-Path -LiteralPath $installedSevenZip -PathType Leaf) {
            $sevenZip = $installedSevenZip
        }
    }
    foreach ($item in $archives) {
        $archive = Join-Path $cache $item[0]
        if ($sevenZip) {
            & $sevenZip x -y "-o$staging" $archive | Out-Null
        } else {
            & tar -xf $archive -C $staging
        }
        if ($LASTEXITCODE -ne 0) { throw "Qt archive extraction failed: $archive; inspected staging retained at $staging" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $staging 'lib/cmake/Qt6/Qt6Config.cmake'))) {
        throw "Qt extraction missing its CMake package; staging retained at $staging"
    }
    $within = $deps.TrimEnd('\') + '\'
    if (-not ([IO.Path]::GetFullPath($staging)).StartsWith($within, [StringComparison]::OrdinalIgnoreCase) -or
        -not ([IO.Path]::GetFullPath($kit)).StartsWith($within, [StringComparison]::OrdinalIgnoreCase) -or
        (Test-Path -LiteralPath $kit)) { throw 'Unsafe or occupied Qt kit move target.' }
    Move-Item -LiteralPath $staging -Destination $kit
}
if (-not (Test-Path -LiteralPath (Join-Path $kit 'bin/windeployqt.exe')) -or
    -not (Test-Path -LiteralPath (Join-Path $kit 'qml/QtQuick/Controls/qmldir'))) {
    throw "Pinned Qt kit is incomplete: $kit"
}
$licenses = Join-Path $kit 'redistribution-licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
foreach ($item in @(
    @('LGPL-3.0-only.txt','DA7EABB7BAFDF7D3AE5E9F223AA5BDC1EECE45AC569DC21B3B037520B4464768'),
    @('GPL-3.0-only.txt','8CEB4B9EE5ADEDDE47B31E975C1D90C73AD27B6B165A1DCD80C7C545EB65B903')
)) {
    $name,$hash = $item
    $file = Join-Path $licenses $name
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        if (-not $Fetch) { throw "Missing Qt redistribution license: $file. Run with -Fetch." }
        Invoke-WebRequest -Uri "https://raw.githubusercontent.com/qt/qtbase/v6.10.2/LICENSES/$name" -OutFile $file
    }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -cne $hash) {
        throw "Pinned Qt license hash mismatch: $file"
    }
}
Write-Output "Verified pinned Qt 6.10.2 kit and redistribution texts: $kit"
