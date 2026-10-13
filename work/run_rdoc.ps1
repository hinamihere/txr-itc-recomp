param(
    [string]$Tag = 'rd',
    [int]$Seconds = 130,
    [string]$Pad = '99999:A:1',
    [int]$Frame = 1720,
    [string]$EnvList = ''
)
# Runs the game through renderdoccmd and captures presented frame number $Frame (ITC_RDOC_FRAME) into C:\itc-work\rdoc\itc_*.rdc
Get-Process itc -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
Get-Process renderdoccmd -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
$env:ITC_WATCHDOG = [string]($Seconds + 60)
$env:ITC_STATUS = '1'
$env:ITC_NO_XINPUT = '1'
$env:ITC_PAD_SCRIPT = $Pad
$env:ITC_DUMP_TAG = $Tag
$env:ITC_RDOC_FRAME = [string]$Frame
$env:ITC_RDOC_QUIT = '1'      # the game exits by itself right after the capture is written
$env:ITC_IDX_UPDATE = '1'      # RenderDoc cannot cope with the big mapped index ring
foreach ($kv in ($EnvList -split ';')) {
    if ($kv -match '^([^=]+)=(.*)$') { Set-Item -Path ("Env:" + $Matches[1]) -Value $Matches[2] }
}
New-Item -ItemType Directory -Force 'C:\itc-work\rdoc' | Out-Null
$err = "C:\itc-work\$Tag.err"
$out = "C:\itc-work\$Tag.out"
$args2 = @('capture', '-w', '-d', 'C:\itc-work', '-c', 'C:\itc-work\rdoc\itc', 'C:\itc-work\rt_build\itc.exe', 'extracted\default.xex')
$p = Start-Process -FilePath 'C:\Program Files\RenderDoc\renderdoccmd.exe' -ArgumentList $args2 -WorkingDirectory 'C:\itc-work' -PassThru -RedirectStandardError $err -RedirectStandardOutput $out -WindowStyle Minimized
if (-not $p.WaitForExit($Seconds * 1000)) {
    Get-Process itc -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
    Start-Sleep -Milliseconds 1500
    try { $p.Kill() } catch {}
}
"exit: $($p.ExitCode)"
Get-ChildItem 'C:\itc-work\rdoc' | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String
Select-String -Path $err -Pattern '\[rdoc\]' | Select-Object -Last 6 | ForEach-Object { $_.Line }
