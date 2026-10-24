param([switch]$Tests, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$qt = Join-Path $repo 'deps/Qt-6.10.2-msvc2022_64'
$toolchain = Join-Path $repo 'deps/msvc-14.44/VC/Tools/MSVC/14.44.35207'
$build = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $repo 'build/statistics-desktop' }
if (-not (Test-Path -LiteralPath (Join-Path $qt 'lib/cmake/Qt6/Qt6Config.cmake'))) {
    throw 'Pinned Qt 6.10.2 kit is missing; run scripts/fetch-statistics-qt.ps1 -Fetch.'
}
if (-not (Test-Path -LiteralPath (Join-Path $toolchain 'bin/Hostx64/x64/cl.exe'))) {
    throw 'Pinned MSVC 14.44 compiler is missing.'
}
& (Join-Path $PSScriptRoot 'fetch-sqlite.ps1')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ Build Tools are required.' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'
$ninja = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdkVersion = Get-ChildItem (Join-Path $sdk 'Lib') -Directory |
    Sort-Object Name -Descending |
    Where-Object {
        $tools = Join-Path $sdk "bin/$($_.Name)/x64"
        (Test-Path -LiteralPath (Join-Path $tools 'rc.exe') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $tools 'mt.exe') -PathType Leaf)
    } |
    Select-Object -First 1 -ExpandProperty Name
if (-not $sdkVersion) { throw 'Windows SDK with x64 rc.exe and mt.exe is required.' }
$sdkBin = Join-Path $sdk "bin/$sdkVersion/x64"
$resourceCompiler = (Join-Path $sdkBin 'rc.exe').Replace('\', '/')
$manifestTool = (Join-Path $sdkBin 'mt.exe').Replace('\', '/')
$saved = @{}
foreach ($key in 'PATH','INCLUDE','LIB','VCINSTALLDIR','VCToolsInstallDir','VSCMD_VER','QT_QPA_PLATFORM') {
    $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
}
try {
    $env:PATH = "$qt/bin;$toolchain/bin/Hostx64/x64;$sdkBin;$env:PATH"
    $env:INCLUDE = "$toolchain/include;$sdk/Include/$sdkVersion/ucrt;$sdk/Include/$sdkVersion/shared;$sdk/Include/$sdkVersion/um;$sdk/Include/$sdkVersion/winrt"
    $env:LIB = "$toolchain/lib/x64;$sdk/Lib/$sdkVersion/ucrt/x64;$sdk/Lib/$sdkVersion/um/x64"
    $env:VCINSTALLDIR = Join-Path $repo 'deps/msvc-14.44/VC/'
    $env:VCToolsInstallDir = "$toolchain/"
    $env:VSCMD_VER = '17.14'
    $options = @('-S',(Join-Path $repo 'src/desktop'),'-B',$build,'-G','Ninja',
        '-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_C_COMPILER=$toolchain/bin/Hostx64/x64/cl.exe",
        "-DCMAKE_CXX_COMPILER=$toolchain/bin/Hostx64/x64/cl.exe",
        "-DCMAKE_RC_COMPILER=$resourceCompiler",
        "-DCMAKE_MT=$manifestTool",
        "-DCMAKE_PREFIX_PATH=$qt",'-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL')
    & $cmake @options
    if ($LASTEXITCODE) { throw 'Statistics desktop configure failed.' }
    $targets = @('MarkSixStudio','MarkSixMaintenance')
    if ($Tests) { $targets += @('MarkSixStatisticsTests','MarkSixStatisticsQtModelTests','MarkSixAppUpdateTests','MarkSixMaintenanceTests','MarkSixStorageUiTests','MarkSixMaintenanceLifecycleFixture','MarkSixUninstallFixture') }
    & $cmake --build $build --target @targets --parallel 4
    if ($LASTEXITCODE) { throw 'Statistics desktop build failed.' }
    if ($Tests) {
        & $ctest --test-dir $build --output-on-failure -L 'statistics|updates|maintenance'
        if ($LASTEXITCODE) { throw 'Statistics desktop backend tests failed.' }
        $env:QT_QPA_PLATFORM = 'offscreen'
        $auditData = Join-Path $build 'update-audit-data'
        $audit = Start-Process -FilePath (Join-Path $build 'mark-six-studio.exe') `
            -ArgumentList @('--app-update-audit','--data-dir',('"{0}"' -f $auditData)) -Wait -PassThru
        if ($audit.ExitCode) { throw 'Application update dialog audit failed.' }
        $cleanupData = Join-Path $build 'cleanup-audit-data'
        $cleanupOut = Join-Path $build 'cleanup-audit.stdout.txt'
        $cleanupError = Join-Path $build 'cleanup-audit.stderr.txt'
        $cleanup = Start-Process -FilePath (Join-Path $build 'mark-six-studio.exe') `
            -ArgumentList @('--storage-cleanup-audit','--data-dir',('"{0}"' -f $cleanupData)) `
            -RedirectStandardOutput $cleanupOut -RedirectStandardError $cleanupError -Wait -PassThru
        Get-Content -LiteralPath $cleanupOut
        Get-Content -LiteralPath $cleanupError
        if ($cleanup.ExitCode) { throw ('Storage cleanup review and cancellation audit failed: ' + $cleanup.ExitCode) }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
Write-Output (Join-Path $build 'mark-six-studio.exe')
