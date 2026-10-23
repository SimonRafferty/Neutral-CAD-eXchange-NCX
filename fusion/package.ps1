# Package the NCX Fusion add-in for end users: builds it (unless -NoBuild) and writes
#   fusion\out\NCX-Fusion-<version>.zip
# holding one folder, NCX\, with everything Fusion needs at run time (NCX.manifest, NCX.dll, NcxFusionImpl.dll,
# ncx_core.dll), the add-in's README and the licence. Users unzip it into Fusion's AddIns folder (README.md,
# "Installing from the zip"); no build tools or runtime installers are needed (the DLLs link the C runtime
# statically).
#
#   powershell -ExecutionPolicy Bypass -File fusion\package.ps1 [-Config Release] [-NoBuild]
param([string]$Config = 'Release', [switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $NoBuild) { & (Join-Path $here 'build.ps1') -Config $Config -NoDeploy }
$version = ([regex]::Match((Get-Content -Raw (Join-Path $here 'NCX\NCX.manifest')), '"version"\s*:\s*"([^"]+)"')).Groups[1].Value
if (-not $version) { throw 'no version in NCX\NCX.manifest' }
$build = Join-Path $here "build"

# the add-in folder, exactly as Fusion loads it
$files = [ordered]@{
    'NCX.manifest'      = Join-Path $here 'NCX\NCX.manifest'
    'NCX.dll'           = Join-Path $build "$Config\NCX.dll"
    'NcxFusionImpl.dll' = Join-Path $build "$Config\NcxFusionImpl.dll"
    'ncx_core.dll'      = Join-Path $build "core\$Config\ncx_core.dll"
    'README.md'         = Join-Path $here 'README.md'
    'LICENSE'           = Join-Path $here '..\LICENSE'
}
foreach ($k in $files.Keys) { if (-not (Test-Path $files[$k])) { throw "missing $k ($($files[$k])): build first" } }

$outDir = Join-Path $here 'out'
New-Item -ItemType Directory -Force $outDir | Out-Null
$zip = Join-Path $outDir "NCX-Fusion-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }

# entries written one by one with '/' separators (Compress-Archive in Windows PowerShell 5.1 can write '\')
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$fs = [System.IO.File]::Open($zip, [System.IO.FileMode]::CreateNew)
try {
    $archive = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($k in $files.Keys) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Resolve-Path $files[$k]).Path, "NCX/$k",
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
        # the folder that holds an add-in's resources (command icons), kept if there is one
        $res = Join-Path $here 'NCX\resources'
        if (Test-Path $res) {
            Get-ChildItem -Recurse -File $res | ForEach-Object {
                $rel = $_.FullName.Substring($res.Length + 1).Replace('\', '/')
                [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, "NCX/resources/$rel",
                    [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
            }
        }
    }
    finally { $archive.Dispose() }
}
finally { $fs.Dispose() }
Write-Host "OK: $zip"
