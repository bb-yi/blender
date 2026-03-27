# GitHub Pages 部署指南

本文档说明如何将 Blender 5.1 NPR Port 文档网站部署到 GitHub Pages。

## 前置条件

- Git 已安装
- GitHub 账户和仓库访问权限 (https://github.com/bb-yi/blender)
- MkDocs 已安装 (`pip install mkdocs mkdocs-material`)

---

## 部署步骤

### 1️⃣ 初始化 Git 仓库

```powershell
cd e:\blender_bulid_test\blender_npr_bulid\blender-5.1-npr-doc-site

# 初始化 git
git init

# 配置 git 用户信息（如果还没有配置全局）
git config user.email "your-email@example.com"
git config user.name "Your Name"

# 添加远端（bb-yi/blender 仓库）
git remote add origin https://github.com/bb-yi/blender.git

# 验证远端配置
git remote -v
```

### 2️⃣ 添加文件到 Git

```powershell
# 添加所有文件
git add .

# 查看即将提交的文件
git status

# 初始提交
git commit -m "初期提交：Blender 5.1 NPR Port 文档网站"
```

### 3️⃣ 自动部署到 GitHub Pages（推荐）

MkDocs 提供了便捷的 `gh-deploy` 命令，自动生成静态网站并推送到 `gh-pages` 分支：

```powershell
mkdocs gh-deploy
```

✅ 这个命令会：
- 生成静态网站到 `site/` 文件夹
- 创建或更新 `gh-pages` 分支
- 推送编译后的网站文件
- 自动处理分支管理

---

## 手动部署（替代方案）

如果 `mkdocs gh-deploy` 出现问题，可以手动部署：

### 方案 A：推送到 docs 分支

```powershell
# 1. 创建并切换到 docs 分支
git checkout -b docs

# 2. 添加所有文件
git add .

# 3. 推送到 docs 分支
git push -u origin docs
```

### 方案 B：完全手动管理 gh-pages 分支

如果要精细控制 GitHub Pages，也可以手动管理 gh-pages 分支：

```powershell
# 1. 生成网站
mkdocs build

# 2. 创建 gh-pages 分支
git checkout --orphan gh-pages
git rm -rf .

# 3. 添加生成的网站文件
Copy-Item site/* . -Recurse -Force

# 4. 提交
git add .
git commit -m "部署网站"
git push -u origin gh-pages

# 5. 切换回 docs 分支
git checkout docs
```

---

## 🌍 GitHub Pages 配置

部署完成后，需要在 GitHub 仓库设置中配置 GitHub Pages：

### 配置步骤：

1. 打开仓库: https://github.com/bb-yi/blender
2. 进入 **Settings** → **Pages**
3. **Source** 设置为：
   - **分支**: `gh-pages`
   - **文件夹**: `/(root)`
4. 点击 **Save**

### 访问你的网站：

- **仓库 Pages URL**: `https://bb-yi.github.io/blender/`
  
  或根据 GitHub 用户名/仓库名自动生成

⏳ 部署通常需要 1-2 分钟生效，稍候后在浏览器访问上述 URL。

---

## 📋 配置 mkdocs.yml（可选）

如果要自定义部署设置，编辑 `mkdocs.yml`：

```yaml
site_name: Blender 5.1 NPR Port 功能指南
site_url: https://bb-yi.github.io/blender/
repo_url: https://github.com/bb-yi/blender
repo_name: bb-yi/blender
edit_uri: edit/docs/docs/
```

---

## 🔄 后续更新

每次更新文档内容后：

```powershell
cd e:\blender_bulid_test\blender_npr_bulid\blender-5.1-npr-doc-site

# 提交本地更改
git add .
git commit -m "更新：[描述更改内容]"

# 自动生成并部署
mkdocs gh-deploy

# 或推送 docs 分支
git push origin docs
```

---

## ❓ 常见问题

### Q: mkdocs gh-deploy 提示权限错误？
**A**: 检查 GitHub 认证设置：
```powershell
# 如果使用 HTTPS，可能需要输入 PAT
# 如果使用 SSH，确保 SSH key 已配置

# 测试连接
git ls-remote https://github.com/bb-yi/blender
```

### Q: 网站仍显示旧内容？
**A**: 清除浏览器缓存（Ctrl+Shift+Delete），或用无痕模式访问

### Q: 如何自定义域名？
**A**: 在 GitHub Pages 设置中添加自定义域名，或编辑 `docs/CNAME` 文件

---

## 📚 更多信息

- [MkDocs 官方文档](https://www.mkdocs.org/)
- [MkDocs gh-deploy 说明](https://www.mkdocs.org/user-guide/deploying-your-docs/#github-pages)
- [GitHub Pages 文档](https://docs.github.com/en/pages)

