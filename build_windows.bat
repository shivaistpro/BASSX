@echo off
rem =====================================================================
rem  BASSX - build Windows passo passo: build, test, release versionata,
rem  Git commit + tag + push. Doppio click. Log completo in build_log.txt
rem
rem  Versioning:
rem    BASSX\VERSION        versione (es. 1.1.0), la legge anche CMake
rem    build_number.txt     numero progressivo, +1 a ogni build riuscita
rem    releases\windows\BASSX-v<versione>-b<build>\   una cartella per build
rem    releases\windows\latest\                       copia dell'ultima
rem    BUILDS.md            registro di tutte le build con esito dei test
rem =====================================================================
setlocal EnableDelayedExpansion
title BASSX - build e test
cd /d "%~dp0"
set "ROOT=%CD%"
set "LOG=%ROOT%\build_log.txt"
set "BUILD=%ROOT%\build-win"
set "TESTOUT=%ROOT%\test-output"
set TESTFAIL=0
set "AUDIOTEST=n/d"
set "PVRESULT=n/d"

set /p VER=<"%ROOT%\BASSX\VERSION"
set "BN=0"
if exist "%ROOT%\build_number.txt" set /p BN=<"%ROOT%\build_number.txt"
set /a BN=BN+1
set "TAG=v%VER%"
set "REL=BASSX-v%VER%-b%BN%"
set "OUT=%ROOT%\releases\windows\%REL%"

echo BASSX %REL% - %DATE% %TIME% > "%LOG%"
echo.
echo  ================= BASSX %VER% build %BN% =================
echo.

rem ---------------------------------------------------------------- 1
echo [1/9] Cerco Visual Studio (C++)...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( set "ERR=vswhere non trovato: installa Visual Studio con 'Sviluppo di applicazioni desktop C++'" & goto :fail )
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( set "ERR=Visual Studio senza componente C++" & goto :fail )
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
where cmake >nul 2>&1 || ( set "ERR=cmake non trovato" & goto :fail )
echo       OK  !VSPATH!

rem ---------------------------------------------------------------- 2
echo [2/9] Configuro CMake...
echo === CONFIGURE === >> "%LOG%"
cmake -S "%ROOT%\BASSX" -B "%BUILD%" -A x64 -DBASSX_BUILD_GUI_RENDERER=ON >> "%LOG%" 2>&1
if errorlevel 1 ( set "ERR=configure CMake fallito" & goto :fail )
echo       OK

rem ---------------------------------------------------------------- 3
echo [3/9] Compilo VST3 + Standalone + tool di test (qualche minuto)...
echo === BUILD === >> "%LOG%"
cmake --build "%BUILD%" --config Release --target BASSX_VST3 BASSX_Standalone BassXAudioTest BassXRenderGui -- /m /v:minimal >> "%LOG%" 2>&1
if errorlevel 1 ( set "ERR=compilazione fallita" & goto :fail )
echo       OK

rem ---------------------------------------------------------------- 4
echo [4/9] Test audio: preset, sequencer, arp, latenza, space, neural, allocazioni...
echo === AUDIO TEST === >> "%LOG%"
if not exist "%TESTOUT%" mkdir "%TESTOUT%"
"%BUILD%\BassXAudioTest_artefacts\Release\BassXAudioTest.exe" "%ROOT%\BASSX\tools\neural\reference_io.txt" > "%TESTOUT%\audio_test.txt" 2>&1
if errorlevel 1 ( set "AUDIOTEST=FALLITO" & set TESTFAIL=1 ) else ( set "AUDIOTEST=OK" )
type "%TESTOUT%\audio_test.txt" >> "%LOG%"
for /f "tokens=*" %%l in ('findstr /c:"verifiche" "%TESTOUT%\audio_test.txt"') do echo       %%l
echo       !AUDIOTEST!

rem ---------------------------------------------------------------- 5
echo [5/9] Screenshot delle 11 pagine GUI in test-output...
echo === GUI RENDER === >> "%LOG%"
set GUIFAIL=0
for %%p in (0 1 2 3 4 5 6 7 8 9 10) do (
  "%BUILD%\BassXRenderGui_artefacts\Release\BassXRenderGui.exe" %%p "%TESTOUT%\gui_page%%p.png" >> "%LOG%" 2>&1
  if errorlevel 1 set GUIFAIL=1
  if not exist "%TESTOUT%\gui_page%%p.png" set GUIFAIL=1
)
if !GUIFAIL!==1 ( echo       FALLITO & set TESTFAIL=1 ) else ( echo       OK )

rem ---------------------------------------------------------------- 6
echo [6/9] Preparo la release %REL%...
set "ART=%BUILD%\BASSX_artefacts\Release"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
xcopy /e /i /q /y "%ART%\VST3\BASSX.vst3" "%OUT%\BASSX.vst3" >> "%LOG%" 2>&1
copy /y "%ART%\Standalone\BASSX.exe" "%OUT%\" >> "%LOG%" 2>&1
copy /y "%ROOT%\INSTALLATION README.txt" "%OUT%\" >> "%LOG%" 2>&1
copy /y "%ROOT%\BASSX\packaging\Install BASSX.bat" "%OUT%\" >> "%LOG%" 2>&1
copy /y "%ROOT%\BASSX\CHANGELOG.md" "%OUT%\" >> "%LOG%" 2>&1
copy /y "%TESTOUT%\audio_test.txt" "%OUT%\test_report.txt" >> "%LOG%" 2>&1
if not exist "%OUT%\BASSX.vst3\Contents\x86_64-win\BASSX.vst3" ( set "ERR=VST3 non trovato dopo la build" & goto :fail )
echo       OK  releases\windows\%REL%

