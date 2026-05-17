# ============================================================================
# Phase G.1.1 - Shared GL Context probe verification wrapper (Windows / PS)
# ============================================================================
# Purpose:
#   Verify AssetLoader Shared GL Context path on dev box or self-hosted GPU CI.
#   Bypass light.exe unstable exit code by grepping stdout key strings.
#
# Exit codes:
#   0  - probe key log found (Shared GL enabled OR fallback to main-thread)
#   1  - key log NOT found (likely startup failure / GL missing / module load)
#   2  - bad user argument
#
# Usage:
#   .\scripts\run_probe_smoke.ps1
#   .\scripts\run_probe_smoke.ps1 -LightExe path\to\light.exe
#
# CI integration sample (self-hosted GPU runner):
#   - name: Phase G.1.1 probe smoke
#     shell: pwsh
#     run: .\scripts\run_probe_smoke.ps1
# ============================================================================

param(
    [string]$LightExe = "lumen-master\build\src\light\Release\light.exe",
    [string]$Script   = "scripts\smoke\asset_loader_async_probe.lua",
    [int]$TimeoutSec  = 10
)

$ErrorActionPreference = "Stop"

$lightExePath = Resolve-Path -ErrorAction SilentlyContinue $LightExe
$scriptPath   = Resolve-Path -ErrorAction SilentlyContinue $Script

if (-not $lightExePath) {
    Write-Host "[FAIL] light.exe not found at: $LightExe"
    exit 2
}
if (-not $scriptPath) {
    Write-Host "[FAIL] probe script not found at: $Script"
    exit 2
}

Write-Host "[INFO] light.exe : $lightExePath"
Write-Host "[INFO] script    : $scriptPath"
Write-Host "[INFO] timeout   : $TimeoutSec sec"

$tmpOut = New-TemporaryFile
$tmpErr = New-TemporaryFile

try {
    $proc = Start-Process -FilePath $lightExePath `
                          -ArgumentList @($scriptPath.Path) `
                          -RedirectStandardOutput $tmpOut.FullName `
                          -RedirectStandardError  $tmpErr.FullName `
                          -PassThru -NoNewWindow

    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Write-Host "[WARN] light.exe timeout, killing"
        try { $proc.Kill() } catch {}
    }

    $allOutput = ""
    if (Test-Path $tmpOut.FullName) { $allOutput += (Get-Content -Raw $tmpOut.FullName) }
    if (Test-Path $tmpErr.FullName) { $allOutput += (Get-Content -Raw $tmpErr.FullName) }

    $sharedOk   = $allOutput -match "AssetLoader: Shared GL Context enabled"
    $fallbackOk = $allOutput -match "AssetLoader: fallback to main-thread upload"

    if ($sharedOk) {
        Write-Host "[PASS] probe enabled path: Shared GL Context worker upload + fence"
        exit 0
    } elseif ($fallbackOk) {
        Write-Host "[PASS] probe fallback path: main-thread upload"
        Write-Host "[INFO] driver / SDL doesn't support shared ctx (mobile / no GPU / GL<3.3)"
        exit 0
    } else {
        Write-Host "[FAIL] probe key log not found in output"
        Write-Host "[INFO] full stdout/stderr below:"
        Write-Host "----- output start -----"
        Write-Host $allOutput
        Write-Host "----- output end -----"
        exit 1
    }
} finally {
    if (Test-Path $tmpOut.FullName) { Remove-Item $tmpOut.FullName -Force -ErrorAction SilentlyContinue }
    if (Test-Path $tmpErr.FullName) { Remove-Item $tmpErr.FullName -Force -ErrorAction SilentlyContinue }
}
