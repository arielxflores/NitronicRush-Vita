@echo off
rem Nitronic Rush Vita: makes the game data for the Vita from your PC copy of Nitronic Rush.
rem Drag your Nitronic Rush folder (the one with NitronicRush.exe in it) onto this file,
rem or double-click it and paste the folder's path.
setlocal
cd /d "%~dp0"
set "GAME=%~1"
if "%GAME%"=="" set /p "GAME=Drag your Nitronic Rush folder into this window, then press Enter: "
set "GAME=%GAME:"=%"
set "PYTHONHOME="
set "PYTHONPATH="
rem The converter zip from the releases page includes Python (the python folder next to this file).
if exist "%~dp0python\python.exe" (
  "%~dp0python\python.exe" make_vita_data.py "%GAME%" "%~dp0output"
  goto end
)
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY (
  echo.
  echo Python was not found. Use the converter zip from the releases page, which includes it,
  echo or install Python 3 from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^).
  goto end
)
%PY% -c "import numpy, PIL" >nul 2>nul || (
  echo Installing the Python packages it needs ^(numpy, Pillow^)...
  %PY% -m pip install --user --quiet --disable-pip-version-check numpy pillow
)
%PY% make_vita_data.py "%GAME%" "%~dp0output"
:end
echo.
pause
