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
if /i "%~1"=="--fetch" goto :gitref
if /i "%~1"=="--pr" goto :fetch
if /i "%~1"=="--commit" goto :fetch
echo usage: update.cmd [--fetch REF ^| --pr N ^| --commit SHA]
exit /b 2

:publish
git fetch origin binaries && call :reset_to origin/binaries
exit /b %errorlevel%

:gitref
if "%~2"=="" (
  echo --fetch needs a ref: update.cmd --fetch binaries-pr64
  exit /b 2
)
rem In place, like the plain form - this branch is disposable by design, so
rem there is nothing here worth protecting from being replaced.
git fetch origin "%~2" || (
  echo no such ref: %~2
  echo   CI publishes a branch as binaries-^<branch^>, e.g. binaries-dblclick-probe
  exit /b 1
)
call :reset_to FETCH_HEAD
exit /b %errorlevel%

rem ---------------------------------------------------------------------------
rem :reset_to <ref>  - make the tree be <ref>, unless a running program is
rem holding a file that has to be replaced to get there.
rem
rem Windows maps a running .exe (and every DLL it has loaded) and refuses to
rem unlink or overwrite it, so an update run while the viewer is open dies
rem partway through:
rem
rem     error: unable to unlink old 'win64/viewer.exe': Invalid argument
rem     fatal: Could not reset index file to revision '...'
rem
rem By then git has already rewritten the files it reached first, so the folder
rem holds a mixture of the two builds while HEAD still names the old one - and
rem nothing in that message mentions the viewer window that caused it.
rem
rem The check is a WRITE PROBE on the files this reset would actually rewrite
rem (git diff --name-only <ref> - the ref against the working tree, which is
rem exactly what reset --hard touches), and NOT a search for a process called
rem viewer.exe. A name match answers a different question: it fires on a build
rem unpacked under try\, on a copy anywhere else on the disk, and on a viewer
rem belonging to some other checkout entirely - while staying silent when the
rem holder is not a viewer at all (an antivirus pass, a backup agent, a
rem debugger still attached). Opening the file for append is the same operation
rem git has to perform, on the same file, so what passes here is what git can
rem do. Appending nothing writes nothing: size and timestamp are untouched.
rem
rem With nothing to update, nothing is probed and a running viewer is never
rem mentioned - the question only comes up when there are files to replace.
:reset_to
set "BUSY="
rem if the ref is bad this lists nothing; the reset below then says so properly
for /f "usebackq delims=" %%f in (`git diff --name-only %1 2^>nul`) do call :probe "%%f"
if defined BUSY (
  echo.
  echo Close every viewer window ^(and any viewer-serve started from this
  echo folder^), then run update.cmd again. Nothing has been changed.
  exit /b 1
)
git reset --hard %1
exit /b %errorlevel%

:probe
rem a file that is not there yet cannot be held open - and ">>" would create it
if not exist "%~1" exit /b 0
( >>"%~1" (call ) ) 2>nul && exit /b 0
if not defined BUSY (
  echo.
  echo This update has to replace files that another program is holding open,
  echo and Windows does not allow that while the program is running:
)
set "BUSY=1"
echo     %~1
exit /b 0

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
