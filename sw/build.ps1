# Build the SolidWorks side: Ncx.Sw.dll (exporter, replayer, SolidWorks geometry host) and the developer
# driver ncxsw.exe. Same toolchain as the AsmFEA add-in: Roslyn csc + .NET Framework 4.8 runtime
# assemblies, SolidWorks interop types EMBEDDED (/link) from the OLDEST supported version (2018, API 26),
# so a call newer than 2018 fails here at compile time, not in a user's SolidWorks.
#
#   powershell -ExecutionPolicy Bypass -File sw\build.ps1 [-SwApi 26]
param([string]$SwApi = '26')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here '..')
$bin = Join-Path $here 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null

$csc = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $csc) { throw 'Roslyn csc.exe not found (install Visual Studio Build Tools)' }
$fw = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$sw = Get-ChildItem "$env:ProgramFiles\SOLIDWORKS Corp" -Directory |
    Where-Object { (Test-Path "$($_.FullName)\SolidWorks.Interop.sldworks.dll") -and
                   ((Get-Item "$($_.FullName)\SolidWorks.Interop.sldworks.dll").VersionInfo.FileVersion -like "$SwApi.*") } |
    Select-Object -First 1
if (-not $sw) { throw "SolidWorks interop assemblies of version $SwApi.x not found" }
$swrefs = @('sldworks', 'swconst') | ForEach-Object { "/link:$($sw.FullName)\SolidWorks.Interop.$_.dll" }
$fwrefs = @('System.dll', 'System.Core.dll', 'Microsoft.CSharp.dll') | ForEach-Object { "/reference:$fw\$_" }
$common = @('/nologo', '/noconfig', '/nostdlib+', "/reference:$fw\mscorlib.dll", '/langversion:7.3', '/platform:x64',
            '/optimize+', '/debug:pdbonly', '/warnaserror+', '/nowarn:1701,1702') + $fwrefs

function Build([string]$out, [string]$target, [string[]]$sources, [string[]]$extra) {
    & $csc @common "/target:$target" "/out:$(Join-Path $bin $out)" @extra @sources
    if ($LASTEXITCODE -ne 0) { throw "build of $out failed" }
    Write-Host "built $out"
}

$lib = @(Get-ChildItem (Join-Path $here 'Ncx.Sw') -Filter *.cs -Recurse | ForEach-Object { $_.FullName }) +
       (Join-Path $root 'core\bindings\csharp\NcxCore.cs')
Build 'Ncx.Sw.dll' 'library' $lib $swrefs
$drv = @(Get-ChildItem (Join-Path $here 'Driver') -Filter *.cs | ForEach-Object { $_.FullName })
Build 'ncxsw.exe' 'exe' $drv ($swrefs + "/reference:$(Join-Path $bin 'Ncx.Sw.dll')")
# the add-in: menu items only (Tools > NCX Translator); ISwAddin comes from swpublished
$addin = @(Get-ChildItem (Join-Path $here 'Ncx.AddIn') -Filter *.cs | ForEach-Object { $_.FullName })
Build 'Ncx.AddIn.dll' 'library' $addin ($swrefs + "/link:$($sw.FullName)\SolidWorks.Interop.swpublished.dll" +
    "/reference:$(Join-Path $bin 'Ncx.Sw.dll')" + "/reference:$fw\System.Windows.Forms.dll" + "/reference:$fw\System.Drawing.dll")
Copy-Item (Join-Path $root 'core\build\Release\ncx_core.dll') $bin -Force
# RegAsm loads the add-in's dependencies to register it: SolidWorks does not put its interop DLLs in the GAC
foreach ($i in 'sldworks', 'swconst', 'swpublished') { Copy-Item "$($sw.FullName)\SolidWorks.Interop.$i.dll" $bin -Force }
