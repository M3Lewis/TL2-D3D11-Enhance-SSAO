$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcFiles = @(
    (Join-Path $RootDir "src\hook_utils.cpp"),
    (Join-Path $RootDir "src\dxbc_cache.cpp"),
    (Join-Path $RootDir "src\hlsl_cbuffer_fastpath.cpp"),
    (Join-Path $RootDir "src\cegui_explicit_shader.cpp"),
    (Join-Path $RootDir "src\fixed_func_manual_uniforms.cpp"),
    (Join-Path $RootDir "src\phase3_scene_pipeline.cpp"),
    (Join-Path $RootDir "src\route_c_d3d11.cpp"),
    (Join-Path $RootDir "src\route_c_ao.cpp"),
    (Join-Path $RootDir "src\config_panel.cpp"),
    (Join-Path $RootDir "src\plugin_main.cpp")
)
$OutDir = Join-Path $RootDir "bin"
$ObjDir = Join-Path $RootDir "obj"

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
New-Item -ItemType Directory -Path $ObjDir -Force | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere"
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Visual Studio C++ x64 toolchain not found."
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

$OutDll = Join-Path $OutDir "Plugin_TL2_DX11_Enhance_SSAO.dll"
$OutPdb = Join-Path $OutDir "Plugin_TL2_DX11_Enhance_SSAO.pdb"
$OutImplib = Join-Path $ObjDir "Plugin_TL2_DX11_Enhance_SSAO.lib"
$JoinedSrc = ($SrcFiles | ForEach-Object { "`"$_`"" }) -join " "

$cmdLine = "call `"$vcvars`" >nul && cl.exe /nologo /utf-8 /O2 /EHsc /std:c++20 /W4 /WX /MD /Zi /FS /Fo`"$ObjDir\\`" /Fd`"$OutPdb`" $JoinedSrc /link /INCREMENTAL:NO /DLL /OUT:`"$OutDll`" /IMPLIB:`"$OutImplib`" /DEBUG"

cmd.exe /c $cmdLine
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $OutDll)) {
    throw "MSVC build failed with exit code $LASTEXITCODE"
}

Write-Host "[OK] Built $OutDll ($((Get-Item $OutDll).Length) bytes)" -ForegroundColor Green

$TestNames = @("test_phase1_verify", "test_phase2_verify", "test_phase3_verify", "test_route_c_verify", "test_shader_scripts")
foreach ($t in $TestNames) {
    $testSrc = Join-Path $RootDir "tests\$t.cpp"
    $testExe = Join-Path $ObjDir "$t.exe"
    if (Test-Path $testSrc) {
        $tCmd = "call `"$vcvars`" >nul && cl.exe /nologo /utf-8 /O2 /EHsc /std:c++20 /W4 /WX /MD /Fo`"$ObjDir\\`" `"$testSrc`" /link /OUT:`"$testExe`""
        cmd.exe /c $tCmd
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $testExe)) {
            throw "Test build failed for $t with exit code $LASTEXITCODE"
        }
        Write-Host "[OK] Built $testExe" -ForegroundColor Green
    }
}
