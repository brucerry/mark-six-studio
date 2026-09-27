param()
$ErrorActionPreference = 'Stop'
# Package identities/hashes from the installed Visual Studio Installer catalog.
# Requires the user's existing licensed Visual Studio Build Tools installation.
# Toolchain files are for local builds only; they are never put in the app package.
$deps = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../deps'))
$cache = Join-Path $deps 'msvc-downloads'
$destination = Join-Path $deps 'msvc-14.44'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'An existing Visual Studio C++ Build Tools installation is required.' }
$packages = @(
    @('aa209763-134c-427b-9960-9ca54ccc1734', 'f4ad3cee4ef18e9f36382399261de27be6910ab3467178d7e687377ec841306d', 'Tools.HostX64.TargetX64.base'),
    @('aa209763-134c-427b-9960-9ca54ccc1734', '603375e4d6d639bc6e0a7d0d657b17e0c3b72e52450f78eb41e987f40c3b5305', 'Tools.HostX64.TargetX64.Res.base.enu'),
    @('c610cd8c-801b-44b8-a80a-82cc382aeb43', '852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5', 'CRT.Headers.base'),
    @('67cf767c-5e71-47c2-a54a-cd5631e28942', 'f01f701a7bcd9587a340898c851424f6a52bb913a70c185ff0d5bf0288c5831a', 'CRT.x64.Desktop.base'),
    @('67cf767c-5e71-47c2-a54a-cd5631e28942', '9135b03c0df53c7a0aa9bef7230a1c2ff4263a0ee7baa7e419d034f484f6bb56', 'CRT.x64.Store.base')
)
New-Item -ItemType Directory -Force -Path $cache,$destination | Out-Null
foreach ($package in $packages) {
    $group,$hash,$suffix = $package
    $name = "Microsoft.VC.14.44.17.14.$suffix.vsix"
    $archive = Join-Path $cache "$name.zip"
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host "Downloading Microsoft $suffix"
        Invoke-WebRequest "https://download.visualstudio.microsoft.com/download/pr/$group/$hash/$name" -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $hash) { throw "MSVC checksum mismatch: $archive" }
    $unpacked = Join-Path $cache $suffix
    if (-not (Test-Path -LiteralPath $unpacked)) { Expand-Archive -LiteralPath $archive -DestinationPath $unpacked }
    $contents = Join-Path $unpacked 'Contents'
    foreach ($file in (Get-ChildItem -LiteralPath $contents -Recurse -File)) {
        $target = Join-Path $destination ([IO.Path]::GetRelativePath($contents, $file.FullName))
        if ((Test-Path -LiteralPath $target) -and (Get-FileHash $target).Hash -eq (Get-FileHash $file.FullName).Hash) { continue }
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
}
# NVCC checks for genuine VC setup scripts even when using --use-local-env.
$auxiliary = Join-Path $destination 'VC/Auxiliary'
New-Item -ItemType Directory -Force -Path $auxiliary | Out-Null
Copy-Item -LiteralPath (Join-Path $vs 'VC/Auxiliary/Build') -Destination $auxiliary -Recurse -Force
$compiler = Get-ChildItem (Join-Path $destination 'VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe') | Select-Object -First 1
if (-not $compiler) { throw 'Portable MSVC compiler not found after extraction.' }
$signature = Get-AuthenticodeSignature -LiteralPath $compiler.FullName
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') { throw 'MSVC compiler signature verification failed.' }
Write-Host "Verified Microsoft compiler: $($compiler.FullName)"
