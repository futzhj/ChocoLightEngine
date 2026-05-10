# samples/demo_skinning_perf/setup.ps1
# Phase AW.x: 一键下载 Khronos RiggedSimple.glb 作为默认测试资产
#
# 用法:
#   .\setup.ps1                  # 默认下载 RiggedSimple
#   .\setup.ps1 -Force           # 强制重新下载
#
# 资产来源: KhronosGroup/glTF-Sample-Models (CC0 / Royalty-Free)
# 仓库不预置二进制, .gitignore 已忽略 assets/

param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$assetsDir  = Join-Path $scriptDir 'assets'
$targetFile = Join-Path $assetsDir 'character.glb'

if ((Test-Path $targetFile) -and -not $Force) {
    Write-Host "Asset already exists: $targetFile"
    Write-Host "Use -Force to re-download."
    exit 0
}

if (-not (Test-Path $assetsDir)) {
    New-Item -ItemType Directory -Path $assetsDir | Out-Null
}

$url = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/master/2.0/RiggedSimple/glTF-Binary/RiggedSimple.glb'
Write-Host "Downloading from $url ..."

try {
    Invoke-WebRequest -Uri $url -OutFile $targetFile -UseBasicParsing
} catch {
    Write-Host "Download failed: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "Manually download any glTF 2.0 skinned mesh to:"
    Write-Host "  $targetFile"
    Write-Host ""
    Write-Host "Recommended public assets:"
    Write-Host "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/RiggedSimple"
    Write-Host "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/RiggedFigure"
    Write-Host "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/CesiumMan"
    exit 1
}

$size = (Get-Item $targetFile).Length
Write-Host "Downloaded $size bytes -> $targetFile"
Write-Host ""
Write-Host "Now run:"
Write-Host "  ..\..\Light-0.2.3\windows-x64\light.exe samples\demo_skinning_perf\main.lua"
