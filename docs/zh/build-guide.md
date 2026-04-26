# 本地构建流程

本文档只针对当前 Blender NPR 工作区：

- 源码目录：`E:\blender_bulid_test\blender_npr_bulid\blender_5_1_port`
- 并行主线 worktree：`E:\blender_bulid_test\blender_npr_bulid\blender_5_1_port_mainfix`
- 默认构建树：`build_windows_x64_vc17_Release_5_1_port_clean_ninja`
- 默认安装树：`install_windows_x64_vc17_Release_5_1_port_clean`
- mainfix 构建树：`build_windows_x64_vc17_Release_5_1_port_mainfix_ninja`
- mainfix 安装树：`install_windows_x64_vc17_Release_5_1_port_mainfix`

目标是让本地协作时都走同一套增量构建和验证流程，不重复新建目录、手动猜测环境或误把安装占用当成源码错误。

## 固定规则

1. 默认使用现有 Ninja 构建树，不新建新的 `build_*` 目录。
2. 默认做增量编译，不随意 `reconfigure-clean`。
3. 最终测试以安装树里的 `blender.exe` 为准，不以 build tree 可执行文件代替。
4. 非交互环境统一追加 `--no-pause`。
5. 同一构建树禁止并发编译；开始前先确认没有残留 `cmake.exe` / `ninja.exe`。

## 日常命令

### 默认 / 日常测试

```powershell
.\build_ninja_sccache_poll.bat install --no-pause
```

- 默认 `WITH_CYCLES=OFF`
- 输出到 `install_windows_x64_vc17_Release_5_1_port_clean`

### 并行主线 mainfix

```powershell
.\build_ninja_sccache_poll.bat mainfix install --no-pause
```

- 使用 `blender_5_1_port_mainfix`
- 输出到 `install_windows_x64_vc17_Release_5_1_port_mainfix`
- 不覆盖默认安装树

### 发布前 / 需要 Cycles

```powershell
.\build_ninja_sccache_poll.bat with-cycles install --no-pause
```

### 遇到 Unity 漏编

```powershell
.\build_ninja_sccache.bat clean-unity install --no-pause
```

## 验证方式

构建完成后优先检查安装树版本：

```powershell
& ".\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe" --background --factory-startup --version
```

或 mainfix：

```powershell
& ".\install_windows_x64_vc17_Release_5_1_port_mainfix\blender.exe" --background --factory-startup --version
```

时间戳本身不足以证明运行时已经更新，最好至少再补一条后台 smoke test。

## 常见问题

### `INSTALL` 失败 / 文件被占用

最常见原因是安装树里的 `blender.exe` 还在运行。先关闭对应安装树的 Blender，再重跑 `install`。

### `build_ninja_sccache_poll.bat` 看起来卡住

- 先看是否仍有 `cmake.exe` / `ninja.exe` / `cl.exe`
- 再看 `temp\codex-build-logs`
- 不要在同一构建树上再启动第二条编译链

### `sccache` 或 `cl.exe` 返回 `-1`

优先降低并发继续增量续跑，例如：

```powershell
$env:BUILD_JOBS='8'
.\build_ninja_sccache_poll.bat mainfix install --no-pause
```

### Windows 预编译库找不到

`blender_5_1_port_mainfix\lib\windows_x64` 必须可用。当前允许的做法是：

- `windows_x64` 根目录保留为真实目录
- 内部各库子目录用 junction 指向 `blender_5_1_port\lib\windows_x64\...`

不要把整个 `windows_x64` 根目录直接做成 junction。