rem ---------------------------------------------------------------- 7
echo [7/9] pluginval strictness 10 (alcuni minuti)...
echo === PLUGINVAL === >> "%LOG%"
set "PV="
for /f "delims=" %%f in ('where /r "%ROOT%\..\pluginval" pluginval.exe 2^>nul') do if not defined PV set "PV=%%f"
if not defined PV (
  echo       pluginval.exe non trovato in ..\pluginval & set "PVRESULT=MANCANTE" & set TESTFAIL=1
) else (
  "!PV!" --strictness-level 10 --timeout-ms 300000 --output-dir "%TESTOUT%" --validate "%OUT%\BASSX.vst3" >> "%LOG%" 2>&1
  if errorlevel 1 ( set "PVRESULT=FALLITO" & set TESTFAIL=1 ) else ( set "PVRESULT=OK" )
  echo       !PVRESULT!
)

rem ---------------------------------------------------------------- 8
echo [8/9] Archivio e registro build...
powershell -NoProfile -Command "Compress-Archive -Force -Path '%OUT%\*' -DestinationPath '%ROOT%\releases\windows\%REL%.zip'" >> "%LOG%" 2>&1
if exist "%ROOT%\releases\windows\latest" rmdir /s /q "%ROOT%\releases\windows\latest"
xcopy /e /i /q /y "%OUT%" "%ROOT%\releases\windows\latest" >> "%LOG%" 2>&1
if not exist "%ROOT%\BUILDS.md" (
  echo # BASSX - registro build> "%ROOT%\BUILDS.md"
  echo.>> "%ROOT%\BUILDS.md"
  echo ^| Release ^| Piattaforma ^| Data ^| Test audio ^| pluginval ^|>> "%ROOT%\BUILDS.md"
  echo ^|---^|---^|---^|---^|---^|>> "%ROOT%\BUILDS.md"
)
echo ^| %REL% ^| Windows x64 ^| %DATE% %TIME:~0,5% ^| !AUDIOTEST! ^| !PVRESULT! ^|>> "%ROOT%\BUILDS.md"
> "%ROOT%\build_number.txt" echo %BN%
echo       OK  releases\windows\%REL%.zip

rem ---------------------------------------------------------------- 9
echo [9/9] Git commit + tag %TAG% + push...
echo === GIT === >> "%LOG%"
if !TESTFAIL!==1 (
  echo       SALTATO: ci sono test falliti, niente push. Vedi test-output\ e build_log.txt
  goto :end
)
where git >nul 2>&1 || ( echo       git non installato & goto :end )
rem Niente dati personali o percorsi del PC nei file versionati
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\check_dati_personali.ps1" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo       BLOCCATO: dati personali o percorsi locali nei file - vedi build_log.txt
  goto :end
)
rem Disco exFAT: git non vede il proprietario e blocca il repo ("dubious ownership").
set "SAFEDIR=%CD:\=/%"
git config --global --get-all safe.directory | findstr /x /c:"%SAFEDIR%" >nul || git config --global --add safe.directory "%SAFEDIR%"
rem Repo GitHub ricavata dal remote "origin" (niente nomi utente scritti negli script)
set "ORIGIN=" & set "REPO="
for /f "delims=" %%u in ('git remote get-url origin 2^>nul') do set "ORIGIN=%%u"
if defined ORIGIN set "REPO=!ORIGIN:https://github.com/=!"
if defined REPO set "REPO=!REPO:.git=!"
if not exist "%ROOT%\.git" git init -b main >> "%LOG%" 2>&1
git add -A >> "%LOG%" 2>&1
if exist "%ROOT%\commit_msg.txt" (
  git commit -F "%ROOT%\commit_msg.txt" >> "%LOG%" 2>&1
  del "%ROOT%\commit_msg.txt"
) else (
  git commit -m "Build %REL%: test audio !AUDIOTEST!, pluginval !PVRESULT!" >> "%LOG%" 2>&1
)
git rev-parse -q --verify "refs/tags/%TAG%" >nul 2>&1 || git tag -a "%TAG%" -m "BASSX %VER%" >> "%LOG%" 2>&1
if not defined REPO ( echo       Nessun remote "origin": commit fatto, niente push. & goto :end )
rem "build_windows.bat win": solo Windows. Si pusha solo il branch della build:
rem main e il tag avviano la build Mac su GitHub Actions, il branch build/ no.
if /i "%~1"=="win" goto :pushbranch
git push -u origin main --follow-tags >> "%LOG%" 2>&1
if errorlevel 1 ( echo       push FALLITO - vedi build_log.txt & goto :end )
:pushbranch
rem Un branch per ogni build: build/v<versione>-b<numero>. Per tornarci:
rem   git checkout build/v1.3.1-b3
git push -f origin HEAD:refs/heads/build/%REL:BASSX-=% >> "%LOG%" 2>&1
if errorlevel 1 ( echo       push del branch build/%REL:BASSX-=% FALLITO - vedi build_log.txt ) else ( echo       OK  github.com/%REPO%  tag %TAG%  branch build/%REL:BASSX-=% )
if /i not "%~1"=="win" echo       Mac: il tag %TAG% avvia la build macOS su GitHub Actions ^(mac_build_github.bat la scarica^)
goto :end

:fail
echo       ERRORE: !ERR!
echo === FALLITO: !ERR! === >> "%LOG%"
echo.
echo  ---- ultime righe del log ----
powershell -NoProfile -Command "Get-Content -Tail 30 '%LOG%'"

:end
echo === FINE === >> "%LOG%"
echo.
echo  Fatto. Log completo: build_log.txt
pause
