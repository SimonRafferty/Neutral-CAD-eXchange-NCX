# Package the NCX Fusion add-in for end users: builds it (unless -NoBuild) and writes the installer
#   fusion\out\NCX-Fusion-Setup-<version>.exe
# from fusion\installer\NCX-Fusion.iss (needs Inno Setup 6: winget install JRSoftware.InnoSetup). It installs the
# add-in folder Fusion loads (NCX.manifest, NCX.dll, NcxFusionImpl.dll, ncx_core.dll, the add-in's README and the
# licence). No build tools or runtime installers are needed on the user's machine (the DLLs link the C runtime
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

# the add-in folder's files, exactly as Fusion loads them (the installer script lists the same)
$files = @(
    (Join-Path $here 'NCX\NCX.manifest'),
    (Join-Path $build "$Config\NCX.dll"),
    (Join-Path $build "$Config\NcxFusionImpl.dll"),
    (Join-Path $build "core\$Config\ncx_core.dll"),
    (Join-Path $here 'README.md'),
    (Join-Path $here '..\LICENSE')
)
foreach ($f in $files) { if (-not (Test-Path $f)) { throw "missing $f`: build first" } }

$outDir = Join-Path $here 'out'
New-Item -ItemType Directory -Force $outDir | Out-Null
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found: winget install JRSoftware.InnoSetup' }
& $iscc /Q "/DAppVersion=$version" "/DConfig=$Config" "/DBuildDir=$build" (Join-Path $here 'installer\NCX-Fusion.iss')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed ($LASTEXITCODE)" }
Write-Host "OK: $(Join-Path $outDir "NCX-Fusion-Setup-$version.exe")"
