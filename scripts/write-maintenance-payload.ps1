param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path -LiteralPath $PackageDirectory).Path
$lines = [Collections.Generic.List[string]]::new()
$pending = [Collections.Generic.Stack[string]]::new()
$pending.Push($root)
while ($pending.Count) {
    $directory = $pending.Pop()
    if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Redirected payload directory.' }
    foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Windows payload links are not supported.' }
        $relative = [IO.Path]::GetRelativePath($root, $entry.FullName).Replace('\','/')
        if ($relative -cin @('manifest.json','maintenance-payload.txt')) { continue }
        $encoded = [Uri]::EscapeDataString($relative).Replace('%2F','/')
        if ($entry.PSIsContainer) {
            $lines.Add("D`t-`t$encoded")
            $pending.Push($entry.FullName)
        } else {
            if ($entry.Extension.ToLowerInvariant() -in @('.msx','.sqlite','.sqlite3','.db','.pfx','.p12')) { throw 'User data or credentials in payload.' }
            $digest = (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $lines.Add("F`t$digest`t$encoded")
        }
        if ($lines.Count -gt 100000) { throw 'Payload entry limit exceeded.' }
    }
}
$content = "M6PAYLOAD`t1`tcom.brucerry.marksixstudio`n" + (($lines | Sort-Object -CaseSensitive) -join "`n") + "`n"
if ([Text.Encoding]::UTF8.GetByteCount($content) -gt 8MB) { throw 'Payload manifest size limit exceeded.' }
[IO.File]::WriteAllText((Join-Path $root 'maintenance-payload.txt'), $content, [Text.UTF8Encoding]::new($false))
