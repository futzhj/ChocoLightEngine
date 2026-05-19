param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$sampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $sampleDir '..\..')).Path
$assetsDir = Join-Path $sampleDir 'assets'
$runtimeDir = Join-Path $sampleDir 'runtime'
$sourceAssets = @()
$sourceAssets += Join-Path $repoRoot 'assets\mx_map\1001.map'
$sourceAssets += Join-Path $repoRoot 'assets\mx_map\1002.map'

New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null
foreach ($sourceAsset in $sourceAssets) {
    if (-not (Test-Path $sourceAsset)) {
        throw "Missing source asset: $sourceAsset"
    }
    $targetAsset = Join-Path $assetsDir (Split-Path -Leaf $sourceAsset)
    if ($Force -or -not (Test-Path $targetAsset)) {
        Copy-Item $sourceAsset $targetAsset -Force
    }
}

New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null
$runtimeSource = Join-Path $repoRoot '.ci-runtime\resource-decode-windows'
if (-not (Test-Path (Join-Path $runtimeSource 'light.exe'))) {
    $runtimeSource = Join-Path $repoRoot 'lumen-master\build\src\light\Release'
}

foreach ($name in @('light.exe', 'lightw.exe', 'Light.dll', 'lua51.dll')) {
    $src = Join-Path $runtimeSource $name
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $runtimeDir $name) -Force
    }
}

# 防呆：本地构建出的 Light.dll 必须最后复制，覆盖 CI runtime 的旧 dll，避免 ABI 不一致
$builtLightDll = Join-Path $repoRoot 'ChocoLight\build\bin\Release\Light.dll'
if (Test-Path $builtLightDll) {
    Copy-Item $builtLightDll (Join-Path $runtimeDir 'Light.dll') -Force
}

$lightExe = Join-Path $runtimeDir 'light.exe'
if (-not (Test-Path $lightExe)) {
    throw "Missing runtime light.exe. Download CI artifact template-windows-x64 or build runtime first."
}

Write-Host "Prepared MAP 0.1M sample assets and runtime."
# 输出运行时 Light.dll 的哈希便于排查 ABI 差异
$dllHash = (Get-FileHash (Join-Path $runtimeDir 'Light.dll') -Algorithm SHA256).Hash
Write-Host "runtime\Light.dll SHA256 = $dllHash"
Write-Host "Run: samples\resource_decode_map_01m\run.ps1"
