# 来源与发布检查

## 本次源码整理

- 已排除构建产物、Python 缓存、macOS 元数据及本机工具配置。
- 对源码进行常见凭据格式和个人绝对路径扫描；未发现匹配。这不是完整安全审计。
- Swift package 未声明外部 package 依赖；构建主要使用 Apple 系统框架。
- `Service/SonaResampler.h` 标注了重采样算法背景链接；算法参考不等于复制代码的来源证明。

## 发布前仍需项目作者确认

- [x] 已采用 FSL-1.1-ALv2，版权署名见仓库根目录 `LICENSE`。这是源码可见许可证，不应表述为无限制商用。
- [ ] 确认源码的原创或授权来源；目前没有足以逐文件证明来源的历史记录。
- [ ] 确认 `pic/` 图片及 `App/Resources/AppIcon.icns` 可公开分发。现有文件无法自行证明图片授权。
- [x] 当前 README 实机截图由项目作者于 2026-10-19 提供并要求用于公开仓库，原图未修饰；后续新增截图仍需检查公开范围。
- [ ] 按兼容性记录完成拟宣称支持的平台和设备验证。
- [x] 已有公开源码仓库：`https://github.com/FfszHy/Sona`，默认分支为 `main`。
- [ ] 发布 `v0.6.1` GitHub Preview Release，上传 `Sona-0.6.1.pkg` 与 `Sona-0.6.1.pkg.sha256`，并回读验证远端资产校验值。
- [ ] 加入 Apple Developer Program，创建 Developer ID Application / Installer 证书，`xcrun notarytool store-credentials` 保存公证凭据，然后带着 `SONA_APP_IDENTITY`、`SONA_INSTALLER_IDENTITY`、`SONA_NOTARY_PROFILE` 重新 `make release`。在此之前安装包未签名、未公证，用户首次打开需在「隐私与安全性」里允许。

## 0.6.1 安装包发布流程（2026-10-19）

1. 将 `App/Info.plist`、`Driver/Info.plist` 的版本设为 0.6.1、build 10，更新 `Shared/SonaProtocol.h` 的 `kSonaDriverVersion`，补 `CHANGELOG.md`。
2. `make test-all`，涉及 IPC 时再 `make test-e2e`。
3. 使用 `SONA_SOURCE_URL=https://github.com/FfszHy/Sona` 和 `SONA_DOWNLOAD_BASE=https://github.com/FfszHy/Sona/releases/download/v0.6.1` 执行 `make release`：生成安装包与 `.sha256`，同步网站下载资源、元数据和 `/download` 跳转。检查包内版本、安装脚本、二进制架构和签名状态。
4. 在干净的机器或用户上实机验证：双击安装包走「仍要打开」流程；安装后菜单栏出现 Sona，选择默认输出、播放音频，检查逐应用音量和输出选择；验证旧版升级、`sudo installer -pkg … -target /` 和卸载恢复物理输出。该项需单独记录实机结果，构建与静态检查不能替代。
5. 创建 `v0.6.1` 标签和 GitHub 预发布，上传安装包及校验文件；说明预览状态、Developer ID 签名与公证状态、已验证范围和未完成的设备验收。下载远端资产，与本地产物逐字节或 SHA-256 对比。
6. 如需部署官网，先运行 `cd website && npm run build`，再部署；用 `curl -I <站点>/download` 确认跳转到新版本安装包。官网构建成功不代表已经部署。

Developer ID 签名、公证、完整安装、升级与卸载验收仍需分别记录结果；发布预览包不代表这些项目已完成。
