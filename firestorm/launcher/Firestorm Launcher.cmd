@echo off
rem Firestorm launcher: set this PC up as a pod or the console, and start it.
rem Add  --game "C:\path\to\MW4"  if the game is not in ..\..\firestorm-run\MW4.
where pythonw >nul 2>nul && (start "" pythonw "%~dp0fslauncher.py" %*) || (start "" python "%~dp0fslauncher.py" %*)
