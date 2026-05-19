$ErrorActionPreference = 'Stop'
$sampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$lightExe = Join-Path $sampleDir 'runtime\light.exe'
$mainLua = Join-Path $sampleDir 'main.lua'

if (-not (Test-Path $lightExe)) {
    & (Join-Path $sampleDir 'setup.ps1')
}

& $lightExe $mainLua
exit $LASTEXITCODE
