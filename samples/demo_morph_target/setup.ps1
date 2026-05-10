# samples/demo_morph_target/setup.ps1
# Phase AX: 一键下载 Khronos AnimatedMorphCube.glb 作为默认测试资产
#
# 用法:
#   .\setup.ps1                  # 默认下载 AnimatedMorphCube
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
$targetFile = Join-Path $assetsDir 'morph.glb'

if ((Test-Path $targetFile) -and -not $Force) {
    Write-Host "Asset already exists: $targetFile"
    Write-Host "Use -Force to re-download."
    exit 0
}

if (-not (Test-Path $assetsDir)) {
    New-Item -ItemType Directory -Path $assetsDir | Out-Null
}

# AnimatedMorphCube: 8 morph targets (各种基本形状变形)
# 注: 此模型无 skin, 但 ChocoLight Phase AX 仅支持 skin+morph 共存路径,
#    无 skin 时会被 LoadSkinnedGLTF 拒绝. 这里只用于 weights/names API 验证.
# 推荐有 skin 的: AnimatedMorphSphere (虽然名字也叫 morph), 但 Khronos 仓库现成的
# skin+morph 一体资产不多. 用户可自行用 Blender 导出.
$url = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/master/2.0/AnimatedMorphCube/glTF-Binary/AnimatedMorphCube.glb'
Write-Host "Downloading from $url ..."

try {
    Invoke-WebRequest -Uri $url -OutFile $targetFile -UseBasicParsing
} catch {
    Write-Host "Download failed: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "Manually download any glTF 2.0 mesh with morph targets to:"
    Write-Host "  $targetFile"
    Write-Host ""
    Write-Host "Recommended public assets:"
    Write-Host "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/AnimatedMorphCube"
    Write-Host "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/AnimatedMorphSphere"
    Write-Host "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/MorphPrimitivesTest"
    Write-Host ""
    Write-Host "Tip: skin+morph combined assets can be exported from Blender via the glTF exporter."
    exit 1
}

$size = (Get-Item $targetFile).Length
Write-Host "Downloaded $size bytes -> $targetFile"
Write-Host ""
Write-Host "Now run:"
Write-Host "  ..\..\Light-0.2.3\windows-x64\light.exe samples\demo_morph_target\main.lua"
