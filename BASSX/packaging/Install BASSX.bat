@echo off
rem BASSX - installazione Windows. Doppio click (chiede i permessi di amministratore).
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

set "VST3DIR=%CommonProgramFiles%\VST3"
set "APPDIR=%ProgramFiles%\BASSX"

if not exist "BASSX.vst3" ( echo BASSX.vst3 non trovato accanto a questo file. & pause & exit /b 1 )

echo Installo il VST3 in "%VST3DIR%" ...
if not exist "%VST3DIR%" mkdir "%VST3DIR%"
if exist "%VST3DIR%\BASSX.vst3" rmdir /s /q "%VST3DIR%\BASSX.vst3" 2>nul
if exist "%VST3DIR%\BASSX.vst3" (
  echo.
  echo BASSX e' in uso: chiudi Ableton / la DAW che ha BASSX caricato e rilancia questo file.
  pause & exit /b 1
)
xcopy /e /i /q /y "BASSX.vst3" "%VST3DIR%\BASSX.vst3" >nul || ( echo Copia VST3 fallita. & pause & exit /b 1 )

if exist "BASSX.exe" (
  echo Installo la versione standalone in "%APPDIR%" ...
  if not exist "%APPDIR%" mkdir "%APPDIR%"
  copy /y "BASSX.exe" "%APPDIR%\" >nul
)

echo.
echo Fatto. Riapri la DAW e riscansiona i plug-in.
pause
