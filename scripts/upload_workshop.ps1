<#
.SYNOPSIS
    通过 Steam Legacy RemoteStorage 接口一键更新 Torchlight II 创意工坊物品 (3840921717)。

.DESCRIPTION
    针对《火炬之光2》（AppID: 200710）这类使用老式 Steam Cloud RemoteStorage 的游戏，
    调用编译好的 legacy_upload.exe 将安装包文件写入 Steam 云存储、共享 UGC 句柄并提交更新。
    不会覆盖你在网页端已编辑好的标题、描述、封面图或可见性设置。

.PARAMETER AppId
    Steam AppID，默认 200710（Torchlight II）。

.PARAMETER ItemId
    Steam 创意工坊物品 ID，默认 3840921717。

.PARAMETER FilePath
    要上传的文件路径，默认指向 TL2_DX11_Enhance_SSAO_Setup.exe。

.PARAMETER Changelog
    可选的更新说明（变动日志）。
#>
[CmdletBinding()]
param(
    [string]$AppId = "200710",
    [string]$ItemId = "3840921717",
    [string]$FilePath = "J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\bin\TL2_DX11_Enhance_SSAO_Setup.exe",
    [string]$Changelog
)

$ErrorActionPreference = "Stop"

$UploaderExe = "J:\Software\steam-workshop-uploader\target\release\legacy_upload.exe"

if (-not (Get-Process steam -ErrorAction SilentlyContinue)) {
    Write-Warning "未检测到正在运行的 Steam 客户端！请先启动并登录 Steam。"
}

if (-not (Test-Path -LiteralPath $UploaderExe)) {
    throw "未找到上传工具: $UploaderExe`n请在 J:\Software\steam-workshop-uploader 运行 cargo build --release --bin legacy_upload"
}

if (-not (Test-Path -LiteralPath $FilePath)) {
    throw "未找到目标文件: $FilePath"
}

$argsList = @($AppId, $ItemId, $FilePath)
if (-not [string]::IsNullOrWhiteSpace($Changelog)) {
    $argsList += $Changelog
}

& $UploaderExe @argsList
if ($LASTEXITCODE -ne 0) {
    throw "上传失败，退出码: $LASTEXITCODE"
}
