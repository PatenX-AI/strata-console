# 发布说明

本文件给发布者和新会话使用。目标：把 `Strata Console` 源码上传到 GitHub，并发布 Windows 可执行包。

## 1. 前置条件

- Windows，已安装 Git。
- 安装 GitHub CLI：
  ```powershell
  winget install --id GitHub.cli -e
  ```
- 登录 GitHub：
  ```powershell
  gh auth login
  gh auth status
  ```
- 需要确认：
  - GitHub 账号/组织（owner）
  - 仓库名（建议 `strata-console`）
  - 可见性（public / private）

## 2. 构建与打包

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
powershell -ExecutionPolicy Bypass -File .\package.ps1 -Version 0.1.4
```

产物：

```text
dist\strata-console-v0.1.4-win64.zip
```

包内包含：

- `strata-console.exe`
- `ui/`（HTML/CSS/JS/字体）
- `auto_config_helper.py`
- `README.md`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `start-daemon.bat`
- `stop-console.bat`

## 3. 上传前检查

不得提交：

- `design-notes/`（设计笔记和本机截图）
- `strata-console.json`（本机路径和 control_token）
- `build/`、`dist/`
- `strata-console.exe`（作为 Release 资产，不放入源码仓库）
- `*.log`

`.gitignore` 已覆盖这些路径。检查：

```powershell
git status --short
git ls-files | Select-String -Pattern "design-notes|strata-console.json|strata-console.exe|^build/|^dist/"
```

最后一条应没有输出。

## 4. 初始化并上传源码

```powershell
git init
git branch -M main
git add .
git status --short
git commit -m "Initial release: Strata Console 0.1.4"
```

创建远程仓库并推送（把 `<owner>`、`<repo>` 换成确认后的值）：

```powershell
gh repo create <owner>/<repo> --public --source=. --remote=origin --push
```

如果仓库已存在，不要覆盖；先确认是否要推送到现有仓库。

## 5. 创建 Release

```powershell
git tag v0.1.4
git push origin v0.1.4
gh release create v0.1.4 .\dist\strata-console-v0.1.4-win64.zip `
  --title "Strata Console v0.1.4" `
  --notes-file RELEASE_NOTES.md
```

`RELEASE_NOTES.md` 可写：

- 官方风格 Web 控制台（基本 / 推理 / 网络 / 配置管理）
- 启动、停止、重启、加载、卸载
- GPU/VRAM/CPU/RAM/请求/日志监控
- 自动配置、时间戳备份、自动重启、空闲卸载
- 图片输入（CPU/GPU，当前本机为 CPU）
- MIT License

## 6. 验证

```powershell
gh repo view <owner>/<repo> --web
gh release view v0.1.4 --web
```

确认：

- 仓库中没有 `design-notes/`、`strata-console.json`、`strata-console.exe`、`build/`、`dist/`；
- Release 页面有 `strata-console-v0.1.4-win64.zip`；
- 下载 zip 后解压，双击 `strata-console.exe` 能打开控制台；
- 没有把 API Key、control_token 或本机路径提交到仓库。

## 7. 本地快速验证

```powershell
.\strata-console.exe --daemon
Invoke-RestMethod http://127.0.0.1:8090/api/health
.\strata-console.exe --stop
```

