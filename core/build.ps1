# Build ncx_core (Release, x64) with the CMake bundled in Visual Studio 2019 Build Tools, then run
# the C++ self-test.  Output: core\build\Release\ncx_core.dll
#
#   powershell -ExecutionPolicy Bypass -File core\build.ps1 [-Config Debug] [-NoTest]
param([string]$Config = 'Release', [switch]$NoTest)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
if (-not (Test-Path $cmake)) { throw "CMake not found under $vs" }
$build = Join-Path $here 'build'
& $cmake -S $here -B $build -G 'Visual Studio 16 2019' -A x64 | Out-Host
if ($LASTEXITCODE) { throw 'cmake configure failed' }
& $cmake --build $build --config $Config -- /m /nologo /v:minimal | Out-Host
if ($LASTEXITCODE) { throw 'build failed' }
if (-not $NoTest) {
    & $ctest --test-dir $build -C $Config --output-on-failure | Out-Host
    if ($LASTEXITCODE) { throw 'self-test failed' }
}
Write-Host "OK: $(Join-Path $build "$Config\ncx_core.dll")"
