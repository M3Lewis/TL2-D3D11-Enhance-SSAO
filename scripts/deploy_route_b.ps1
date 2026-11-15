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
$VanillaBackups = @(
    "RenderScale.material",
    "shaders.program",
    "hlsl4\skinning.hlsl",
    "hlsl4\Fxaa_ps.hlsl"
)
New-Item -ItemType Directory -Path (Join-Path $VanDir "hlsl4") -Force | Out-Null
foreach ($rel in $VanillaBackups) {
    $gameFile = Join-Path $ProgDir $rel
    $vanFile  = Join-Path $VanDir $rel
    if ((Test-Path $gameFile) -and -not (Test-Path $vanFile)) {
        Copy-Item -LiteralPath $gameFile -Destination $vanFile -Force
        Write-Host "[BACKUP] Saved vanilla $rel -> $vanFile" -ForegroundColor Cyan
    }
}

# 2. Deploy built Plugin_TL2_DX11_Enhance_SSAO.dll
$BuiltDll = Join-Path $RootDir "bin\Plugin_TL2_DX11_Enhance_SSAO.dll"
if (-not (Test-Path $BuiltDll)) {
    throw "Missing $BuiltDll; run build.ps1 first."
}
Copy-Item -LiteralPath $BuiltDll -Destination (Join-Path $GameDir "Plugin_TL2_DX11_Enhance_SSAO.dll") -Force
Write-Host "[DEPLOY] Plugin_TL2_DX11_Enhance_SSAO.dll -> $GameDir" -ForegroundColor Green

# 3. Deploy Phase 2+ shader assets & materials
$DeployList = @(
    @{ Src = "Copy_ps.hlsl";              Dst = "Copy_ps.hlsl" },
    @{ Src = "hlsl4\Copy_ps.hlsl";        Dst = "hlsl4\Copy_ps.hlsl" },
    @{ Src = "hlsl4\SimpleUI_vs.hlsl";    Dst = "hlsl4\SimpleUI_vs.hlsl" },
    @{ Src = "hlsl4\SimpleUI_ps.hlsl";    Dst = "hlsl4\SimpleUI_ps.hlsl" },
    @{ Src = "SimpleUI.program";          Dst = "SimpleUI.program" },
    @{ Src = "RenderScale.material";      Dst = "RenderScale.material" },
    @{ Src = "shaders.program";           Dst = "shaders.program" },
    @{ Src = "Phase3_Scene.program";      Dst = "Phase3_Scene.program" },
    @{ Src = "hlsl4\DiffPass_ps.hlsl";    Dst = "hlsl4\DiffPass_ps.hlsl" },
    @{ Src = "hlsl4\SkinnedDiff_ps.hlsl"; Dst = "hlsl4\SkinnedDiff_ps.hlsl" },
    @{ Src = "hlsl4\LightPass_ps.hlsl";   Dst = "hlsl4\LightPass_ps.hlsl" },
    @{ Src = "hlsl4\RimLit_ps.hlsl";      Dst = "hlsl4\RimLit_ps.hlsl" },
    @{ Src = "hlsl4\skinning.hlsl";       Dst = "hlsl4\skinning.hlsl" },
    @{ Src = "hlsl4\Fxaa_ps.hlsl";        Dst = "hlsl4\Fxaa_ps.hlsl" }
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
