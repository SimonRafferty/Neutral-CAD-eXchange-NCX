# Compile and run the C# P/Invoke smoke test against the built core (core\build.ps1 first).
# Same toolchain as the AsmFEA add-in: Roslyn csc + the .NET Framework 4.8 runtime assemblies, x64.
#
#   powershell -ExecutionPolicy Bypass -File core\bindings\csharp\build_smoke.ps1
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here '..\..\..')
$csc = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $csc) { throw 'Roslyn csc.exe not found' }
$fw = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$out = Join-Path $here 'smoke\bin'
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $root 'core\build\Release\ncx_core.dll') $out -Force
& $csc /nologo /target:exe /platform:x64 /optimize+ /out:"$out\NcxSmoke.exe" `
    /reference:"$fw\mscorlib.dll" /reference:"$fw\System.dll" /nostdlib+ `
    (Join-Path $here 'NcxCore.cs') (Join-Path $here 'smoke\Program.cs')
if ($LASTEXITCODE) { throw 'csc failed' }
& "$out\NcxSmoke.exe" (Join-Path $root 'schema\examples\frame.step')
if ($LASTEXITCODE) { throw 'smoke test failed' }
