# Keeps the DLAA fork on top of the newest bbhost and installs a fresh build
# (docs/dlaa.md). Windows; the build runs in WSL (an Arch Linux distribution,
# tools/ci_windows.sh). update-dlaa.bat at the top of the repository runs it.
#
#   tools/win/update_dlaa.ps1 -InstallDir D:\bbhost         update, build, install
#   tools/win/update_dlaa.ps1 -InstallDir D:\bbhost -Force  build and install even when up to date
#   tools/win/update_dlaa.ps1 -Setup ...                    first run: also installs the
#                                                       build packages in WSL (CI_PACMAN=1)
#
# 1. fetches the 'upstream' remote (the original bbhost; 'origin' when there
#    is no 'upstream');
# 2. rebases the DLAA branch onto upstream/master, after saving it as
#    backup/dlaa-<date>. On a conflict (upstream changed the lines DLAA
#    changes) it aborts and leaves everything as it was: merge that by hand;
# 3. with an 'upstream' remote, pushes upstream's master and the rebased
#    branch (--force-with-lease) to 'origin', your fork;
# 4. builds the Windows package in WSL and copies it to
#    <InstallDir>\bbhost-dlaa-<version>, with ngx_bridge.dll and
#    nvngx_dlss.dll from -DllDir. Older installs are kept.
param(
    # Defaults: beside the repository's folder.
    [string]$InstallDir = (Join-Path $PSScriptRoot '..\..\..\bbhost-installs'),
    [string]$DllDir     = (Join-Path $PSScriptRoot '..\..\..\dlaa-dlls'),
    [string]$Branch     = 'dlaa',
    [string]$WslDistro  = 'archlinux',
    [string]$WslSrc     = '/root/src/bbhost',        # the build's copy of the repository, inside WSL
    [string]$WinDeps    = '/root/win-deps',          # tools/win_deps.sh output, kept between builds
    [string]$WinBuild   = '/root/build-win',
    [switch]$Force,
    [switch]$Setup,
    [switch]$NoPush
)
# Not 'Stop': Windows PowerShell 5.1 turns a native command's stderr (git's
# progress, wsl's "Failed to translate" notes) into errors under it. Native
# commands are checked by $LASTEXITCODE; the cmdlets that matter say -ErrorAction Stop.
$ErrorActionPreference = 'Continue'

# git.exe, not git: a PowerShell function named Git would shadow it (names are
# case-insensitive) and call itself.
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..') -ErrorAction Stop).Path
function Invoke-Git { & git.exe -C $Repo @args; if ($LASTEXITCODE) { throw "git $args failed" } }
function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function To-Wsl($p) { $p = (Resolve-Path $p -ErrorAction Stop).Path; '/mnt/' + $p.Substring(0,1).ToLower() + ($p.Substring(2) -replace '\\','/') }

foreach ($dll in 'ngx_bridge.dll', 'nvngx_dlss.dll') {
    if (-not (Test-Path (Join-Path $DllDir $dll))) { throw "$dll is not in $DllDir (-DllDir; docs/dlaa.md says where to get it)." }
}
$remotes = git.exe -C $Repo remote
$Upstream = if ($remotes -contains 'upstream') { 'upstream' } else { 'origin' }
$push = ($Upstream -eq 'upstream') -and -not $NoPush

Step "Checking the repository"
if (git.exe -C $Repo status --porcelain --untracked-files=no) { throw "The repository has uncommitted changes: commit or stash them first." }
Invoke-Git checkout -q $Branch

Step "Fetching $Upstream"
Invoke-Git fetch -q $Upstream --tags
$base = "$Upstream/master"
$version = (git.exe -C $Repo describe --tags --abbrev=0 $base).Trim()
Write-Host "Upstream is at $version"

& git.exe -C $Repo merge-base --is-ancestor $base HEAD
$upToDate = ($LASTEXITCODE -eq 0)

if (-not $upToDate) {
    Step "Moving the DLAA commits onto $version"
    $backup = "backup/dlaa-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
    Invoke-Git branch $backup
    & git.exe -C $Repo rebase $base
    if ($LASTEXITCODE) {
        & git.exe -C $Repo rebase --abort
        throw "Upstream changed the same lines as DLAA (a rebase conflict). Nothing changed; merge it by hand (git rebase $base)."
    }
    Write-Host "Done. The previous branch is saved as $backup"
}

# Every run, so the fork catches up even when there was nothing to rebase.
if ($push) {
    Step "Pushing to origin (your fork)"
    Invoke-Git push -q origin "${base}:refs/heads/master"
    Invoke-Git push -q --force-with-lease origin "${Branch}:refs/heads/$Branch"
}

if ($upToDate -and -not $Force) {
    Write-Host "`nAlready up to date. (-Force builds and installs anyway.)" -ForegroundColor Green
    exit 0
}

$sha = (git.exe -C $Repo rev-parse --short HEAD).Trim()

Step "Building $sha in WSL ($WslDistro; a few minutes)"
$pacman = if ($Setup) { 'CI_PACMAN=1 ' } else { '' }
$build = @"
set -e
command -v rsync > /dev/null || pacman -S --noconfirm --needed rsync > /dev/null
mkdir -p $WslSrc
rsync -a --delete --exclude build --exclude build-win $(To-Wsl $Repo)/ $WslSrc/
cd $WslSrc
rm -rf build/win/bbhost-win-$sha build/win/bbhost-win-$sha+
${pacman}WIN_DEPS_ROOT=$WinDeps WIN_BUILD=$WinBuild tools/ci_windows.sh > /tmp/bbhost-dlaa-build.log 2>&1 || { tail -30 /tmp/bbhost-dlaa-build.log; exit 1; }
if grep -Eq 'FAILED|build failed' /tmp/bbhost-dlaa-build.log; then grep -E 'error|FAILED' /tmp/bbhost-dlaa-build.log | head -20; exit 1; fi
test -f build/win/bbhost-win-$sha/bbhost.exe
"@ -replace "`r", ''
wsl -d $WslDistro -u root -- bash -lc $build 2>$null
if ($LASTEXITCODE) { throw "The build failed (log: /tmp/bbhost-dlaa-build.log in WSL $WslDistro)." }

Step "Installing"
New-Item -ItemType Directory -Force $InstallDir -ErrorAction Stop | Out-Null
$dest = Join-Path (Resolve-Path $InstallDir -ErrorAction Stop).Path "bbhost-dlaa-$version"
if (Test-Path $dest) { $dest = "$dest-$(Get-Date -Format 'yyyyMMdd-HHmmss')" }
New-Item -ItemType Directory $dest -ErrorAction Stop | Out-Null
wsl -d $WslDistro -u root -- bash -lc "cd $WslSrc/build/win/bbhost-win-$sha && cp -r --no-preserve=ownership . '$(To-Wsl $dest)'/ && rm -rf '$(To-Wsl $dest)'/logs && mkdir '$(To-Wsl $dest)'/logs" 2>$null
if ($LASTEXITCODE) { throw "Copying the build to $dest failed." }
Copy-Item (Join-Path $DllDir 'ngx_bridge.dll'), (Join-Path $DllDir 'nvngx_dlss.dll') $dest -ErrorAction Stop

Write-Host "`nInstalled bbhost $version + DLAA ($sha) in:" -ForegroundColor Green
Write-Host "  $dest"
Write-Host "Start it with run-bbhost.bat there; the settings (%APPDATA%\bbhost) carry over."
