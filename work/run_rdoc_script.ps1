param(
    [string]$Script,
    [string]$Capture,
    [string]$OutDir,
    [string]$Extra = '',
    [int]$TimeoutSec = 420
)
# Runs a RenderDoc python script headlessly: qrenderdoc.exe --python <Script> with RDOC_CAP / RDOC_OUT set; $Extra = "NAME=value;NAME2=value2" for more variables
Get-Process qrenderdoc -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
$env:RDOC_CAP = $Capture
$env:RDOC_OUT = $OutDir
foreach ($kv in ($Extra -split '\|')) {
    if ($kv -match '^([^=]+)=(.*)$') { Set-Item -Path ("Env:" + $Matches[1]) -Value $Matches[2] }
}
$p = Start-Process -FilePath 'C:\Program Files\RenderDoc\qrenderdoc.exe' -ArgumentList '--python', $Script -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { try { $p.Kill() } catch {}; 'killed after timeout' } else { 'qrenderdoc exited' }
