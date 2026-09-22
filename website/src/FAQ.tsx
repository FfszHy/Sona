import type React from "react";
import { release } from "./release";
const uninstallCommand =
  'sudo "/Library/Application Support/Sona/uninstall.sh"';
const terminalInstall = `sudo installer -pkg ~/Downloads/${release.fileName} -target /`;
const questions: [string, React.ReactNode][] = [
  [
    "哪些 Mac 可以使用 Sona？",
    `当前预览版要求 macOS ${release.minMacOS} 或更高版本，App 构建目标为 Apple Silicon（M 系列芯片）。Intel Mac 和更早的 macOS 暂不在已验证的支持范围内。`,
  ],
  [
    "现在可以直接下载安装吗？",
    release.downloadUrl ? (
      <>
        可以。本页提供 {release.fileName} 安装包，安装步骤见
        <a href="/install">安装说明</a>。安装包还未经过 Apple
        公证，首次打开需要在「隐私与安全性」里允许一次；安装过程中正在播放的声音会短暂中断。
      </>
    ) : (
      "目前尚未提供安装包，暂时需要从源码构建并安装驱动和服务。"
    ),
  ],
  [
    "打开安装包时提示「Apple 无法验证」怎么办？",
    <>
      这是 macOS
      对未公证软件的常规提示，不是安装包损坏。点「完成」，到「系统设置 →
      隐私与安全性」底部点「仍要打开」并输入密码即可继续；或者在终端运行{" "}
      <code>{terminalInstall}</code>。完整步骤见<a href="/install">安装说明</a>
      。正式的 Developer ID 签名与公证会在后续版本提供。
    </>,
  ],
  [
    "Sona 会申请麦克风或系统录音权限吗？",
    "不会。Sona 通过虚拟音频输出设备和独立音频服务处理声音，自身不读取麦克风，也不申请系统录音权限。其他应用的录音指示不由 Sona 控制。",
  ],
  [
    "一个应用可以同时输出到多个设备吗？",
    "可以。你可以为应用选择一个或多个输出设备。不过，不同设备之间不保证采样级同步；蓝牙、USB 热插拔和长时间播放等场景仍在验证中。",
  ],
  [
    "为什么有些应用的声音无法控制？",
    "Sona 只处理发送到 Sona 虚拟输出的声音。自行指定物理输出设备的应用（例如部分专业音频软件）不会被接管。Sona 目前也不提供逐应用麦克风路由。",
  ],
  [
    "如何卸载？",
    <>
      先在 Sona 菜单里点「退出 Sona」，然后在终端运行{" "}
      <code>{uninstallCommand}</code>。它会删除应用、驱动和后台服务；加{" "}
      <code>--purge</code> 会连设置一起删除。
    </>,
  ],
];

export function FAQ() {
  return (
    <section className="faq wrap" id="faq">
      <h2>你可能想知道。</h2>
      <div className="faq-items">
        {questions.map(([q, a]) => (
          <details key={q}>
            <summary>
              {q}
              <span>+</span>
            </summary>
            <p>{a}</p>
          </details>
        ))}
      </div>
    </section>
  );
}
