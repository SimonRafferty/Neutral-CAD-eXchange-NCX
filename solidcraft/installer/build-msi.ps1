<#
  Builds the NCX Translator installer: solidcraft\installer\out\NCX-Translator-<version>.msi

  Needs: Rust (cargo) and the WiX 3 tools (candle.exe, light.exe). WiX is looked for in $env:WIX_BIN, then in
  work\tools\wix3 of this repository, then on PATH. The version comes from solidcraft\Cargo.toml only.

  usage:  powershell -ExecutionPolicy Bypass -File solidcraft\installer\build-msi.ps1 [-NoBuild]
#>
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$crate = Split-Path -Parent $here
$repo = Split-Path -Parent $crate

# the version, from Cargo.toml (the one place it is set)
$cargoToml = [System.IO.File]::ReadAllText((Join-Path $crate 'Cargo.toml'))
if ($cargoToml -notmatch '(?m)^version\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"') { throw 'no version in Cargo.toml' }
$version = $Matches[1]

# WiX
$wix = $env:WIX_BIN
if (-not $wix) { $wix = Join-Path $repo 'work\tools\wix3' }
$candle = Join-Path $wix 'candle.exe'
$light = Join-Path $wix 'light.exe'
if (-not (Test-Path $candle)) {
    $c = Get-Command candle.exe -ErrorAction SilentlyContinue
    if (-not $c) { throw 'WiX 3 (candle.exe) not found: set WIX_BIN' }
    $candle = $c.Source; $light = Join-Path (Split-Path $c.Source) 'light.exe'
}

# a clean release build
if (-not $NoBuild) {
    Push-Location $crate
    try {
        & cargo build --release
        if ($LASTEXITCODE -ne 0) { throw 'cargo build failed' }
    } finally { Pop-Location }
}
$exe = Join-Path $crate 'target\release\ncx-translator.exe'
if (-not (Test-Path $exe)) { throw "no release build at $exe" }

$out = Join-Path $here 'out'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

# the licence page: the repository's LICENSE as a plain RTF (UTF-8 read, ASCII-escaped)
$licence = [System.IO.File]::ReadAllText((Join-Path $repo 'LICENSE'), [System.Text.Encoding]::UTF8)
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append('{\rtf1\ansi\deff0{\fonttbl{\f0 Segoe UI;}}\fs18 ')
foreach ($ch in $licence.ToCharArray()) {
    $code = [int]$ch
    if ($ch -eq '\' -or $ch -eq '{' -or $ch -eq '}') { [void]$sb.Append('\' + $ch) }
    elseif ($ch -eq "`n") { [void]$sb.Append('\par ') }
    elseif ($ch -eq "`r") { }
    elseif ($code -gt 127) { [void]$sb.Append('\u' + $code + '?') }
    else { [void]$sb.Append($ch) }
}
[void]$sb.Append('}')
$rtf = Join-Path $obj 'License.rtf'
[System.IO.File]::WriteAllText($rtf, $sb.ToString(), (New-Object System.Text.ASCIIEncoding))

$msi = Join-Path $out "NCX-Translator-$version.msi"
& $candle -nologo -arch x64 -ext WixUIExtension -ext WixUtilExtension `
    "-dVersion=$version" "-dExePath=$exe" "-dAssets=$(Join-Path $crate 'assets')" "-dRepoRoot=$repo" "-dLicence=$rtf" `
    -out (Join-Path $obj 'ncx-translator.wixobj') (Join-Path $here 'ncx-translator.wxs')
if ($LASTEXITCODE -ne 0) { throw 'candle failed' }
& $light -nologo -ext WixUIExtension -ext WixUtilExtension -spdb `
    -out $msi (Join-Path $obj 'ncx-translator.wixobj')
if ($LASTEXITCODE -ne 0) { throw 'light failed' }
Write-Host "built $msi"
