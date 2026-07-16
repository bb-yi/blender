# GitHub Pages 部署

文档源位于 `docs` 分支，生成站点发布到 `gh-pages` 分支：

- 中文：<https://bb-yi.github.io/blender/>
- English: <https://bb-yi.github.io/blender/en/>

## 前置条件

- 当前目录必须是本仓库，当前分支必须是 `docs`。
- Git 的 `origin` 指向 `bb-yi/blender`，并已具备推送权限。
- Python 环境已安装 `mkdocs` 和 `mkdocs-material`。
- 文档改动已经提交；部署脚本不会替用户提交或推送文档源。

## 发布步骤

先验证并推送文档源：

```powershell
python .\build_multilingual.py
git status --short
git push origin docs
```

工作区干净后部署：

```powershell
powershell -ExecutionPolicy Bypass -File .\deploy-to-github.ps1
```

可指定 `gh-pages` 提交信息：

```powershell
powershell -ExecutionPolicy Bypass -File .\deploy-to-github.ps1 `
  -CommitMessage "Deploy Blender 5.2 LTS NPR documentation"
```

部署入口始终执行以下流程：

1. 检查仓库、`docs` 分支和干净工作区。
2. 运行 `build_multilingual.py`，以 strict 模式重建中文根站与英文 `/en/` 子站。
3. 检查 `site/index.html`、`site/en/index.html` 和两种语言的 `release.html`。
4. 使用 MkDocs 自带的 `ghp-import` 将完整 `site/` 发布到 `origin/gh-pages`。

## 部署后核对

```powershell
Invoke-WebRequest https://bb-yi.github.io/blender/ -UseBasicParsing
Invoke-WebRequest https://bb-yi.github.io/blender/en/ -UseBasicParsing
Invoke-WebRequest https://bb-yi.github.io/blender/release.html -UseBasicParsing
Invoke-WebRequest https://bb-yi.github.io/blender/en/release.html -UseBasicParsing
```

GitHub Pages 可能需要数分钟刷新。核对页面中的 5.2 LTS 标识、三平台下载链接、SHA256、Filter Graph 和 GLSL Function Code 模式说明。

## 本地等路径预览

```powershell
powershell -ExecutionPolicy Bypass -File .\preview-ghpages.ps1
```

该脚本会把双语构建结果复制到被忽略的 `.preview_root/blender/`，并在 <http://127.0.0.1:8000/blender/> 提供预览。
