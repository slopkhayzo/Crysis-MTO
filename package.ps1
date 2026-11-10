<#
.SYNOPSIS
  Builds Release binaries and assembles two drop-in packages, laid out like the game folder (Bin32 and Bin64):
    dist\optiperf-<version>\ (+ dist\optiperf-<version>-with-asi-loader.zip): optiperf.asi and optiperf.ini with
      Ultimate ASI Loader (as version.dll) next to them;
    dist\optiperf-<version>-plugin-only\ (+ .zip): the same without the loader, for a game that already has one.

  Ultimate ASI Loader is downloaded from its GitHub release at a pinned version and checked against SHA-256 digests.
  -SkipBuild packages the binaries already in build\release (the ones tested, when only the packaging changed).
.EXAMPLE
  .\package.ps1 -Version 1.0.0
#>
param(
    [string]$Version = '0.2.0',
    [string]$UalVersion = 'v9.7.4',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$ual = @{
    x86 = @{ Asset = 'Ultimate-ASI-Loader-NoPDB.zip'; Sha256 = '14b3a1ad018899571ac9aa01482977f3c6d49e6cba99f552d01c5acacd1315e1'; Bin = 'Bin32' }
    x64 = @{ Asset = 'Ultimate-ASI-Loader-NoPDB_x64.zip'; Sha256 = 'e5860e7d9a1805267535b65749575b5e406cc6ea3325c7392189c578815045d1'; Bin = 'Bin64' }
}

if ($SkipBuild) {
    foreach ($a in 'x86', 'x64') {
        $asi = Join-Path $root "build\release\$a\optiperf.asi"
        if (-not (Test-Path $asi)) { throw "-SkipBuild: $asi is missing; run without it" }
    }
} else {
    & (Join-Path $root 'build.ps1') -Arch x86, x64 -Config Release -BuildDir (Join-Path $root 'build\release')
}

$dist = Join-Path $root "dist\optiperf-$Version"
if (Test-Path $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
$cache = Join-Path $root 'build\downloads'
New-Item -ItemType Directory -Force $cache | Out-Null

foreach ($a in 'x86', 'x64') {
    $u = $ual[$a]
    $zip = Join-Path $cache "$UalVersion-$($u.Asset)"
    if (-not (Test-Path $zip)) {
        Invoke-WebRequest -UseBasicParsing -OutFile $zip "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/$UalVersion/$($u.Asset)"
    }
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $u.Sha256) { Remove-Item $zip; throw "SHA-256 mismatch for $($u.Asset): $hash" }
    $bin = Join-Path $dist $u.Bin
    New-Item -ItemType Directory -Force $bin | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try { [IO.Compression.ZipFileExtensions]::ExtractToFile($z.GetEntry('dinput8.dll'), (Join-Path $bin 'version.dll'), $true) } finally { $z.Dispose() }
    $out = Join-Path $root "build\release\$a"
    Copy-Item (Join-Path $out 'optiperf.asi'), (Join-Path $out 'optiperf.ini') $bin
}

# notices: our README for players and the licenses of what is inside the binaries
Copy-Item (Join-Path $root 'src\dist\README.txt') (Join-Path $dist 'optiperf-README.txt')
$ualLicense = Join-Path $cache "ual-$UalVersion-license.txt"
if (-not (Test-Path $ualLicense)) {
    Invoke-WebRequest -UseBasicParsing -OutFile $ualLicense "https://raw.githubusercontent.com/ThirteenAG/Ultimate-ASI-Loader/$UalVersion/license"
}
$deps = Join-Path $root 'build\release\x86\_deps'
$head = @("Third-party software in this package", "")
$loaderNotice = @(
    "== Ultimate ASI Loader $UalVersion (Bin32\version.dll, Bin64\version.dll) - https://github.com/ThirteenAG/Ultimate-ASI-Loader", (Get-Content $ualLicense -Raw), ""
)
$libNotices = @(
    "== mimalloc (statically linked into optiperf.asi) - https://github.com/microsoft/mimalloc", (Get-Content (Join-Path $deps 'mimalloc-src\LICENSE') -Raw), "",
    "== MinHook (statically linked into optiperf.asi) - https://github.com/TsudaKageyu/minhook", (Get-Content (Join-Path $deps 'minhook-src\LICENSE.txt') -Raw)
)
Set-Content -Path (Join-Path $dist 'optiperf-THIRD-PARTY-NOTICES.txt') -Value (($head + $loaderNotice + $libNotices) -join "`r`n") -Encoding utf8

# the plugin-only package: the same without the loader (and its notice)
$distPlugin = Join-Path $root "dist\optiperf-$Version-plugin-only"
if (Test-Path $distPlugin) { Remove-Item -LiteralPath $distPlugin -Recurse -Force }
Copy-Item -LiteralPath $dist -Destination $distPlugin -Recurse
Get-ChildItem $distPlugin -Recurse -Filter 'version.dll' | Remove-Item
Set-Content -Path (Join-Path $distPlugin 'optiperf-THIRD-PARTY-NOTICES.txt') -Value (($head + $libNotices) -join "`r`n") -Encoding utf8

foreach ($p in @(@{ Dir = $dist; Zip = "optiperf-$Version-with-asi-loader.zip" }, @{ Dir = $distPlugin; Zip = "optiperf-$Version-plugin-only.zip" })) {
    $zipOut = Join-Path $root "dist\$($p.Zip)"
    if (Test-Path $zipOut) { Remove-Item $zipOut }
    Compress-Archive -Path (Join-Path $p.Dir '*') -DestinationPath $zipOut
    Get-ChildItem $p.Dir -Recurse -File | ForEach-Object { "{0,-60} {1,10:N0}" -f $_.FullName.Substring($p.Dir.Length + 1), $_.Length }
    "package: $zipOut"
}
