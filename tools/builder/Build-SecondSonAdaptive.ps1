# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Second Son Adaptive - one-click Windows x64 builder (Windows PowerShell 5.1 or later).
#
#  1. Checks the tools (Git, CMake, Ninja, Visual Studio C++ with clang-cl, Windows SDK) and the
#     free space; lists anything missing and ASKS before installing it.
#  2. Clones MeWeGo/infamous-pc at the pinned commit with its pinned submodules, applies this
#     package's patches (each SHA-256 checked) and verifies the resulting source tree hash.
#  3. Builds shadPS4.exe with clang-cl + Ninja, builds and runs the optimizer tests, smoke-tests
#     the executable and packages the release folder + zip.
# Everything (sources, build files, temp files, logs, output) stays under the workspace on E:.

[CmdletBinding()]
param(
    [string]$Workspace = 'E:\SecondSonAdaptive',
    [int]$Jobs = 2,
    [switch]$CheckOnly,
    [switch]$NonInteractive,
    [switch]$AllowAnyDrive,   # build servers only; players keep everything on E:
    [switch]$SkipTests,
    [switch]$Clean,
    [int]$MinFreeGB = 25
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$script:Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:Lock = Get-Content -Raw -LiteralPath (Join-Path $script:Here 'source_lock.json') | ConvertFrom-Json

# ---------------------------------------------------------------------------------------------
# Workspace and logging
# ---------------------------------------------------------------------------------------------
function Fail([string]$message) {
    Write-Host ''
    Write-Host "BUILD STOPPED: $message" -ForegroundColor Red
    if ($script:LogFile) { Write-Host "Full log: $script:LogFile" }
    try { Stop-Transcript | Out-Null } catch { }
    exit 1
}

function Step([string]$message) {
    Write-Host ''
    Write-Host "==> $message" -ForegroundColor Cyan
}

$Workspace = [System.IO.Path]::GetFullPath($Workspace)
$drive = [System.IO.Path]::GetPathRoot($Workspace)
if (-not $AllowAnyDrive) {
    if ($drive.ToUpperInvariant() -ne 'E:\') {
        Write-Host "The workspace must be on E: (got $Workspace). This builder will not fall back to C:." -ForegroundColor Red
        exit 1
    }
    if (-not (Test-Path -LiteralPath 'E:\')) {
        Write-Host 'Drive E: was not found. Connect it or pass -Workspace with an E:\ path.' -ForegroundColor Red
        exit 1
    }
}
New-Item -ItemType Directory -Force -Path $Workspace | Out-Null
if ((Get-Item -LiteralPath $Workspace).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    Write-Host "$Workspace is a junction/symlink; use a real folder so nothing lands elsewhere." -ForegroundColor Red
    exit 1
}
$script:Logs  = Join-Path $Workspace 'Logs'
$script:Temp  = Join-Path $Workspace 'Temp'
$script:Src   = Join-Path $Workspace 'Source\infamous-pc'
$script:Build = Join-Path $Workspace 'Build\x64-clang-release'
$script:Deps  = Join-Path $Workspace 'Deps'
$script:Out   = Join-Path $Workspace 'Output'
foreach ($d in @($script:Logs, $script:Temp, $script:Deps, $script:Out)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:LogFile = Join-Path $script:Logs "build-$stamp.log"
Start-Transcript -Path $script:LogFile | Out-Null

# Temporary files of this process and of every tool it starts go to the workspace, not C:.
foreach ($name in @('TEMP', 'TMP', 'TMPDIR')) { Set-Item -Path "Env:$name" -Value $script:Temp }
$env:CCACHE_DIR = Join-Path $script:Deps 'ccache'
$env:GIT_TERMINAL_PROMPT = '0'

Write-Host "Second Son Adaptive builder - workspace $Workspace"
Write-Host "Pinned base: $($script:Lock.base_repo) @ $($script:Lock.base_commit)"

# ---------------------------------------------------------------------------------------------
# Requirements
# ---------------------------------------------------------------------------------------------
function Get-FreeGB([string]$root) {
    $letter = $root.Substring(0, 1)
    $d = Get-PSDrive -Name $letter -ErrorAction SilentlyContinue
    if ($null -eq $d) { return 0 }
    return [math]::Round($d.Free / 1GB, 1)
}

function Find-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Get-VsWhere {
    $p = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $p) { return $p }
    return $null
}

function Find-VisualStudio {
    # Returns @{Path; HasCpp; HasClang; Name} for the newest instance, or $null.
    $vswhere = Get-VsWhere
    if (-not $vswhere) { return $null }
    $all = & $vswhere -products * -prerelease -format json | ConvertFrom-Json
    if (-not $all) { return $null }
    $best = $all | Sort-Object -Property installationVersion -Descending | Select-Object -First 1
    $cpp = & $vswhere -products * -prerelease -path $best.installationPath -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $clang = & $vswhere -products * -prerelease -path $best.installationPath -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
    # A stand-alone LLVM (clang-cl on PATH) works as well as Visual Studio's own clang.
    $hasClang = [bool]$clang -or [bool](Find-Tool 'clang-cl')
    return @{
        Path     = $best.installationPath
        Name     = $best.displayName
        HasCpp   = [bool]$cpp
        HasClang = $hasClang
    }
}

function Test-WindowsSdk {
    $root = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
    if (-not (Test-Path -LiteralPath $root)) { return $false }
    return [bool](Get-ChildItem -LiteralPath $root -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') })
}

function Get-Requirements {
    $vs = Find-VisualStudio
    $r = [ordered]@{
        Git      = Find-Tool 'git'
        CMake    = Find-Tool 'cmake'
        Ninja    = Find-Tool 'ninja'
        VS       = $vs
        WinSdk   = Test-WindowsSdk
        FreeWork = Get-FreeGB $Workspace
        FreeC    = Get-FreeGB 'C:\'
    }
    return $r
}

function Show-Requirements($r) {
    $ok = { param($b) if ($b) { 'OK     ' } else { 'MISSING' } }
    Write-Host ("  Git ................ {0} {1}" -f (& $ok $r.Git), $r.Git)
    Write-Host ("  CMake 3.24+ ........ {0} {1}" -f (& $ok $r.CMake), $r.CMake)
    Write-Host ("  Ninja .............. {0} {1}" -f (& $ok $r.Ninja), $r.Ninja)
    if ($r.VS) {
        Write-Host ("  Visual Studio ...... OK      {0} ({1})" -f $r.VS.Name, $r.VS.Path)
        Write-Host ("  MSVC C++ tools ..... {0}" -f (& $ok $r.VS.HasCpp))
        Write-Host ("  clang-cl ........... {0}" -f (& $ok $r.VS.HasClang))
    } else {
        Write-Host  "  Visual Studio ...... MISSING"
    }
    Write-Host ("  Windows 10/11 SDK .. {0}" -f (& $ok $r.WinSdk))
    Write-Host ("  Free space: workspace drive {0} GB (need {2}), C: {1} GB" -f $r.FreeWork, $r.FreeC, $MinFreeGB)
}

function Get-Missing($r) {
    $m = @()
    if (-not $r.Git) { $m += 'Git' }
    if (-not $r.CMake) { $m += 'CMake' }
    if (-not $r.Ninja) { $m += 'Ninja' }
    if (-not $r.VS) { $m += 'VisualStudio' }
    elseif (-not ($r.VS.HasCpp -and $r.VS.HasClang)) { $m += 'VSComponents' }
    if (-not $r.WinSdk) { $m += 'WindowsSdk' }
    return ,$m
}

function Confirm-Action([string]$question) {
    if ($NonInteractive) { return $false }
    $answer = Read-Host "$question [y/N]"
    return $answer -match '^(y|yes)$'
}

function Install-Missing([string[]]$missing, $r) {
    Write-Host ''
    Write-Host 'To build, these must be installed:' -ForegroundColor Yellow
    foreach ($m in $missing) {
        switch ($m) {
            'Git'          { Write-Host '  - Git (winget Git.Git)' }
            'CMake'        { Write-Host '  - CMake (winget Kitware.CMake)' }
            'Ninja'        { Write-Host '  - Ninja (winget Ninja-build.Ninja)' }
            'VisualStudio' { Write-Host '  - Visual Studio 2022 Build Tools with C++ and clang-cl, installed to E:\SecondSonAdaptive\Tools' }
            'VSComponents' { Write-Host "  - In your existing $($r.VS.Name): 'MSVC C++ x64 tools', 'C++ Clang compiler', 'MSBuild support for LLVM', Windows 11 SDK" }
            'WindowsSdk'   { Write-Host '  - Windows 11 SDK (added with the Visual Studio components)' }
        }
    }
    $vsWork = ($missing -contains 'VisualStudio') -or ($missing -contains 'VSComponents') -or ($missing -contains 'WindowsSdk')
    if ($vsWork) {
        Write-Host ''
        Write-Host 'Note: Visual Studio always puts some shared files and the Windows SDK on C: (about 3-6 GB),' -ForegroundColor Yellow
        Write-Host '      even when Visual Studio itself is on another drive. This cannot be moved.' -ForegroundColor Yellow
        if ($r.FreeC -lt 10) {
            Write-Host ''
            Write-Host ("C: has only {0} GB free; the Visual Studio installer needs about 10 GB there." -f $r.FreeC) -ForegroundColor Red
            Write-Host 'No installer was started. Free space on C: (Disk Cleanup -> Clean up system files, empty the'
            Write-Host 'Recycle Bin, move Downloads to E:) and run this again - or use the ready-made build from'
            Write-Host 'GitHub (Releases of your infamous-pc fork), which needs no tools at all.'
            Fail 'not enough space on C: for the Visual Studio installer'
        }
    }
    if (-not (Confirm-Action 'Install the items above now? Windows will ask for administrator permission')) {
        Fail 'required tools are missing and installation was not approved (nothing was installed)'
    }
    $winget = Find-Tool 'winget'
    $toolRoot = Join-Path $Workspace 'Tools'
    New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null
    foreach ($m in $missing) {
        if ($m -in @('Git', 'CMake', 'Ninja')) {
            if (-not $winget) { Fail "winget is not available; install $m by hand and run this again" }
            $id = @{ Git = 'Git.Git'; CMake = 'Kitware.CMake'; Ninja = 'Ninja-build.Ninja' }[$m]
            Step "Installing $m ($id)"
            # --location is honoured only if the package supports it; some may still use C:.
            & $winget install --id $id -e --accept-source-agreements --accept-package-agreements --location (Join-Path $toolRoot $m)
            if ($LASTEXITCODE -ne 0) { Fail "winget could not install $m (exit $LASTEXITCODE)" }
        }
    }
    if ($missing -contains 'VSComponents' -or ($missing -contains 'WindowsSdk' -and $r.VS)) {
        $setup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
        if (-not (Test-Path -LiteralPath $setup)) { Fail "Visual Studio Installer not found at $setup" }
        Step "Adding the C++ and clang-cl components to $($r.VS.Name)"
        # 'modify' on the installed setup.exe: no --wait (only the bootstrapper accepts it; it
        # caused exit code 87 before). Start-Process -Wait waits for the installer itself.
        $vsArgs = @('modify', '--installPath', "`"$($r.VS.Path)`"",
                  '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                  '--add', 'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
                  '--add', 'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset',
                  '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621',
                  '--passive', '--norestart')
        $p = Start-Process -FilePath $setup -ArgumentList $vsArgs -Verb RunAs -Wait -PassThru
        if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {
            Fail "the Visual Studio Installer returned $($p.ExitCode); its log is in %TEMP%\dd_*.log (here: $script:Temp)"
        }
    } elseif ($missing -contains 'VisualStudio') {
        Step 'Installing Visual Studio 2022 Build Tools to E:'
        $boot = Join-Path $script:Temp 'vs_BuildTools.exe'
        Invoke-WebRequest -UseBasicParsing -Uri 'https://aka.ms/vs/17/release/vs_BuildTools.exe' -OutFile $boot
        $btArgs = @('--installPath', "`"$(Join-Path $toolRoot 'VSBuildTools')`"",
                  '--add', 'Microsoft.VisualStudio.Workload.VCTools',
                  '--add', 'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
                  '--add', 'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset',
                  '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621',
                  '--includeRecommended', '--passive', '--norestart', '--wait')
        $p = Start-Process -FilePath $boot -ArgumentList $btArgs -Verb RunAs -Wait -PassThru
        if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { Fail "Build Tools installer returned $($p.ExitCode)" }
    }
    # Tools installed by winget are on PATH only for new processes: refresh ours.
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}

Step 'Checking this PC'
$req = Get-Requirements
Show-Requirements $req
if ($req.FreeWork -lt $MinFreeGB) {
    Fail ("only {0} GB free on {1}; the build needs about {2} GB" -f $req.FreeWork, $drive, $MinFreeGB)
}
$missing = Get-Missing $req
if ($CheckOnly) {
    Write-Host ''
    if ($missing.Count -eq 0) { Write-Host 'Everything needed is installed. Run BUILD_ON_THIS_PC.cmd.' -ForegroundColor Green }
    else { Write-Host ("Missing: {0}. BUILD_ON_THIS_PC.cmd will offer to install them." -f ($missing -join ', ')) -ForegroundColor Yellow }
    Stop-Transcript | Out-Null
    exit 0
}
if ($missing.Count -gt 0) {
    Install-Missing $missing $req
    $req = Get-Requirements
    Show-Requirements $req
    $missing = Get-Missing $req
    if ($missing.Count -gt 0) { Fail ("still missing after installation: {0}" -f ($missing -join ', ')) }
}

# ---------------------------------------------------------------------------------------------
# Pinned sources
# ---------------------------------------------------------------------------------------------
function Invoke-Git([string[]]$a) {
    & git.exe @a
    if ($LASTEXITCODE -ne 0) { Fail "git $($a -join ' ') failed ($LASTEXITCODE)" }
}

Step 'Fetching the pinned sources'
if ($Clean -and (Test-Path -LiteralPath $script:Src)) { Remove-Item -Recurse -Force -LiteralPath $script:Src }
if (-not (Test-Path -LiteralPath (Join-Path $script:Src '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $script:Src) | Out-Null
    Invoke-Git @('clone', '--no-checkout', $script:Lock.base_repo, $script:Src)
}
Push-Location $script:Src
try {
    # LF working tree regardless of the global Git for Windows setting, so the patches apply.
    Invoke-Git @('config', 'core.autocrlf', 'false')
    Invoke-Git @('config', 'core.eol', 'lf')
    Invoke-Git @('fetch', '--tags', 'origin')
    & git.exe cat-file -e "$($script:Lock.base_commit)^{commit}"
    if ($LASTEXITCODE -ne 0) { Fail "the pinned commit $($script:Lock.base_commit) is not in $($script:Lock.base_repo)" }
    Invoke-Git @('-c', 'advice.detachedHead=false', 'checkout', '--force', $script:Lock.base_commit)
    Invoke-Git @('clean', '-fdq')
    # android.googlesource.com is unreachable on some networks: the same pinned commit is fetched
    # from GitHub's mirror then. The commit hash is still checked by git.
    $mirror = @('-c', 'url.https://github.com/mstorsjo/fdk-aac.insteadOf=https://android.googlesource.com/platform/external/aac')
    & git.exe @mirror submodule sync --recursive | Out-Null
    & git.exe @mirror submodule update --init --recursive --force --jobs 4
    if ($LASTEXITCODE -ne 0) { Fail 'downloading the pinned submodules failed (network?) - run again to resume' }
    $bad = & git.exe submodule status --recursive | Where-Object { $_ -match '^[-+U]' }
    if ($bad) { Fail "submodules not at their pinned commits:`n$($bad -join "`n")" }

    Step 'Applying the Second Son Adaptive patches'
    foreach ($patch in $script:Lock.patches) {
        $file = Join-Path $script:Here ("patches\" + $patch.file)
        if (-not (Test-Path -LiteralPath $file)) { Fail "patch missing: $file" }
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant()
        if ($hash -ne $patch.sha256) { Fail "patch $($patch.file) does not match its SHA-256 (corrupt or edited)" }
    }
    Invoke-Git @('-c', 'advice.detachedHead=false', 'checkout', '--force', '-B', 'second-son-adaptive-build', $script:Lock.base_commit)
    $files = $script:Lock.patches | ForEach-Object { Join-Path $script:Here ("patches\" + $_.file) }
    & git.exe -c user.name=builder -c user.email=builder@localhost -c core.autocrlf=false am --keep-cr --whitespace=nowarn @files
    if ($LASTEXITCODE -ne 0) { & git.exe am --abort; Fail 'the patches did not apply to the pinned sources' }
    $tree = (& git.exe rev-parse 'HEAD^{tree}').Trim()
    if ($tree -ne $script:Lock.result_tree) { Fail "patched source tree $tree differs from the expected $($script:Lock.result_tree)" }
    Write-Host "Source verified: tree $tree" -ForegroundColor Green
    $pins = @("base $($script:Lock.base_repo) $($script:Lock.base_commit)", "tree $tree") + (& git.exe submodule status --recursive)
} finally {
    Pop-Location
}

# ---------------------------------------------------------------------------------------------
# Compiler environment
# ---------------------------------------------------------------------------------------------
Step 'Loading the Visual Studio x64 environment'
$vsdev = Join-Path $req.VS.Path 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $vsdev)) { Fail "VsDevCmd.bat not found in $($req.VS.Path)" }
$envDump = & cmd.exe /s /c "`"$vsdev`" -arch=x64 -host_arch=x64 -no_logo >nul && set"
if ($LASTEXITCODE -ne 0) { Fail 'VsDevCmd.bat failed' }
foreach ($line in $envDump) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("Env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
foreach ($name in @('TEMP', 'TMP', 'TMPDIR')) { Set-Item -Path "Env:$name" -Value $script:Temp }
# Visual Studio's own clang-cl first: it always matches the MSVC STL, while a separately
# installed LLVM can be older than the STL accepts (error STL1000).
$llvm = Join-Path $req.VS.Path 'VC\Tools\Llvm\x64\bin'
if (Test-Path -LiteralPath (Join-Path $llvm 'clang-cl.exe')) {
    $env:Path = "$llvm;$env:Path"
} else {
    Write-Host 'Visual Studio has no bundled clang-cl; using the one on PATH and allowing an STL version mismatch.' -ForegroundColor Yellow
    $env:CXXFLAGS = '/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH'
    $env:CFLAGS = '/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH'
}
if (-not (Find-Tool 'clang-cl')) { Fail 'clang-cl was not found (Visual Studio component "C++ Clang compiler for Windows")' }
& clang-cl --version | Select-Object -First 1

# ---------------------------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------------------------
Step "Configuring (Release, clang-cl, Ninja)"
if ($Clean -and (Test-Path -LiteralPath $script:Build)) { Remove-Item -Recurse -Force -LiteralPath $script:Build }
& cmake --fresh -S $script:Src -B $script:Build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON -DCMAKE_EXE_LINKER_FLAGS=/MAP
if ($LASTEXITCODE -ne 0) { Fail 'CMake configuration failed (see the log above)' }

Step "Building shadPS4.exe with $Jobs parallel jobs (the first build takes 30-90 minutes)"
& cmake --build $script:Build --target shadps4 --parallel $Jobs
if ($LASTEXITCODE -ne 0) { Fail 'compilation failed - the first error is the line starting with FAILED: above' }
$exe = Join-Path $script:Build 'shadPS4.exe'
if (-not (Test-Path -LiteralPath $exe)) { Fail 'the build finished but shadPS4.exe is missing' }

if (-not $SkipTests) {
    Step 'Building and running the optimizer tests'
    $gtest = Join-Path $script:Deps 'googletest-1.17.0'
    if (-not (Test-Path -LiteralPath (Join-Path $gtest 'CMakeLists.txt'))) {
        Invoke-Git @('clone', '--depth', '1', '--branch', 'v1.17.0', 'https://github.com/google/googletest.git', $gtest)
    }
    $tb = Join-Path $Workspace 'Build\adaptive-tests'
    & cmake --fresh -S (Join-Path $script:Src 'tests\adaptive') -B $tb -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl "-DGTEST_SOURCE_DIR=$gtest"
    if ($LASTEXITCODE -ne 0) { Fail 'configuring the tests failed' }
    & cmake --build $tb --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { Fail 'building the tests failed' }
    & (Join-Path $tb 'adaptive_tests.exe') --gtest_brief=1
    if ($LASTEXITCODE -ne 0) { Fail 'optimizer tests FAILED' }
}

Step 'Smoke test'
$p = Start-Process -FilePath $exe -ArgumentList '--help' -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $script:Temp 'help.txt')
if (-not $p.WaitForExit(60000)) { $p.Kill(); Fail 'shadPS4.exe --help did not return' }
Write-Host 'shadPS4.exe starts and answers --help.'

# ---------------------------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------------------------
Step 'Packaging'
$pkg = Join-Path $script:Out "SecondSonAdaptive-$stamp"
New-Item -ItemType Directory -Force -Path $pkg | Out-Null
Copy-Item -LiteralPath $exe -Destination $pkg
Copy-Item -Path (Join-Path $script:Src 'dist\second-son-adaptive\*') -Destination $pkg -Recurse -Force
Copy-Item -LiteralPath (Join-Path $script:Src 'LICENSE') -Destination (Join-Path $pkg 'LICENSE.txt')
$pins | Out-File -Encoding ascii (Join-Path $pkg 'SOURCE_PINS.txt')
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $pkg 'shadPS4.exe')).Hash
"shadPS4.exe SHA256 $sha" | Out-File -Encoding ascii (Join-Path $pkg 'SHA256.txt')
New-Item -ItemType Directory -Force -Path (Join-Path $pkg 'user') | Out-Null
Compress-Archive -Path (Join-Path $pkg '*') -DestinationPath "$pkg.zip" -Force

Write-Host ''
Write-Host 'BUILD SUCCEEDED' -ForegroundColor Green
Write-Host "  Ready to play: $pkg\PLAY_SECOND_SON.cmd"
Write-Host "  Zip:           $pkg.zip"
Write-Host "  shadPS4.exe SHA256 $sha"
Write-Host "  Log:           $script:LogFile"
Stop-Transcript | Out-Null
exit 0
