param(
    [string]$Tag = 'rt',
    [int]$Seconds = 90,
    [string]$Pad = '450:START:15,520:DOWN:8,560:A:8',
    [string]$EnvList = ''
)
# Runs the recompiled game for N seconds with status logging, then kills it and prints the [stat] summary.
# NOTE: kills EVERY running itc.exe first (including a game window the user is playing in).
Get-Process itc -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
$env:ITC_WATCHDOG = [string]($Seconds + 60)
$env:ITC_STATUS = '1'
if ($Pad -ne '') { $env:ITC_PAD_SCRIPT = $Pad }
$env:ITC_DUMP_TAG = $Tag
foreach ($kv in ($EnvList -split ';')) {
    if ($kv -match '^([^=]+)=(.*)$') {
        if ($Matches[2] -eq '') { Remove-Item -Path ("Env:" + $Matches[1]) -ErrorAction SilentlyContinue } else { Set-Item -Path ("Env:" + $Matches[1]) -Value $Matches[2] }
    }
}
$err = "C:\itc-work\$Tag.err"
$out = "C:\itc-work\$Tag.out"
# stderr goes straight to the file: a -RedirectStandardError pipe blocks the game's log writes for milliseconds whenever PowerShell is slow to drain it
# (that showed up as a 50 ms frame every 2 s, exactly when the [stat] block is printed).
$cmdLine = '/c "C:\itc-work\rt_build\itc.exe extracted\default.xex 2>' + $err + ' 1>' + $out + '"'
$p = Start-Process -FilePath 'cmd.exe' -ArgumentList $cmdLine -WorkingDirectory 'C:\itc-work' -PassThru -WindowStyle Minimized
if (-not $p.WaitForExit($Seconds * 1000)) {
    Get-Process itc -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
    try { $p.Kill() } catch {}
    Start-Sleep -Milliseconds 500
}
"exit: $($p.ExitCode)"
Select-String -Path $err -Pattern '\[stat\]' | Select-Object -Last 14 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(400, $_.Line.Length)) }
