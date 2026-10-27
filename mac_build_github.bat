@echo off
rem =====================================================================
rem  BASSX - build macOS con GitHub Actions. Doppio click.
rem  1. commit del progetto
rem  2. push sul branch di versione release/v<versione> (non tocca main)
rem  3. GitHub compila Mac (universal) + Windows, test + pluginval
rem  4. se GitHub CLI (gh) e' installato: attende la build e scarica
rem     l'installer in releases\macos\BASSX-v<versione>-github\
rem =====================================================================
setlocal EnableDelayedExpansion
title BASSX - build Mac su GitHub
cd /d "%~dp0"
set /p VER=<"BASSX\VERSION"
set "BRANCH=release/v%VER%"

where git >nul 2>&1 || ( echo git non installato. & pause & exit /b 1 )

rem Disco exFAT: git non vede il proprietario e blocca il repo ("dubious ownership").
set "SAFEDIR=%CD:\=/%"
git config --global --get-all safe.directory | findstr /x /c:"%SAFEDIR%" >nul || git config --global --add safe.directory "%SAFEDIR%"
if not exist ".git" git init -b main
rem Repo GitHub ricavata dal remote "origin" (niente nomi utente scritti negli script)
set "ORIGIN=" & set "REPO="
for /f "delims=" %%u in ('git remote get-url origin 2^>nul') do set "ORIGIN=%%u"
if defined ORIGIN set "REPO=!ORIGIN:https://github.com/=!"
if defined REPO set "REPO=!REPO:.git=!"
if not defined REPO ( echo Nessun remote "origin": collega la repo GitHub. & pause & exit /b 1 )

echo [1/4] Commit v%VER%...
git add -A
git commit -q -m "BASSX v%VER%" || echo       nessuna modifica da committare

echo [2/4] Push su %BRANCH%...
git push -f origin HEAD:refs/heads/%BRANCH%
if errorlevel 1 (
  echo.
  echo  Push fallito su github.com/%REPO%: controlla accesso e connessione.
  pause & exit /b 1
)

where gh >nul 2>&1
if errorlevel 1 (
  echo.
  echo [3/4] Build avviata. Segui e scarica da:
  echo       https://github.com/%REPO%/actions
  echo       artifact "BASSX-macOS-Install" = installer BASSX-%VER%.pkg
  echo       ^(installa GitHub CLI "gh" per scaricarlo in automatico^)
  start "" "https://github.com/%REPO%/actions"
  pause & exit /b 0
)

echo [3/4] Attendo la build su GitHub (15-25 minuti)...
timeout /t 20 /nobreak >nul
set "RUN="
for /f %%i in ('gh run list -R %REPO% --branch %BRANCH% --workflow "BASSX build" -L 1 --json databaseId -q ".[0].databaseId"') do set "RUN=%%i"
if not defined RUN ( echo Build non trovata: controlla https://github.com/%REPO%/actions & pause & exit /b 1 )
gh run watch !RUN! -R %REPO% --exit-status
if errorlevel 1 (
  gh run view !RUN! -R %REPO% --log-failed > "ci_failed_log.txt" 2>&1
  rem Una piattaforma puo' essere riuscita anche se l'altra no: scarica quello che c'e'.
  gh run download !RUN! -R %REPO% -n BASSX-macOS-Install -D "releases\macos\BASSX-v%VER%-github" 2>nul && echo Installer Mac scaricato in releases\macos\BASSX-v%VER%-github
  echo Build FALLITA: log salvato in ci_failed_log.txt  ^(https://github.com/%REPO%/actions/runs/!RUN!^)
  pause & exit /b 1
)

echo [4/4] Scarico gli installer...
set "OUT=releases\macos\BASSX-v%VER%-github"
if exist "%OUT%" rmdir /s /q "%OUT%"
gh run download !RUN! -R %REPO% -n BASSX-macOS-Install -D "%OUT%"
echo.
echo Fatto: %OUT%  (dentro lo zip: BASSX-%VER%.pkg)
start "" "%OUT%"
pause
