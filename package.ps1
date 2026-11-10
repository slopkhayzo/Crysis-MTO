<#
.SYNOPSIS
  Builds the Release binaries and packages them as two release archives in dist\, laid out like the game folder
  (Bin32, Bin64), each with a .sha256 file beside it:
    Crysis-MTO-v<version>-plugin-only.zip: optiperf.asi and optiperf.ini for both games, README.txt, LICENSE.txt,
      THIRD-PARTY-NOTICES.txt (the libraries linked into the plugin) and SHA256SUMS.txt (the payload's hashes), for
      a game that already has an ASI loader;
    Crysis-MTO-v<version>-with-Ultimate-ASI-Loader-<loader version>.zip: the same plus the pinned loader (as
      version.dll in both folders) and its notice.
  Existing release archives are never overwritten.

  Ultimate ASI Loader is downloaded from its GitHub release at a pinned version and checked against SHA-256 digests
  (the release's zips and the DLLs taken from them).
  -SkipBuild packages the binaries already in build\release (the ones tested, when only the packaging changed).
.EXAMPLE
  .\package.ps1 -Version 1.0.0
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$UalVersion = 'v9.7.4',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$ual = @{
    x86 = @{ Asset = 'Ultimate-ASI-Loader-NoPDB.zip'; Sha256 = '14b3a1ad018899571ac9aa01482977f3c6d49e6cba99f552d01c5acacd1315e1'
             Dll = 'ec2f4824eca58dd40f425756a4a7cec77b8e381f21d11d7c846ec4b339b617ab'; Bin = 'Bin32' }
    x64 = @{ Asset = 'Ultimate-ASI-Loader-NoPDB_x64.zip'; Sha256 = 'e5860e7d9a1805267535b65749575b5e406cc6ea3325c7392189c578815045d1'
             Dll = '031a3e5576d91dce1e438d36b9a3d462c7334ab4791990a8ff1e3ddc0e132daf'; Bin = 'Bin64' }
}

if ($SkipBuild) {
    foreach ($a in 'x86', 'x64') {
        $asi = Join-Path $root "build\release\$a\optiperf.asi"
        if (-not (Test-Path $asi)) { throw "-SkipBuild: $asi is missing; run without it" }
    }
} else {
    & (Join-Path $root 'build.ps1') -Arch x86, x64 -Config Release -BuildDir (Join-Path $root 'build\release')
}

$dist = Join-Path $root 'dist'
$staging = Join-Path $root 'build\release-staging'
$cache = Join-Path $root 'build\downloads'
New-Item -ItemType Directory -Force $dist, $staging, $cache | Out-Null

# the loader: each architecture's DLL from the pinned release zip, checked
$loaderDll = @{}
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($a in 'x86', 'x64') {
    $u = $ual[$a]
    $zip = Join-Path $cache "$UalVersion-$($u.Asset)"
    if (-not (Test-Path $zip)) {
        Invoke-WebRequest -UseBasicParsing -OutFile $zip "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/$UalVersion/$($u.Asset)"
    }
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $u.Sha256) { Remove-Item $zip; throw "SHA-256 mismatch for $($u.Asset): $hash" }
    $dll = Join-Path $staging "ual-$a-dinput8.dll"
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try { [IO.Compression.ZipFileExtensions]::ExtractToFile($z.GetEntry('dinput8.dll'), $dll, $true) } finally { $z.Dispose() }
    $hash = (Get-FileHash $dll -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $u.Dll) { throw "SHA-256 mismatch for $($u.Asset)'s dinput8.dll: $hash" }
    $loaderDll[$a] = $dll
}

# the notices: the loader's (in the archive with it) and those of the libraries linked into optiperf.asi (in both)
$ualLicense = Join-Path $cache "ual-$UalVersion-license.txt"
if (-not (Test-Path $ualLicense)) {
    Invoke-WebRequest -UseBasicParsing -OutFile $ualLicense "https://raw.githubusercontent.com/ThirteenAG/Ultimate-ASI-Loader/$UalVersion/license"
}
$deps = Join-Path $root 'build\release\x86\_deps'
$noticesHead = "Third-party notices`r`n===================`r`n"
$loaderNotice = @(
    "Ultimate ASI Loader $UalVersion (Bin32\version.dll and Bin64\version.dll, in the archive with the loader only)",
    "Project: https://github.com/ThirteenAG/Ultimate-ASI-Loader",
    "Release: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/$UalVersion", "",
    "Bundled x86 Bin32\version.dll (the release's dinput8.dll) SHA-256:", $ual.x86.Dll.ToUpper(),
    "Bundled x64 Bin64\version.dll (the release's dinput8.dll) SHA-256:", $ual.x64.Dll.ToUpper(), "",
    (Get-Content $ualLicense -Raw).Trim(), ""
) -join "`r`n"
$libraryNotices = @(
    "mimalloc v2.5.2 (statically linked into optiperf.asi)",
    "Project: https://github.com/microsoft/mimalloc", "",
    (Get-Content (Join-Path $deps 'mimalloc-src\LICENSE') -Raw).Trim(), "", "",
    "MinHook v1.3.4 (statically linked into optiperf.asi)",
    "Project: https://github.com/TsudaKageyu/minhook", "",
    (Get-Content (Join-Path $deps 'minhook-src\LICENSE.txt') -Raw).Trim(), ""
) -join "`r`n"

# a text file for Windows: CRLF line ends, one at the end
function Write-Text([string]$Path, [string]$Text, [string]$Encoding) {
    $t = $Text.Replace("`r`n", "`n").TrimEnd("`n").Replace("`n", "`r`n") + "`r`n"
    Set-Content -LiteralPath $Path -Value $t -Encoding $Encoding -NoNewline
}

function New-ReleaseArchive([string]$Name, [string]$Stage, [bool]$IncludeLoader) {
    $archive = Join-Path $dist "$Name.zip"
    if ((Test-Path -LiteralPath $archive) -or (Test-Path -LiteralPath "$archive.sha256")) {
        throw "Release output already exists; refusing to overwrite: $archive"
    }
    if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
    foreach ($a in 'x86', 'x64') {
        $bin = Join-Path $Stage $ual[$a].Bin
        New-Item -ItemType Directory -Force $bin | Out-Null
        $out = Join-Path $root "build\release\$a"
        Copy-Item (Join-Path $out 'optiperf.asi'), (Join-Path $out 'optiperf.ini') $bin
        if ($IncludeLoader) { Copy-Item $loaderDll[$a] (Join-Path $bin 'version.dll') }
    }
    $readme = (Get-Content (Join-Path $root 'src\dist\README.txt') -Raw).Replace('@VERSION@', $Version).Replace('@UAL_VERSION@', $UalVersion)
    Write-Text (Join-Path $Stage 'README.txt') $readme 'UTF8'
    Write-Text (Join-Path $Stage 'LICENSE.txt') (Get-Content (Join-Path $root 'LICENSE') -Raw) 'UTF8'
    $notices = if ($IncludeLoader) { $noticesHead + "`r`n" + $loaderNotice + "`r`n" + $libraryNotices } else { $noticesHead + "`r`n" + $libraryNotices }
    Write-Text (Join-Path $Stage 'THIRD-PARTY-NOTICES.txt') $notices 'UTF8'
    $sums = Get-ChildItem -LiteralPath $Stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($Stage.Length + 1).Replace('\', '/')
    }
    Write-Text (Join-Path $Stage 'SHA256SUMS.txt') ($sums -join "`n") 'Ascii'

    Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $archive
    $zipHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$archive.sha256" -Value ('{0}  {1}' -f $zipHash, "$Name.zip") -Encoding Ascii
    Get-ChildItem $Stage -Recurse -File | ForEach-Object { "{0,-60} {1,10:N0}" -f $_.FullName.Substring($Stage.Length + 1), $_.Length }
    "package: $archive"
    "SHA-256: $zipHash"
}

New-ReleaseArchive -Name "Crysis-MTO-v$Version-plugin-only" -Stage (Join-Path $staging 'plugin-only') -IncludeLoader $false
New-ReleaseArchive -Name "Crysis-MTO-v$Version-with-Ultimate-ASI-Loader-$UalVersion" -Stage (Join-Path $staging 'with-loader') -IncludeLoader $true
