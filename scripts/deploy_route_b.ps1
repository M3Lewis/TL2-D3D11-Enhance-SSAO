$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$GameDir = "H:\Steam\steamapps\Common\Torchlight II"
$ProgDir = Join-Path $GameDir "programs"
$VanDir  = Join-Path $GameDir "programs_vanilla_reference"
$ShaderSrc = Join-Path $RootDir "shaders"

$proc = Get-Process Torchlight2 -ErrorAction SilentlyContinue
if ($proc) {
    throw "Torchlight2.exe is running (PID $($proc.Id)); close the game before deploying."
}

# 1. Preserve untouched vanilla files before any modification in programs/
$RenderScaleGame = Join-Path $ProgDir "RenderScale.material"
$RenderScaleVan  = Join-Path $VanDir "RenderScale.material"
if ((Test-Path $RenderScaleGame) -and -not (Test-Path $RenderScaleVan)) {
    New-Item -ItemType Directory -Path $VanDir -Force | Out-Null
    Copy-Item -LiteralPath $RenderScaleGame -Destination $RenderScaleVan -Force
    Write-Host "[BACKUP] Saved vanilla RenderScale.material -> $RenderScaleVan" -ForegroundColor Cyan
}

# 2. Deploy built Plugin_TL2_RouteB.dll
$BuiltDll = Join-Path $RootDir "bin\Plugin_TL2_RouteB.dll"
if (-not (Test-Path $BuiltDll)) {
    throw "Missing $BuiltDll; run build.ps1 first."
}
Copy-Item -LiteralPath $BuiltDll -Destination (Join-Path $GameDir "Plugin_TL2_RouteB.dll") -Force
Write-Host "[DEPLOY] Plugin_TL2_RouteB.dll -> $GameDir" -ForegroundColor Green

# 3. Deploy Phase 2+ shader assets & materials
$DeployList = @(
    @{ Src = "Copy_ps.hlsl";              Dst = "Copy_ps.hlsl" },
    @{ Src = "hlsl4\Copy_ps.hlsl";        Dst = "hlsl4\Copy_ps.hlsl" },
    @{ Src = "hlsl4\SimpleUI_vs.hlsl";    Dst = "hlsl4\SimpleUI_vs.hlsl" },
    @{ Src = "hlsl4\SimpleUI_ps.hlsl";    Dst = "hlsl4\SimpleUI_ps.hlsl" },
    @{ Src = "SimpleUI.program";          Dst = "SimpleUI.program" },
    @{ Src = "RenderScale.material";      Dst = "RenderScale.material" }
)

New-Item -ItemType Directory -Path (Join-Path $ProgDir "hlsl4") -Force | Out-Null
foreach ($item in $DeployList) {
    $srcPath = Join-Path $ShaderSrc $item.Src
    if (Test-Path $srcPath) {
        $dstPath = Join-Path $ProgDir $item.Dst
        Copy-Item -LiteralPath $srcPath -Destination $dstPath -Force
        Write-Host "[DEPLOY] $($item.Dst)" -ForegroundColor Green
    }
}
