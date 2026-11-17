# Steam 创意工坊（火炬之光2）后续维护与图片上传指南

- **适用物品 ID**：`3840921717`（AppID: `200710`）
- **工坊页面**：`https://steamcommunity.com/sharedfiles/filedetails/?id=3840921717`
- **已编译工具**：`J:\Software\steam-workshop-uploader\target\release\legacy_upload.exe`
- **一键更新安装包脚本**：
  - `J:\Software\steam-workshop-uploader\upload_tl2_mod.ps1`
  - `J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\scripts\upload_workshop.ps1`

---

## 一、手动在 Steam 网页端上传并排序图片（最推荐）

由于 Steam 网页端轮播图与 `SetItemPreview` / `AddItemPreviewFile` 的展示顺序存在渲染差异及浏览器/CDN 缓存，**直接在网页端管理是最直观、顺序 100% 可控的方式**：

1. 打开工坊页面：`https://steamcommunity.com/sharedfiles/filedetails/?id=3840921717`
2. 在页面右侧 **「所有者控制」** 栏中，点击 **「添加/编辑图片与视频」**。
3. 在管理界面中：
   - 可以删除不想要的旧图/占位图；
   - 点击上传本地图片（单张需 **≤ 1 MB**，支持 JPG / PNG / GIF）；
   - 上传完成后直接用鼠标拖拽调整显示顺序（**第一张即为首页默认展示图**），最后保存。
4. **缓存提示**：保存后若回到详情页发现图片或顺序没立即变化，按 `Ctrl + F5` 强制刷新网页（或等待 1~3 分钟 Steam CDN 同步）。

### 本地 7 张截图路径与时间顺序对照表

截图所在目录：`H:\Steam\userdata\197244315\760\remote\200710\screenshots\`

| 拍摄时间 | 文件名 | 大小 | 画面内容 |
| --- | --- | --- | --- |
| `00:29:22` | `20261117002922_1.jpg` | 370,822 B | 英文面板 - 特效全关（原版基准） |
| `00:29:30` | `20261117002930_1.jpg` | 414,991 B | 英文面板 - 8 项特效全开 (`Full HD+`) |
| `00:29:59` | `20261117002959_1.jpg` | 425,416 B | 英文面板 - `Hob Vibrant` 滤镜 |
| `00:30:01` | `20261117003001_1.jpg` | 415,417 B | 英文面板 - `Dark Film` 滤镜 |
| `00:30:03` | `20261117003003_1.jpg` | 429,738 B | 英文面板 - `Dark Film + Teal_Orange` LUT |
| `00:30:05` | `20261117003005_1.jpg` | 424,589 B | 英文面板 - `Dark Film + Cold Nordic` LUT |
| `00:30:24` | `20261117003024_1.jpg` | 425,790 B | 简体中文面板展示 |

---

## 二、使用命令行工具更新图片 / 文件

> 运行任何命令前，请确保本机 Steam 客户端已启动并登录。

### 1. 用命令行替换图片（仅更新主图，或主图 + 附加图）

```powershell
cd J:\Software\steam-workshop-uploader

# 仅替换第 1 张主封面图（其余图留待网页端手动添加/调整时最干净）
.\target\release\legacy_upload.exe 200710 3840921717 --set-images "图片1路径.jpg"

# 或一次设置主封面图 + 多张附加展示图（第1个参数为主封面图，后续参数为附加图）
.\target\release\legacy_upload.exe 200710 3840921717 --set-images `
  "H:\Steam\userdata\197244315\760\remote\200710\screenshots\20261117002922_1.jpg" `
  "H:\Steam\userdata\197244315\760\remote\200710\screenshots\20261117002930_1.jpg" `
  "H:\Steam\userdata\197244315\760\remote\200710\screenshots\20261117002959_1.jpg"
```

> **注意**：`--set-images` 每次运行都会先清空旧的附加展示图，再重新按顺序写入，不会修改安装包文件、标题、描述与可见性。如果图片大于 1 MB，需先压缩为 ≤ 1 MB 的 JPG/PNG。

### 2. 更新安装包（`TL2_DX11_Enhance_SSAO_Setup.exe`）

重新编译打包 exe 后，运行一键脚本即可：

```powershell
cd J:\Software\steam-workshop-uploader
.\upload_tl2_mod.ps1 -Changelog "本次更新内容说明"
```

或直接调用 `legacy_upload.exe` 上传任意文件：

```powershell
.\target\release\legacy_upload.exe 200710 3840921717 "文件绝对路径" "更新说明"
```

### 3. 查询 Steam 服务器上的实时物品状态

```powershell
.\target\release\legacy_upload.exe 200710 3840921717 --query
```

可以实时回查当前 Steam 后端记录的文件名、文件大小、主封面图 URL 与附加预览图列表（用于区分是“没上传成功”还是“网页浏览器缓存没刷新”）。
