# Install (or remove) the SolidWorks add-in: copies the add-in build from sw\bin to an install folder and
# registers it there with RegAsm /codebase, so later dev builds into sw\bin are never locked by a running
# SolidWorks. Needs SolidWorks closed; RegAsm runs elevated (one UAC prompt).
#
#   powershell -ExecutionPolicy Bypass -File sw\install.ps1 [-Uninstall] [-Target <folder>]
param([switch]$Uninstall, [string]$Target = "$env:LOCALAPPDATA\NcxTranslator\addin")
$ErrorActionPreference = 'Stop'
$bin = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'bin'
$regasm = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\RegAsm.exe"
$dll = Join-Path $Target 'Ncx.AddIn.dll'

if (Get-Process sldworks -ErrorAction SilentlyContinue) { throw 'Close every SolidWorks session first' }

function RegAsm([string[]]$regArgs) {
    # elevated: the add-in's register function writes HKLM\SOFTWARE\SolidWorks\Addins
    $log = Join-Path $env:TEMP 'ncx_regasm.log'
    $cmd = "& '$regasm' $($regArgs -join ' ') *> '$log'; exit `$LASTEXITCODE"
    $p = Start-Process powershell -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $cmd
    if (Test-Path $log) { Get-Content $log | Write-Host }
    if ($p.ExitCode -ne 0) { throw "RegAsm failed ($($p.ExitCode))" }
}

if ($Uninstall) {
    if (Test-Path $dll) { RegAsm @('/unregister', "'$dll'") }
    Write-Host "unregistered; files left in $Target"
    return
}

New-Item -ItemType Directory -Force $Target | Out-Null
$files = 'Ncx.AddIn.dll', 'Ncx.AddIn.pdb', 'Ncx.Sw.dll', 'Ncx.Sw.pdb', 'ncx_core.dll',
         'SolidWorks.Interop.sldworks.dll', 'SolidWorks.Interop.swconst.dll', 'SolidWorks.Interop.swpublished.dll'
foreach ($f in $files) { Copy-Item (Join-Path $bin $f) $Target -Force }
RegAsm @('/codebase', "'$dll'")
Write-Host "installed to $Target"
