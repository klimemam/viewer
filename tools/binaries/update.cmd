@echo off
setlocal EnableDelayedExpansion

rem   update.cmd                 - the published build, in place (what it always did)
rem   update.cmd --pr 64         - that pull request's build, into try\pr64\
rem   update.cmd --commit a1b2c3 - that commit's build, into try\a1b2c3\
rem
rem The two new forms never touch the published binaries. This branch's history
rem is REPLACED on every publish, so anything unpacked over win64\ would be
rem destroyed by the next update without warning - and a build you are testing
rem is exactly the thing you would not want quietly replaced. They go under
rem try\, and you run them from there.

if "%~1"=="" goto :publish
if /i "%~1"=="--pr" goto :fetch
if /i "%~1"=="--commit" goto :fetch
echo usage: update.cmd [--pr N ^| --commit SHA]
exit /b 2

:publish
git fetch origin binaries && git reset --hard origin/binaries
exit /b %errorlevel%

:fetch
if "%~2"=="" (
  echo %~1 needs a value: update.cmd %~1 ^<value^>
  exit /b 2
)

rem gh is routinely absent from PATH here, and a quoted full path does not
rem survive inside a for /f backquote - so put its DIRECTORY on PATH instead.
where gh >nul 2>&1 || (
  if exist "%ProgramFiles%\GitHub CLI\gh.exe" set "PATH=%ProgramFiles%\GitHub CLI;%PATH%"
  if exist "%LOCALAPPDATA%\GitHubCLIin\gh.exe" set "PATH=%LOCALAPPDATA%\GitHubCLIin;%PATH%"
)
gh --version >nul 2>&1 || (
  echo GitHub CLI not found. Install from https://cli.github.com/ then: gh auth login
  exit /b 1
)

set "REPO=klimemam/viewer"

if /i "%~1"=="--pr" (
  set "TAG=pr%~2"
  rem A PR's build is the run for its HEAD BRANCH, not for the PR number
  for /f "usebackq delims=" %%b in (`gh pr view %~2 --repo %REPO% --json headRefName --jq .headRefName 2^>nul`) do set "REF=%%b"
  if not defined REF (
    echo PR %~2 not found in %REPO%
    exit /b 1
  )
  for /f "usebackq delims=" %%r in (`gh run list --repo %REPO% --branch !REF! --status success --limit 1 --json databaseId --jq ".[0].databaseId" 2^>nul`) do set "RUN=%%r"
) else (
  set "TAG=%~2"
  rem --commit matches the full 40-character sha only, and nobody types those.
  rem Expand through the API rather than git rev-parse: this clone has the
  rem binaries branch, not main's history, so it cannot resolve main's shas.
  for /f "usebackq delims=" %%c in (`gh api repos/%REPO%/commits/%~2 --jq .sha 2^>nul`) do set "FULL=%%c"
  if not defined FULL (
    echo commit %~2 not found in %REPO%
    exit /b 1
  )
  for /f "usebackq delims=" %%r in (`gh run list --repo %REPO% --commit !FULL! --status success --limit 1 --json databaseId --jq ".[0].databaseId" 2^>nul`) do set "RUN=%%r"
)

if not defined RUN (
  echo no SUCCESSFUL build found for %~1 %~2
  echo   a run still going, or one that failed, has no binaries to take
  exit /b 1
)

set "DEST=try\!TAG!"
if exist "!DEST!" rmdir /s /q "!DEST!"
mkdir "!DEST!" 2>nul

echo run !RUN! -^> !DEST!
gh run download !RUN! --repo %REPO% -n viewer-win64 -D "!DEST!" || (
  echo download failed ^(artifacts expire after 90 days^)
  exit /b 1
)

echo.
echo   !DEST!\viewer.exe
echo.
echo The published build in win64\ is untouched. Delete try\ when you are done.
exit /b 0
