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
    It gets the green variant of the icon (viewer-remote.ico, generated next to
    the exe) so it is telling you where it starts before you click it - the same
    green the running window paints its own taskbar button while the session is
    up. Without that file it falls back to the exe's own icon.

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

# ssh://host:2222 is a legitimate host spec and ':' is not a legitimate file
# name, so the shortcut is named after a sanitized copy - the url keeps the port.
$remoteName = if ($RemoteHost) { "$Name (" + ($RemoteHost -replace '[\\/:*?"<>|]', '-') + ")" } else { $null }

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

# The green icon for a remote shortcut: shipped next to the exe by CI, or left
# in the build tree by mkicon. Its absence is not an error - the exe's own icon
# is a fine fallback, and the running window recolors itself either way.
$remoteIcon = $null
if ($RemoteHost) {
    foreach ($c in @('viewer-remote.ico', 'icons\viewer-remote.ico',
                     '..\icons\viewer-remote.ico', '..\build\icons\viewer-remote.ico',
                     '..\build-mingw\icons\viewer-remote.ico')) {
        $p = Join-Path $exeDir $c
        if (Test-Path -LiteralPath $p) { $remoteIcon = (Resolve-Path -LiteralPath $p).Path; break }
    }
    if ($remoteIcon) { Write-Host "icon:    $remoteIcon (remote shortcut)" }
    else { Write-Host 'icon:    viewer-remote.ico not found - using the exe icon for both' }
}

$shell = New-Object -ComObject WScript.Shell
function New-ViewerShortcut([string]$path, [string]$arguments, [string]$description, [string]$icon) {
    $lnk = $shell.CreateShortcut($path)
    $lnk.TargetPath       = $exePath
    $lnk.Arguments        = $arguments
    $lnk.WorkingDirectory = $exeDir
    $lnk.IconLocation     = if ($icon) { "$icon,0" } else { "$exePath,0" }
    $lnk.Description      = $description
    $lnk.WindowStyle      = 7          # minimized: see the note at the top
    $lnk.Save()
    Write-Host "created  $path"
}

foreach ($t in $targets.GetEnumerator()) {
    New-ViewerShortcut (Join-Path $t.Value "$Name.lnk") '' 'viewer - engineering image viewer' $null
    if ($RemoteHost) {
        $url = Get-RemoteUrl $RemoteHost $RemotePath
        New-ViewerShortcut (Join-Path $t.Value "$remoteName.lnk") "`"$url`"" `
                           "viewer - connect to $RemoteHost" $remoteIcon
    }
}

Write-Host ''
Write-Host 'Drag a .npy / .raw onto the shortcut to open it, or drop files on the window.'
Write-Host 'To pin: start it, then right-click the taskbar button > Pin to taskbar.'
if (-not $RemoteHost) {
    Write-Host 'For a shortcut that connects to a server on startup:'
    Write-Host '    .\install_shortcut.ps1 -RemoteHost user@server -RemotePath /data/run42'
}
