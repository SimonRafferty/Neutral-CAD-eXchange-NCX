# Build the NCX Fusion add-in (and the shared core) with the CMake bundled in Visual Studio (2019 or 2022, Build
# Tools are enough), then deploy it to Fusion's user add-in folder.
#
#   powershell -ExecutionPolicy Bypass -File fusion\build.ps1 [-Config Release] [-NoDeploy] [-Clean]
#
# -Clean deletes the build folder first (a build from scratch). -NoDeploy builds only (fusion\package.ps1 uses it).
#
# Deploying while Fusion runs: NCX.dll (the stub Fusion loaded) is locked and is only replaced when it changed
# and Fusion is closed; NcxFusionImpl.dll and ncx_core.dll are always replaced (Fusion runs private copies),
# and a running add-in picks them up with a {"cmd":"reload"} job (tools\fjob.py reload).
param([string]$Config = 'Release', [switch]$NoDeploy, [switch]$Clean)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# the version is set once, in CMakeLists.txt; the manifest must agree
$cmVersion = ([regex]::Match((Get-Content -Raw (Join-Path $here 'CMakeLists.txt')), 'project\(ncx_fusion VERSION ([0-9.]+)')).Groups[1].Value
# (by pattern: Windows PowerShell 5.1's ConvertFrom-Json refuses the manifest's empty "" description key)
$mfVersion = ([regex]::Match((Get-Content -Raw (Join-Path $here 'NCX\NCX.manifest')), '"version"\s*:\s*"([^"]+)"')).Groups[1].Value
if ($cmVersion -ne $mfVersion) { throw "version mismatch: CMakeLists.txt says $cmVersion, NCX\NCX.manifest says $mfVersion" }

# Visual Studio (any edition with the C++ tools) and the CMake it bundles
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio (2019 or 2022, with the C++ build tools) not found: vswhere.exe is missing' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'no Visual Studio installation with the C++ x64 build tools found' }
$vsMajor = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Split('.')[0]
$generator = @{ '16' = 'Visual Studio 16 2019'; '17' = 'Visual Studio 17 2022' }[$vsMajor]
if (-not $generator) { throw "Visual Studio version $vsMajor is not supported by this script (2019 or 2022)" }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw "CMake not found under $vs (install the 'C++ CMake tools for Windows' component)" }

$build = Join-Path $here 'build'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
& $cmake -S $here -B $build -G $generator -A x64 | Out-Host
if ($LASTEXITCODE) { throw 'cmake configure failed' }
& $cmake --build $build --config $Config --target NCX NcxFusionImpl -- /m /nologo /v:minimal | Out-Host
if ($LASTEXITCODE) { throw 'build failed' }
Write-Host "OK: built the NCX add-in $cmVersion ($generator, $Config)"
if ($NoDeploy) { return }

$dest = Join-Path $env:APPDATA 'Autodesk\Autodesk Fusion 360\API\AddIns\NCX'
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $here 'NCX\NCX.manifest') $dest -Force
if (Test-Path (Join-Path $here 'NCX\resources')) { Copy-Item (Join-Path $here 'NCX\resources') $dest -Recurse -Force }
$out = Join-Path $build $Config
Copy-Item (Join-Path $out 'NcxFusionImpl.dll') $dest -Force
Copy-Item (Join-Path $build "core\$Config\ncx_core.dll") $dest -Force
$stub = Join-Path $out 'NCX.dll'
$deployed = Join-Path $dest 'NCX.dll'
if (-not (Test-Path $deployed) -or (Get-FileHash $stub).Hash -ne (Get-FileHash $deployed).Hash) {
    try { Copy-Item $stub $dest -Force -ErrorAction Stop; Write-Host 'NCX.dll deployed' }
    catch { Write-Warning 'NCX.dll is in use (Fusion is running): it is replaced the next time Fusion is closed and this script rerun' }
}
Write-Host "OK: deployed to $dest"
