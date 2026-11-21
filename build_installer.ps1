$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $RootDir "bin"
$ObjDir = Join-Path $RootDir "obj"
$ShaderSrc = Join-Path $RootDir "shaders"
$DllPath = Join-Path $OutDir "Plugin_TL2_DX11_Enhance_SSAO.dll"

if (-not (Test-Path $DllPath)) {
    throw "Missing $DllPath; run build.ps1 first."
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
New-Item -ItemType Directory -Path $ObjDir -Force | Out-Null

# --- HLSL Shader Pre-compilation Validation ---
$fxcCandidates = @(
    "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\fxc.exe",
    "C:\Program Files (x86)\Windows Kits\10\bin\10.0.18362.0\x64\fxc.exe"
)
$fxcPath = $fxcCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($fxcPath) {
    Write-Host "[Validating Shaders] Running fxc pre-compilation check..." -ForegroundColor Cyan
    $testShaders = @(
        @{ File = "shaders\hlsl4\DiffPass_ps.hlsl"; Entry = "main_opaque"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\DiffPass_ps.hlsl"; Entry = "main_alpha"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\SkinnedDiff_ps.hlsl"; Entry = "main_opaque"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\SkinnedDiff_ps.hlsl"; Entry = "main_alpha"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\LightPass_ps.hlsl"; Entry = "main"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\RimLit_ps.hlsl"; Entry = "main_2d"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\RimLit_ps.hlsl"; Entry = "main_cube"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\RimLit_ps.hlsl"; Entry = "main_cube3"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\skinning.hlsl"; Entry = "hardwareSkinning_vp"; Target = "vs_4_0" },
        @{ File = "shaders\hlsl4\SimpleUI_ps.hlsl"; Entry = "main"; Target = "ps_4_0" },
        @{ File = "shaders\hlsl4\SimpleUI_vs.hlsl"; Entry = "main"; Target = "vs_4_0" }
    )
    foreach ($ts in $testShaders) {
        $shaderFullPath = Join-Path $RootDir $ts.File
        $proc = Start-Process -FilePath $fxcPath -ArgumentList "/nologo", "/T", $ts.Target, "/E", $ts.Entry, "`"$shaderFullPath`"" -NoNewWindow -Wait -PassThru
        if ($proc.ExitCode -ne 0) {
            throw "Shader validation failed: $($ts.File)::$($ts.Entry) [$($ts.Target)]"
        }
    }
    Write-Host "[Validating Shaders] All shaders compiled cleanly." -ForegroundColor Green
}

$Payloads = @(
    @{ Id = 101; RelSrc = "bin\Plugin_TL2_DX11_Enhance_SSAO.dll";   Dst = "Plugin_TL2_DX11_Enhance_SSAO.dll";   Overwrite = $false },
    @{ Id = 102; RelSrc = "shaders\Copy_ps.hlsl";                   Dst = "programs\Copy_ps.hlsl";              Overwrite = $false },
    @{ Id = 103; RelSrc = "shaders\hlsl4\Copy_ps.hlsl";             Dst = "programs\hlsl4\Copy_ps.hlsl";        Overwrite = $false },
    @{ Id = 104; RelSrc = "shaders\hlsl4\SimpleUI_vs.hlsl";         Dst = "programs\hlsl4\SimpleUI_vs.hlsl";    Overwrite = $false },
    @{ Id = 105; RelSrc = "shaders\hlsl4\SimpleUI_ps.hlsl";         Dst = "programs\hlsl4\SimpleUI_ps.hlsl";    Overwrite = $false },
    @{ Id = 106; RelSrc = "shaders\SimpleUI.program";               Dst = "programs\SimpleUI.program";          Overwrite = $false },
    @{ Id = 107; RelSrc = "shaders\RenderScale.material";           Dst = "programs\RenderScale.material";      Overwrite = $true  },
    @{ Id = 108; RelSrc = "shaders\shaders.program";                Dst = "programs\shaders.program";           Overwrite = $true  },
    @{ Id = 109; RelSrc = "shaders\Phase3_Scene.program";           Dst = "programs\Phase3_Scene.program";      Overwrite = $false },
    @{ Id = 110; RelSrc = "shaders\hlsl4\DiffPass_ps.hlsl";         Dst = "programs\hlsl4\DiffPass_ps.hlsl";    Overwrite = $false },
    @{ Id = 111; RelSrc = "shaders\hlsl4\SkinnedDiff_ps.hlsl";      Dst = "programs\hlsl4\SkinnedDiff_ps.hlsl"; Overwrite = $false },
    @{ Id = 112; RelSrc = "shaders\hlsl4\LightPass_ps.hlsl";        Dst = "programs\hlsl4\LightPass_ps.hlsl";   Overwrite = $false },
    @{ Id = 113; RelSrc = "shaders\hlsl4\RimLit_ps.hlsl";           Dst = "programs\hlsl4\RimLit_ps.hlsl";      Overwrite = $false },
    @{ Id = 114; RelSrc = "shaders\hlsl4\skinning.hlsl";            Dst = "programs\hlsl4\skinning.hlsl";       Overwrite = $true  },
    @{ Id = 115; RelSrc = "shaders\hlsl4\Fxaa_ps.hlsl";             Dst = "programs\hlsl4\Fxaa_ps.hlsl";        Overwrite = $true  },
    @{ Id = 116; RelSrc = "shaders\hlsl4\BrightBloom2_ps20.hlsl";   Dst = "programs\hlsl4\BrightBloom2_ps20.hlsl"; Overwrite = $true },
    @{ Id = 117; RelSrc = "shaders\hlsl4\Bloom2_ps20.hlsl";         Dst = "programs\hlsl4\Bloom2_ps20.hlsl";  Overwrite = $true  }
)

$ManifestPath = Join-Path $ObjDir "installer_payload.manifest"
$ManifestXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="1.0.0.0" processorArchitecture="amd64" name="TL2.DX11.Enhance.SSAO.Setup" type="win32"/>
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="amd64" publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
</assembly>
"@
[System.IO.File]::WriteAllText($ManifestPath, $ManifestXml, (New-Object System.Text.UTF8Encoding($false)))

$RcLines = New-Object System.Collections.Generic.List[string]
$IncLines = New-Object System.Collections.Generic.List[string]
$IncLines.Add("constexpr PayloadEntry kPayloadEntries[] = {")

$RcLines.Add("1 24 `"obj\\installer_payload.manifest`"")

foreach ($p in $Payloads) {
    $fullSrc = Join-Path $RootDir $p.RelSrc
    if (-not (Test-Path $fullSrc)) {
        throw "Missing payload source file: $fullSrc"
    }
    $rcSrc = ($p.RelSrc) -replace "\\", "\\\\"
    $RcLines.Add("$($p.Id) RCDATA `"$rcSrc`"")

    $cDst = ($p.Dst) -replace "\\", "\\\\"
    $ov = if ($p.Overwrite) { "true" } else { "false" }
    $IncLines.Add("    {$($p.Id), L`"$cDst`", $ov},")
}
$IncLines.Add("};")

$UiAssets = @(
    @{ Id = 201; RelSrc = "installer\assets\tl2_logo.png" },
    @{ Id = 202; RelSrc = "installer\assets\vilderan_map.jpg" },
    @{ Id = 203; RelSrc = "installer\assets\menu_frame.png" },
    @{ Id = 204; RelSrc = "installer\assets\corner_tl.png" },
    @{ Id = 205; RelSrc = "installer\assets\corner_tr.png" },
    @{ Id = 206; RelSrc = "installer\assets\corner_bl.png" },
    @{ Id = 207; RelSrc = "installer\assets\corner_br.png" }
)
foreach ($a in $UiAssets) {
    $fullSrc = Join-Path $RootDir $a.RelSrc
    if (-not (Test-Path $fullSrc)) {
        throw "Missing UI asset file: $fullSrc"
    }
    $rcSrc = ($a.RelSrc) -replace "\\", "\\\\"
    $RcLines.Add("$($a.Id) RCDATA `"$rcSrc`"")
}

$RcPath = Join-Path $ObjDir "installer_payload.rc"
$IncPath = Join-Path $ObjDir "installer_payload_table.inc"
[System.IO.File]::WriteAllLines($RcPath, $RcLines, [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllLines($IncPath, $IncLines, (New-Object System.Text.UTF8Encoding($false)))

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Visual Studio C++ x64 toolchain not found."
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

$ResPath = Join-Path $ObjDir "installer_payload.res"
$InstallerCpp = Join-Path $RootDir "installer\installer_main.cpp"
$SetupExe = Join-Path $OutDir "TL2_DX11_Enhance_SSAO_Setup.exe"

Push-Location $RootDir
try {
    $cmdLine = "call `"$vcvars`" >nul && rc.exe /nologo /fo `"$ResPath`" `"$RcPath`" && cl.exe /nologo /utf-8 /O2 /EHsc /std:c++20 /W4 /WX /MT /I`"$ObjDir`" /Fo`"$ObjDir\\`" `"$InstallerCpp`" `"$ResPath`" /link /SUBSYSTEM:WINDOWS /OUT:`"$SetupExe`" user32.lib gdi32.lib gdiplus.lib shlwapi.lib comctl32.lib shell32.lib ole32.lib advapi32.lib"
    cmd.exe /c $cmdLine
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $SetupExe)) {
        throw "Installer build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Write-Host "[OK] Built standalone installer: $SetupExe ($((Get-Item $SetupExe).Length) bytes)" -ForegroundColor Green
