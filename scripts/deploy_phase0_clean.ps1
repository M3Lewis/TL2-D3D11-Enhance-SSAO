$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Split-Path -Parent $ScriptDir
$ModsDir = Split-Path -Parent $ProjectDir
$GameDir = "H:\Steam\steamapps\Common\Torchlight II"

$proc = Get-Process Torchlight2 -ErrorAction SilentlyContinue
if ($proc) {
    throw "Torchlight2.exe is running (PID $($proc.Id)). Please close it before deploying."
}

Write-Host "[Phase 0] Step 1: Reverting programs/ to vanilla reference..." -ForegroundColor Cyan
& (Join-Path $ModsDir "revert_shaders.ps1")

$SrcDll = Join-Path $ProjectDir "bin\Plugin_TL2_DX11_Enhance_SSAO.dll"
if (-not (Test-Path $SrcDll)) {
    throw "Built DLL not found at $SrcDll. Run build.ps1 first."
}

$DstDll = Join-Path $GameDir "Plugin_TL2_DX11_Enhance_SSAO.dll"
Copy-Item -LiteralPath $SrcDll -Destination $DstDll -Force
Write-Host "[Phase 0] Step 2: Deployed $DstDll" -ForegroundColor Green

$CfgPath = Join-Path $GameDir "Plugins_D3D11.cfg"
$CfgBak = Join-Path $GameDir "Plugins_D3D11.cfg.route_b_orig.bak"
if (-not (Test-Path $CfgBak)) {
    Copy-Item -LiteralPath $CfgPath -Destination $CfgBak -Force
    Write-Host "[Phase 0] Backed up $CfgBak" -ForegroundColor Yellow
}

$lines = Get-Content -LiteralPath $CfgPath
if ($lines -notcontains "Plugin=Plugin_TL2_DX11_Enhance_SSAO") {
    $lines += "Plugin=Plugin_TL2_DX11_Enhance_SSAO"
    Set-Content -LiteralPath $CfgPath -Value $lines -Encoding ASCII
    Write-Host "[Phase 0] Step 3: Added Plugin=Plugin_TL2_DX11_Enhance_SSAO to $CfgPath" -ForegroundColor Green
} else {
    Write-Host "[Phase 0] Step 3: Plugin=Plugin_TL2_DX11_Enhance_SSAO already present in $CfgPath" -ForegroundColor Yellow
}
