# 全球语言切换功能说明

你的文档网站现在有了**全局语言切换按钮**！✨

## 功能说明

### 位置
- 按钮位置：页面顶部右侧（紧跟导航栏）
- 按钮样式：固定浮动，带该地球符号 🌐

### 功能
点击按钮可以：

1. **切换页面内容** - 所有 English / 中文 选项卡自动切换
2. **切换导航菜单** - 左侧菜单标签自动更新为对应语言
3. **保存偏好** - 记住用户选择，下次访问时自动应用

## 使用方式

### 用户体验
```
访问网站 → 看到语言切换按钮 (右上角)
    ↓
点击按钮 → 导航菜单 + 内容同时切换语言
    ↓
页面刷新时 → 自动保留用户选择的语言
```

## 技术实现

### 文件结构
```
docs/
├── javascripts/
│   └── language-switcher.js          # 语言切换脚本
├── stylesheets/
│   └── language-switcher.css         # 按钮样式
└── mkdocs.yml                        # 配置文件
```

### 配置
mkdocs.yml 中：
- `extra_javascript` - 加载语言切换脚本
- `extra_css` - 加载样式文件
- `extra.alternate` - 定义可选语言

## 支持的语言

| 语言 | 按钮显示 |
|------|---------|
| English | 🇬🇧 English |
| 中文 | 🇨🇳 中文 |

## 浏览器支持

- ✅ Chrome / Edge（最新版）
- ✅ Firefox（最新版）
- ✅ Safari（11+）
- ⚠️ 需要启用 JavaScript

## 用户偏好存储

- 使用 `localStorage` 保存用户选择
- 键名：`doc-language`
- 值：`en` 或 `zh`

## 自定义

如需修改以下内容，编辑 `language-switcher.js`：

1. **导航菜单翻译** - 修改 `navigationMapZh` 对象
2. **按钮样式** - 修改 CSS 或 .css 文件
3. **按钮位置** - 修改 `position` 和 `top/right` 值
4. **按钮文本** - 修改 `buttonLabels` 对象

## 故障排除

### 按钮不显示
- 检查 JavaScript 是否启用
- 检查浏览器控制台是否有错误

### 切换后未生效
- 清除浏览器缓存
- 检查页面选项卡是否存在 `role="tab"` 属性

### 导航菜单未更新
- 确保导航标签与 `navigationMapZh` 中的键完全匹配
- 检查拼写和大小写

## 部署

无需特殊部署步骤，文件已包含在网站中。

## 之后的更新

添加新页面时：
1. 在 mkdocs.yml 中添加导航项
2. 在 `language-switcher.js` 中的 `navigationMapZh` 添加中文翻译
3. 重新构建/部署网站

---

**现在可以部署到 GitHub 了！** 🚀
