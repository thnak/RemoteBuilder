param()
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$outDir = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$src = Get-ChildItem (Join-Path $PSScriptRoot "src") -Filter *.cpp | ForEach-Object { $_.FullName }
if ($src.Count -eq 0) { throw "no sources found in agent/src" }
$include = Join-Path $PSScriptRoot "third_party"

function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

$vcvars = $null
if (-not (Have cl)) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vsroot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsroot) { $vcvars = Join-Path $vsroot "VC\Auxiliary\Build\vcvars64.bat" }
  }
}

$flagArgs = @("/nologo", "/std:c++17", "/EHsc", "/O2", "/MT", "/W3",
  "/I", $include, "/Fe:$outDir\rbagent.exe") + $src

if (Have cl) {
  & cl @flagArgs /link ws2_32.lib shell32.lib advapi32.lib
  exit $LASTEXITCODE
}

if ($vcvars -and (Test-Path $vcvars)) {
  $quoted = ($src | ForEach-Object { "`"$_`"" }) -join " "
  cmd /c "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /I `"$include`" /Fe:$outDir\rbagent.exe $quoted /link ws2_32.lib shell32.lib advapi32.lib"
  exit $LASTEXITCODE
}

if (-not (Have g++)) {
  if (Have clang++) {
    & clang++ -std=c++17 -O2 -o "$outDir\rbagent.exe" -I $include $src -lws2_32 -lshell32 -ladvapi32
    exit $LASTEXITCODE
  }
  throw "no C++ compiler found: probed cl, g++, clang++"
}
& g++ -std=c++17 -O2 -o "$outDir\rbagent.exe" -I $include $src -lws2_32 -lshell32 -ladvapi32
exit $LASTEXITCODE
