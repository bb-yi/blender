# GitHub Pages 部署

文档源位于 `docs` 分支，生成站点发布到 `gh-pages` 分支：

- 自定义域（主）：<https://blendernpr.fun/>、<https://blendernpr.fun/en/>
- 旧 GitHub Pages：<https://bb-yi.github.io/blender/>、<https://bb-yi.github.io/blender/en/>

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
4. 使用 MkDocs 自带的 `ghp-import` 将完整 `site/` 发布到 `origin/gh-pages`，并带 `--cname blendernpr.fun`（每次写入根目录 `CNAME`，避免整枝发布冲掉自定义域）。

## 自定义域与 CNAME

- 部署脚本固定写入 `gh-pages` 根目录 `CNAME` → `blendernpr.fun`。
- `ghp-import` 会用 `site/` **整枝替换** `gh-pages`；若不传 `--cname`，仅在 GitHub Settings 里保存过的 CNAME 会在下次部署丢失。
- DNS（Cloudflare）仍需自行配置：`@` / `www` → `bb-yi.github.io`（建议先灰云），并在仓库 **Settings → Pages → Custom domain** 填 `blendernpr.fun`。
- 若将来更换域名：改 `deploy-to-github.ps1` 中的 `$pagesCname`，并同步 DNS 与 GitHub Pages 设置。

## 部署后核对

```powershell
Invoke-WebRequest https://blendernpr.fun/ -UseBasicParsing
Invoke-WebRequest https://blendernpr.fun/en/ -UseBasicParsing
Invoke-WebRequest https://blendernpr.fun/release.html -UseBasicParsing
Invoke-WebRequest https://blendernpr.fun/en/release.html -UseBasicParsing
# 自定义域未生效前可用旧地址：
Invoke-WebRequest https://bb-yi.github.io/blender/ -UseBasicParsing
```

GitHub Pages 可能需要数分钟刷新。核对页面中的 5.2 LTS 标识、下载链接、SHA256、Filter Graph 和 GLSL Function Code 模式说明。自定义域绑好后也可在 `gh-pages` 根目录确认存在内容为 `blendernpr.fun` 的 `CNAME` 文件。

## 本地等路径预览

```powershell
powershell -ExecutionPolicy Bypass -File .\preview-ghpages.ps1
```

该脚本会把双语构建结果复制到被忽略的 `.preview_root/blender/`，并在 <http://127.0.0.1:8000/blender/> 提供预览。
