# Blender 5.2 LTS NPR Port 文档网站

本仓库保存 Blender 5.2 LTS NPR Port 的中英文 MkDocs 文档源文件。中文站部署在网站根路径，英文站部署在 `/en/`。

- 中文：<https://bb-yi.github.io/blender/>
- English: <https://bb-yi.github.io/blender/en/>
- 正式版本：[Blender 5.2.0 LTS NPR Port](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938)

## 目录

```text
docs/zh/                 中文文档
docs/en/                 英文文档
mkdocs.yml               中文站配置
mkdocs.en.yml            英文站配置
build_multilingual.py    双语严格构建入口
preview-ghpages.ps1      本地 GitHub Pages 路径预览
deploy-to-github.ps1     双语 GitHub Pages 部署入口
```

`site/` 与 `.preview_root/` 都是生成目录，不应提交。

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
powershell -ExecutionPolicy Bypass -File .\preview-ghpages.ps1
```

打开 <http://127.0.0.1:8000/blender/>。

## 部署

先提交并推送 `docs` 分支，再运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\deploy-to-github.ps1
```

部署入口会重新执行完整双语 strict 构建，然后把生成的 `site/` 发布到 `gh-pages`。详细要求见 [DEPLOY.md](DEPLOY.md)。
