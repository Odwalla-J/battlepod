# start-centre.ps1 - start a one-PC Firestorm centre: the pod's launcher and
# the operator console, each through fsrun so the shim is in them.
#
#   start-centre.ps1 -Game C:\path\to\MW4            launcher + console
#   start-centre.ps1 -Game ... -Mission -Pilot Adam   ...and set up and launch
#                                                     a mission against one bot
#   -NoBot     no computer opponent       -Stop    stop everything and exit
#   -NoLaunch  load the mission but leave LAUNCH to the operator
#   -NoPrint   without mw4print, the battle-report printer
#
# The launcher starts the pod (ctcl.ini [Games] 1=) when the console asks.
# Each step waits for the shim's log to say the last one is ready (the pod's
# first frame, the console's, the pod's panel lit after LOAD) rather than for
# a fixed time, so a centre is up in about half a minute; -LoadWait is only
# the longest it waits for the pod to load.
# -Mission clicks through the console's first screen the way an operator
# would; it assumes the console's window is the size the shim gives it and
# that the slot-1 list holds Pilot1 (ctcl.ini [teslas]). Needs Python.
param(
  [string]$Game = (Join-Path $PSScriptRoot "..\..\firestorm-run\MW4"),
  [switch]$Mission, [switch]$NoBot, [switch]$Stop, [switch]$NoLaunch, [switch]$NoPrint,
  [string]$Pilot = "Pilot", [int]$LoadWait = 30
)
$ErrorActionPreference = "Stop"
$Game = (Resolve-Path $Game).Path
$desk = Join-Path $PSScriptRoot "desk.py"

$running = @(Get-Process MW4, fsrun, Launcher, mw4print -ErrorAction SilentlyContinue)
$running | Stop-Process -Force
$running | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
if ($running) { Start-Sleep -Seconds 1 }     # until Windows lets go of their DLLs
if ($Stop) { "stopped"; return }
if (-not (Test-Path "$Game\fsrun.exe") -or -not (Test-Path "$Game\ddraw.dll")) {
  throw "no fsrun.exe/ddraw.dll in $Game - run 'make install' first"
}

# The shim's logs, fresh for each run: what each copy has got to.
$logs = "launcher", "ctcl1", "ctcl2", "mw4print" | ForEach-Object { Join-Path $Game "shim-$_.log" }
$logs | Where-Object { Test-Path $_ } | Remove-Item -Force
function Lines([string]$log, [string]$pattern) {
  if (-not (Test-Path $log)) { return 0 }
  @(Get-Content $log -ErrorAction SilentlyContinue | Select-String -SimpleMatch $pattern).Count
}
function WaitFor([string]$log, [string]$pattern, [int]$seconds, [int]$more = 0) {
  $until = (Get-Date).AddSeconds($seconds)
  while ((Lines $log $pattern) -le $more) {
    if ((Get-Date) -gt $until) { return $false }
    Start-Sleep -Milliseconds 250
  }
  $true
}

# At the end of a match the console hands mw4print each pod's record to
# print, so it has to be running already. Minimised: it is a blank window.
if (-not $NoPrint) {
  Start-Process -FilePath "$Game\fsrun.exe" -ArgumentList "-exe mw4print.exe" -WorkingDirectory $Game -WindowStyle Minimized
}
Start-Process -FilePath "$Game\fsrun.exe" -ArgumentList "-exe Launcher.exe" -WorkingDirectory $Game -WindowStyle Hidden
# The launcher listens and starts the pod; once the pod draws, the console.
WaitFor $logs[2] "main1: first frame" 30 | Out-Null
Start-Sleep -Seconds 1
Start-Process -FilePath "$Game\fsrun.exe" -ArgumentList "-ctcltype 1 /gosnoblade" -WorkingDirectory $Game -WindowStyle Hidden
"launcher and console started"
if (-not $Mission) { return }

if (-not (WaitFor $logs[1] "main1: first frame" 30)) { throw "the console did not start" }
Start-Sleep -Seconds 2
# The console: the game window of the MW4 started with -ctcltype 1.
$conpid = (Get-CimInstance Win32_Process -Filter "Name='MW4.exe'" | Where-Object CommandLine -match "ctcltype 1").ProcessId
$wins = @(python $desk wins "BattleTech Firestorm" | ForEach-Object { , ($_ -split ' ' | ForEach-Object { [int]$_ }) })
$con = $wins | Where-Object { $_[5] -eq $conpid } | Select-Object -First 1
if (-not $con) { throw "the console's window did not appear" }
$ox = $con[1]; $oy = $con[2]
function At([int]$x, [int]$y) { python $desk click ($ox + $x) ($oy + $y) | Out-Null; Start-Sleep -Milliseconds 600 }

python $desk front "BattleTech Firestorm" $ox x | Out-Null
# A pilot seated in the pod (the launcher's Pilots page) is put in slot 1 by
# the console's shim itself ([profiles] fill_console); wait for it rather
# than click over it.
$seats = Join-Path $Game "profiles\assign.ini"
$seated = (Test-Path $seats) -and (Select-String -Path $seats -Pattern '^\s*name\s*=\s*\S' -Quiet)
if ($seated -and -not (WaitFor $logs[1] "profiles: the console's slot" 30)) { $seated = $false }
if (-not $seated) {
  At 181 118                # slot 1's player list
  At 90 159                 # the pod's pilot
  At 120 143                # the name box
  python $desk type $Pilot
  python $desk key enter
  Start-Sleep -Milliseconds 500
}
if (-not $NoBot) {
  At 181 168                # slot 2's player list
  At 90 228                 # a bot
}
$lamps = Lines $logs[2] "RIO from game: 84"
At 672 580                  # LOAD
"loading the pod..."
# Loaded, the pod lights its panel.
if (WaitFor $logs[2] "RIO from game: 84" $LoadWait $lamps) { Start-Sleep -Seconds 2 }
else { throw "the pod did not load within $LoadWait s: see the console's screen, and shim-ctcl1.log / shim-ctcl2.log" }
if ($NoLaunch) { "loaded; LAUNCH is on the console"; return }
At 672 580                  # LAUNCH
Start-Sleep -Seconds 2
$pod = @(python $desk wins "BattleTech Firestorm" | ForEach-Object { , ($_ -split ' ' | ForEach-Object { [int]$_ }) } |
  Where-Object { $_[5] -ne $conpid }) | Select-Object -First 1
if ($pod) { python $desk front "BattleTech Firestorm" $pod[1] x | Out-Null }
"mission launched"
