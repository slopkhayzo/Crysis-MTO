<#
.SYNOPSIS
  Builds optiperf.asi, optiperf_launch.exe and the tests with the installed Visual Studio (MSVC, CMake and Ninja
  from VS). Output: <BuildDir>\<arch>\
.EXAMPLE
  .\build.ps1                          # x86, RelWithDebInfo, into build\x86 (for Bin32\Crysis.exe)
  .\build.ps1 -Arch x86,x64
  .\build.ps1 -Arch x86,x64 -Config Release -BuildDir build\release
#>
param(
    [ValidateSet('x86', 'x64')][string[]]$Arch = @('x86'),
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'RelWithDebInfo',
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x86/x64 tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'

foreach ($a in $Arch) {
    $target = if ($a -eq 'x86') { 'x64_x86' } else { 'x64' }
    $bdir = Join-Path $BuildDir $a
    $src = Join-Path $PSScriptRoot 'src'
    cmd /c "`"$vcvars`" $target >nul 2>&1 && cmake -S `"$src`" -B `"$bdir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config >nul && cmake --build `"$bdir`""
    if ($LASTEXITCODE) { throw "build failed for $a" }
    Write-Host "built $a ($Config) -> $bdir"
}
