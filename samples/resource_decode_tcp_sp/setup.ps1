param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$sampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $sampleDir '..\..')
$assetsDir = Join-Path $sampleDir 'assets'
$runtimeDir = Join-Path $sampleDir 'runtime'
$sourceAsset = Get-ChildItem (Join-Path $repoRoot 'assets') -Filter '*.tcp' -File | Select-Object -First 1
$targetAsset = Join-Path $assetsDir 'sample.tcp'

if (-not $sourceAsset) {
    throw "Missing TCP source asset under: $(Join-Path $repoRoot 'assets')"
}

New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null
if ($Force -or -not (Test-Path $targetAsset)) {
    Copy-Item $sourceAsset.FullName $targetAsset -Force
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

Write-Host "Prepared TCP/SP sample assets and runtime."
Write-Host "Copied TCP asset: $($sourceAsset.FullName) -> $targetAsset"
# 输出运行时 Light.dll 的哈希便于排查 ABI 差异
$dllHash = (Get-FileHash (Join-Path $runtimeDir 'Light.dll') -Algorithm SHA256).Hash
Write-Host "runtime\Light.dll SHA256 = $dllHash"
Write-Host "Run: samples\resource_decode_tcp_sp\run.ps1"
