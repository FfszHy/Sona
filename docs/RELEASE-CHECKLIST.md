# 来源与发布检查

## 本次源码整理

- 已排除构建产物、Python 缓存、macOS 元数据及本机工具配置。
- 对源码进行常见凭据格式和个人绝对路径扫描；未发现匹配。这不是完整安全审计。
- Swift package 未声明外部 package 依赖；构建主要使用 Apple 系统框架。
- `Service/SonaResampler.h` 标注了重采样算法背景链接；算法参考不等于复制代码的来源证明。

## 发布前仍需项目作者确认

- [ ] 选择开源许可证并确认版权署名。
- [ ] 确认源码的原创或授权来源；目前没有足以逐文件证明来源的历史记录。
- [ ] 确认 `pic/` 图片及 `App/Resources/AppIcon.icns` 可公开分发。现有文件无法自行证明图片授权。
- [ ] 检查截图中展示的应用、设备及个人内容是否适合公开。
- [ ] 按兼容性记录完成拟宣称支持的平台和设备验证。
- [ ] 创建 GitHub 仓库、推送源码并发布预览版（目前尚未执行）。
- [ ] 加入 Apple Developer Program，创建 Developer ID Application / Installer 证书，`xcrun notarytool store-credentials` 保存公证凭据，然后带着 `SONA_APP_IDENTITY`、`SONA_INSTALLER_IDENTITY`、`SONA_NOTARY_PROFILE` 重新 `make release`。在此之前安装包未签名、未公证，用户首次打开需在「隐私与安全性」里允许。

## 安装包发布流程（2026-09-20 起）

1. 更新 `App/Info.plist`、`Driver/Info.plist` 的 `CFBundleShortVersionString`/`CFBundleVersion` 和 `Shared/SonaProtocol.h` 的 `kSonaDriverVersion`，补 `CHANGELOG.md`。
2. `make test-all`，涉及 IPC 时再 `make test-e2e`。
3. `make release`：生成 `build/release/Sona-<版本>.pkg` 与 `.sha256`，复制到 `website/public/downloads/`，重写 `website/src/release.ts` 和 `website/vercel.json` 里的 `/download` 跳转。
4. 在干净的机器或用户上实机验证：双击安装包走「仍要打开」流程；安装后菜单栏出现 Sona，选默认输出、打开开关有声音；`sudo installer -pkg … -target /` 路径同样可用；运行卸载脚本后系统输出回到物理设备。
5. `cd website && npm run build && npx vercel --prod` 部署官网；用 `curl -I <站点>/download` 确认跳转到新版本安装包。
6. 源码仓库公开后，安装包可改为 GitHub Release 资产，用 `SONA_DOWNLOAD_BASE` 指向新地址重新生成 `release.ts`。

Developer ID 签名、公证、升级流程和卸载验收在上述第 4 步和最后一个确认项中跟踪。
