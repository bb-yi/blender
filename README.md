# Blender 5.2 LTS NPR Port 文档网站

本仓库保存 Blender 5.2 LTS NPR Port 的中英文 MkDocs Material 文档源文件。中文站部署在网站根路径，英文站部署在 `/en/`。

站点包含现代化首页（产品介绍 + 功能入口）与下载发布页（正式包 CTA / SHA256 / 变更摘要）。

- 中文：<https://blendernpr.fun/>
- English: <https://blendernpr.fun/en/>
- 旧地址（仍可用）：<https://bb-yi.github.io/blender/>
- 正式版本：[Blender 5.2.2 LTS NPR Port](https://github.com/bb-yi/blender/releases/tag/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554)

## 目录

```text
docs/zh/                 中文文档（含 stylesheets/extra.css）
docs/en/                 英文文档（含 stylesheets/extra.css）
overrides/               Material 主题覆盖（main.html、语言切换菜单）
mkdocs.yml               中文站配置
mkdocs.en.yml            英文站配置
build_multilingual.py    双语严格构建入口
preview-ghpages.ps1      本地 GitHub Pages 路径预览
deploy-to-github.ps1     双语 GitHub Pages 部署入口
```

`site/` 与 `.preview_root/` 都是生成目录，不应提交。

主题定制要点：

- 顶栏 Tabs：首页 / 下载 / 功能文档
- 共享样式：`docs/*/stylesheets/extra.css`（hero、卡片、下载 CTA）
- 首页与下载页用 HTML + Material cards；功能页保持正文文档布局

## 环境

```powershell
python -m pip install mkdocs mkdocs-material
```

## 构建与预览

构建中文根站和英文子站：

```powershell
python .\build_multilingual.py
```

构建脚本对两套配置都使用 MkDocs strict 模式，任一语言出现警告或错误都会失败。

按线上 `/blender/` 路径预览：

```powershell
pwsh -NoProfile -File .\preview-ghpages.ps1
```

打开 <http://127.0.0.1:8000/blender/>。

预览服务只绑定 `127.0.0.1`。构建失败或缺少必要双语页面时，脚本会立即停止并保留上次预览；`-SkipBuild` 也会检查必要页面。多 Python 环境可通过 `-Python "C:\path\to\python.exe"` 指定已安装 MkDocs 的解释器。

语言菜单会切换到同一文档的另一语言版本，不再回到首页；不同语言的章节锚点可能不同，因此不继承原页面的片段定位。404 页面仍回到相应语言首页。

## 部署

先提交并推送 `docs` 分支，再运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\deploy-to-github.ps1
```

部署入口会重新执行完整双语 strict 构建，然后把生成的 `site/` 发布到 `gh-pages`。详细要求见 [DEPLOY.md](DEPLOY.md)。
