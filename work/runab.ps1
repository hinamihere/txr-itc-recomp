param([string]$Tag, [string]$Env, [int]$From = 2250, [int]$Every = 2, [int]$Max = 100)
# A/B run for the dark-surface glitch: Free Run race, dumps every $Every frames, prints the frames with a flat dark-grey area (blackpix.py > 2000).
Set-Location C:\itc-work
Remove-Item "C:\itc-work\dumps\frame${Tag}_*" -ErrorAction SilentlyContinue
$pad = (Get-Content padscript4.txt -Raw).Trim() + ',1800:RT:900'
$envl = "ITC_DUMP_FROM=$From;ITC_DUMP_EVERY=$Every;ITC_DUMP_MAX=$Max;$Env"
.\run_test.ps1 -Tag $Tag -Seconds 110 -Pad $pad -EnvList $envl | Out-Null
python blackpix.py $Tag > "C:\itc-work\$Tag.black.txt"
$rows = Get-Content "C:\itc-work\$Tag.black.txt"
$spikes = $rows | Where-Object { [int]($_.Split(' ')[1]) -gt 2000 }
"$Tag ($Env): $($rows.Count) frames, spikes: $($spikes -join ', ')"
