<#
.SYNOPSIS
    Put viewer on the desktop and in the Start menu, so it starts with a
    double-click instead of a command line.

.DESCRIPTION
    Creates .lnk shortcuts pointing at viewer.exe. Nothing is copied, nothing is
    written to the registry, and the executable stays where it is - which is what
    makes this survive `update.cmd` on a binaries-branch clone: the shortcut
    points at a path whose contents get replaced, not at a copy that goes stale.

    Two details are not cosmetic:
      * WindowStyle 7 (minimized) keeps viewer.exe's console from flashing.
        viewer.exe is a console-subsystem binary on purpose (so --bench,
        --help and --remote-selftest still work from a shell) and closes that
        console itself at startup when nobody else is attached to it; starting
        minimized covers the moment before that happens.
      * WorkingDirectory is the exe's folder, so plugins\ is found however the
        shortcut is invoked.

.PARAMETER Exe
    Path to viewer.exe. Found automatically when this script sits next to it
    (binaries branch) or inside a source checkout.

.PARAMETER RemoteHost
    Also make a shortcut that connects to this host on startup, e.g. user@server.
    It gets the same icon; the running window recolors its own icon green while a
    remote session is up.

.PARAMETER RemotePath
    Directory to browse on that host (default ~). A .npy path opens that file.

.EXAMPLE
    .\install_shortcut.ps1
.EXAMPLE
    .\install_shortcut.ps1 -RemoteHost user@calcserver -RemotePath /data/run42
.EXAMPLE
    .\install_shortcut.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$Exe,
    [string]$Name = 'viewer',
    [string]$RemoteHost,
    [string]$RemotePath = '~',
    [switch]$NoDesktop,
    [switch]$NoStartMenu,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Definition

function Find-ViewerExe {
    if ($Exe) {
        if (-not (Test-Path -LiteralPath $Exe)) { throw "no such file: $Exe" }
        return (Resolve-Path -LiteralPath $Exe).Path
    }
    # in the order they are likely to exist: binaries-branch clone first, then a
    # source checkout (this script lives in tools\)
    $candidates = @(
        'viewer.exe',
        'win64\viewer.exe',
        '..\win64\viewer.exe',
        '..\build\Release\viewer.exe',
        '..\build-mingw\viewer.exe',
        '..\build\viewer.exe'
    )
    foreach ($c in $candidates) {
        $p = Join-Path $here $c
        if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path }
    }
    throw "viewer.exe not found near $here - pass -Exe <path\to\viewer.exe>"
}

# ssh://host (home), ssh://host/abs, ssh://host/~/rel - what core/remote.cpp parses.
function Get-RemoteUrl([string]$h, [string]$p) {
    if ([string]::IsNullOrWhiteSpace($p) -or $p -eq '~') { return "ssh://$h" }
    if ($p.StartsWith('/')) { return "ssh://$h$p" }
    return "ssh://$h/~/" + ($p -replace '^~[\\/]', '')
}

$targets = @{}
if (-not $NoDesktop)   { $targets['desktop']    = [Environment]::GetFolderPath('Desktop') }
if (-not $NoStartMenu) { $targets['Start menu'] = [Environment]::GetFolderPath('Programs') }
if ($targets.Count -eq 0) { throw '-NoDesktop and -NoStartMenu together leave nothing to do' }

$remoteName = if ($RemoteHost) { "$Name ($RemoteHost)" } else { $null }

if ($Uninstall) {
    foreach ($t in $targets.GetEnumerator()) {
        foreach ($n in @($Name, $remoteName) | Where-Object { $_ }) {
            $lnk = Join-Path $t.Value "$n.lnk"
            if (Test-Path -LiteralPath $lnk) {
                Remove-Item -LiteralPath $lnk
                Write-Host "removed  $lnk"
            }
        }
    }
    Write-Host 'done. (a shortcut pinned to the taskbar is a separate copy: unpin it there)'
    return
}

$exePath = Find-ViewerExe
$exeDir  = Split-Path -Parent $exePath
Write-Host "viewer:  $exePath"

$shell = New-Object -ComObject WScript.Shell
function New-ViewerShortcut([string]$path, [string]$arguments, [string]$description) {
    $lnk = $shell.CreateShortcut($path)
    $lnk.TargetPath       = $exePath
    $lnk.Arguments        = $arguments
    $lnk.WorkingDirectory = $exeDir
    $lnk.IconLocation     = "$exePath,0"
    $lnk.Description      = $description
    $lnk.WindowStyle      = 7          # minimized: see the note at the top
    $lnk.Save()
    Write-Host "created  $path"
}

foreach ($t in $targets.GetEnumerator()) {
    New-ViewerShortcut (Join-Path $t.Value "$Name.lnk") '' 'viewer - engineering image viewer'
    if ($RemoteHost) {
        $url = Get-RemoteUrl $RemoteHost $RemotePath
        New-ViewerShortcut (Join-Path $t.Value "$remoteName.lnk") "`"$url`"" "viewer - connect to $RemoteHost"
    }
}

Write-Host ''
Write-Host 'Drag a .npy / .raw onto the shortcut to open it, or drop files on the window.'
Write-Host 'To pin: start it, then right-click the taskbar button > Pin to taskbar.'
if (-not $RemoteHost) {
    Write-Host 'For a shortcut that connects to a server on startup:'
    Write-Host '    .\install_shortcut.ps1 -RemoteHost user@server -RemotePath /data/run42'
}
