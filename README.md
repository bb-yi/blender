# Blender 5.1 NPR Port 文档网站

这是一个使用 MkDocs 构建的 Blender 5.1 NPR Port 功能文档网站。

## 项目结构

```
blender-5.1-npr-doc-site/
├── mkdocs.yml          # MkDocs 配置文件
├── docs/               # 文档源文件目录
│   ├── index.md        # 首页
│   ├── 1_scene_level_extension.md      # Scene 级 Eevee 扩展
│   ├── 2_extended_nodes.md             # 主要扩展节点
│   ├── 3_npr_tree_workflow.md          # NPR Tree 工作流
│   ├── 4_interface_workflow.md         # 界面与工作流补充
│   └── images/         # 文档中使用的图片
└── site/               # 生成的静态网站（build后产生）
```

## 快速开始

### 1. 安装依赖

如果你的系统中还没有安装 MkDocs，请先安装：

```bash
pip install mkdocs mkdocs-material
```

### 2. 本地预览

如果你要看和 GitHub Pages 部署后一模一样的路径与语言切换行为，请使用：

```powershell
cd blender-5.1-npr-doc-site
powershell -ExecutionPolicy Bypass -File .\preview-ghpages.ps1
```

然后在浏览器中打开 `http://127.0.0.1:8000/blender/`。

这个方式会先构建双语站点，再按 `/blender/` 路径提供本地静态预览，因此和线上部署行为一致。

如果只是临时调试单一语言页面，也可以使用：

```powershell
mkdocs serve -f mkdocs.yml
```

但这种方式不会完整模拟 GitHub Pages 的 `/blender/` 路径和双语切换。

### 3. 生成静态网站

生成可部署的静态网站文件：

```bash
mkdocs build
```

生成的文件将保存在 `site/` 文件夹中。

## 文档内容

### 首页 (index.md)
- 项目介绍
- 功能总览
- 快速导航

### Scene 级 Eevee 扩展 (1_scene_level_extension.md)
- Render Textures 系统
- Filter Materials 全屏滤镜

### 主要扩展节点 (2_extended_nodes.md)
- Eevee 通用辅助节点
  - Render Info
  - Scene Time
  - Screen Derivative
  - Portal In / Out
- Eevee 物体材质节点
  - Render Texture
  - Screenspace Info
  - World Environment
  - World To Tangent
  - GLSL Function
  - Image to Closure
  - Bevel
  - Curvature
  - Shader Info
  - Light Info
- Filter 域节点
  - Scene Color

### NPR Tree 工作流 (3_npr_tree_workflow.md)
- 基本概念和挂接方式
- NPR Input / Output
- NPR Refraction
- Image Sample
- For Each Light
- 内置节点组资产

### 界面与工作流补充 (4_interface_workflow.md)
- 材质选择器预览开关
- 世界环境排除
- Eevee Performance
- Lightgroup ID
- 启动图版本标识
- 常见问题解决

## 配置说明

mkdocs.yml 配置文件包含以下主要设置：

- **site_name**: 网站标题
- **theme**: 使用 Material 主题
- **nav**: 导航菜单结构
- **markdown_extensions**: Markdown 扩展支持
- **plugins**: 搜索、视频等插件

## 主题特性

该网站使用 Material for MkDocs 主题，提供的特性包括：

- ✨ 亮色/深色模式切换
- 🔍 全文搜索
- 📱 响应式设计（适配手机和平板）
- ⚡ 快速加载
- 💬 代码块高亮和复制按钮
- 📊 支持表格、提示块、代码注释

## 部署

### 部署到 GitHub Pages

1. 创建一个 GitHub 仓库
2. 将项目推送到仓库
3. 在 GitHub 仓库设置中启用 GitHub Pages
4. 选择 `gh-pages` 分支作为发布源

使用以下脚本自动部署：

```bash
mkdocs gh-deploy
```

### 部署到其他平台

生成的 `site/` 文件夹可以部署到任何静态网站托管服务，如：
- Netlify
- Vercel
- GitLab Pages
- 自己的服务器

## 修改和扩展

### 添加新文档

1. 在 `docs/` 文件夹中创建新的 `.md` 文件
2. 在 `mkdocs.yml` 的 `nav` 部分添加导航链接
3. 运行 `mkdocs serve` 预览效果

### 修改网站配置

编辑 `mkdocs.yml` 文件可以：
- 改变网站名称和描述
- 调整主题颜色和特性
- 修改导航结构
- 添加或移除扩展功能

### 添加图片

1. 将图片放在 `docs/images/` 文件夹中
2. 在 Markdown 中使用相对路径引用：`![描述](images/文件名.png)`

## 许可证

本文档遵循与 Blender 5.1 NPR Port 相同的许可证。

## 支持和反馈

如有问题或建议，请提交 Issue 或 Pull Request。

## 参考

- [MkDocs 官方文档](https://www.mkdocs.org/)
- [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/)
- [Markdown 语法](https://www.markdownguide.org/)

---

**最后更新**: 2026-04-06

**构建工具**: MkDocs + Material Theme
