param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cache = Join-Path $repo 'deps/installer-downloads'
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$items = @(
    @('innosetup-7.1.0-x64.exe', 'https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/innosetup-7.1.0-x64.exe',
      '0362A383ED217D4C4239B5933866DD96D3EB2102737DA92F80F6057A4B40DF2F'),
    @('VC_redist.x64.exe', 'https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe',
      'CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B')
)
foreach ($item in $items) {
    $name,$url,$hash = $item
    $file = Join-Path $cache $name
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Invoke-WebRequest -Uri $url -OutFile $file }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -cne $hash) { throw "Pinned installer dependency changed: $file" }
}
$redist = Join-Path $cache 'VC_redist.x64.exe'
$signature = Get-AuthenticodeSignature -LiteralPath $redist
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
    throw 'VC runtime prerequisite has no valid Microsoft signature.'
}
$inno = Join-Path $repo 'deps/InnoSetup-7.1.0'
if (-not (Test-Path -LiteralPath (Join-Path $inno 'ISCC.exe') -PathType Leaf)) {
    if (Test-Path -LiteralPath $inno) { throw "Incomplete Inno Setup directory: $inno" }
    $process = Start-Process -FilePath (Join-Path $cache 'innosetup-7.1.0-x64.exe') -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',"/DIR=`"$inno`"") -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Inno Setup installation failed: $($process.ExitCode)" }
}
$compilerVersion = & (Join-Path $inno 'ISCC.exe') --version
if ($LASTEXITCODE -ne 0 -or ($compilerVersion -join '').Trim() -cne '7.1.0') {
    throw "Inno Setup compiler version mismatch: $compilerVersion"
}
Write-Output 'Verified Inno Setup 7.1.0 and Microsoft VC runtime prerequisite.'
