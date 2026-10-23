param([Parameter(Mandatory)][string]$Path)
$ErrorActionPreference = 'Stop'
if (-not $env:WINDOWS_SIGN_CERT_THUMBPRINT) { throw 'WINDOWS_SIGN_CERT_THUMBPRINT is required; refusing unsigned release.' }
$signTool = $env:WINDOWS_SIGNTOOL
if (-not $signTool) {
    $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $signTool = Get-ChildItem -Path (Join-Path $sdk '*/x64/signtool.exe') |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $signTool) { throw 'Windows SDK signtool.exe is required.' }
& $signTool sign /sha1 $env:WINDOWS_SIGN_CERT_THUMBPRINT /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $Path
if ($LASTEXITCODE) { throw 'Windows code signing failed.' }
& $signTool verify /pa /all $Path
if ($LASTEXITCODE -or (Get-AuthenticodeSignature -LiteralPath $Path).Status -ne 'Valid') { throw 'Windows signature verification failed.' }
