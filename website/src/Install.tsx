import {
  Download,
  LockOpen,
  PackageCheck,
  Play,
  LifeBuoy,
  Trash2,
  Terminal,
} from "lucide-react";
import { release } from "./release";
import { DownloadLink, sizeMB } from "./Shared";
const uninstallCommand =
  'sudo "/Library/Application Support/Sona/uninstall.sh"';
const terminalInstall = `sudo installer -pkg ~/Downloads/${release.fileName} -target /`;

export function Install() {
  const pkg = release.fileName;
  return (
    <section className="install wrap" id="install">
      <div className="section-heading">
        <div>
          <a className="text-link" href="/">
            ← 返回首页
          </a>
          <h1>安装 Sona。</h1>
        </div>
        <p>
          安装包会同时装好菜单栏应用、虚拟输出设备驱动和后台音频服务。
          <br />
          全程需要一次管理员密码，安装结束时正在播放的声音会停顿一两秒。
        </p>
      </div>
      <ol className="install-steps">
        <li>
          <span className="step-index">
            <Download size={18} /> 01
          </span>
          <div>
            <h3>下载安装包</h3>
            <p>
              {release.downloadUrl ? (
                <>
                  下载 <code>{pkg}</code>（{sizeMB} MB）。要求 macOS{" "}
                  {release.minMacOS} 或更高版本、Apple Silicon（M 系列芯片）的
                  Mac。
                </>
              ) : (
                "安装包尚未发布。"
              )}
            </p>
            {release.downloadUrl && <DownloadLink secondary />}
            {release.sha256 && (
              <p className="checksum">
                SHA-256 校验：<code>{release.sha256}</code>
                <br />
                在终端运行 <code>shasum -a 256 ~/Downloads/{pkg}</code> 可核对。
              </p>
            )}
          </div>
        </li>
        <li>
          <span className="step-index">
            <LockOpen size={18} /> 02
          </span>
          <div>
            <h3>允许打开（只需一次）</h3>
            {release.notarized ? (
              <p>安装包已经过 Apple 公证，双击即可打开。</p>
            ) : (
              <>
                <p>
                  这个预览版还没有经过 Apple 公证，所以双击安装包时 macOS
                  会提示「Apple 无法验证…」并拒绝打开。点「完成」，然后：
                </p>
                <ol className="sub-steps">
                  <li>
                    打开「系统设置」→「隐私与安全性」，向下滚动到「安全性」一栏。
                  </li>
                  <li>找到「已阻止打开“{pkg}”以保护 Mac」，点「仍要打开」。</li>
                  <li>输入登录密码，再点一次「打开」，安装程序就会启动。</li>
                </ol>
                <p className="alt">
                  习惯用终端的话，也可以跳过上面这些，直接安装：
                </p>
                <pre>
                  <code>{terminalInstall}</code>
                </pre>
              </>
            )}
          </div>
        </li>
        <li>
          <span className="step-index">
            <PackageCheck size={18} /> 03
          </span>
          <div>
            <h3>按提示完成安装</h3>
            <p>安装程序会请求管理员密码，然后放置三样东西：</p>
            <ul className="paths">
              <li>
                <b>Sona.app</b>
                <span>「应用程序」文件夹里的菜单栏界面</span>
              </li>
              <li>
                <b>SonaDriver.driver</b>
                <span>
                  /Library/Audio/Plug-Ins/HAL，系统里名为「Sona」的虚拟输出设备
                </span>
              </li>
              <li>
                <b>SonaAudioService</b>
                <span>
                  /Library/PrivilegedHelperTools，由 launchd
                  常驻的后台音频服务，负责混音并输出到真实设备
                </span>
              </li>
            </ul>
            <p>
              最后一步会重新启动系统音频服务来加载驱动，正在播放的声音停顿一两秒后自动恢复。升级时直接安装新版本的安装包即可。
            </p>
          </div>
        </li>
        <li>
          <span className="step-index">
            <Play size={18} /> 04
          </span>
          <div>
            <h3>开始使用</h3>
            <p>安装完成后 Sona 会自动启动，菜单栏右上角出现它的波形图标。</p>
            <ol className="sub-steps">
              <li>
                点开图标，选择「默认输出」——通常就是你现在用的扬声器或耳机。
              </li>
              <li>
                打开总开关，系统输出切换到 Sona；键盘音量键照常控制默认输出。
              </li>
              <li>
                播放任意声音，正在发声的应用会出现在列表里：拖滑块调音量，点喇叭静音，点音箱图标为它选一个或多个输出设备。
              </li>
              <li>
                建议在设置菜单里打开「登录时启动」，这样重启后不用再手动打开。
              </li>
            </ol>
          </div>
        </li>
      </ol>
      <div className="install-aside">
        <article>
          <h3>
            <LifeBuoy size={18} /> 检查与排错
          </h3>
          <ul>
            <li>
              <b>菜单栏没有图标</b>：在「应用程序」里打开 Sona。
            </li>
            <li>
              <b>菜单里提示「需要安装 Sona 驱动」或「等待设备出现」</b>
              ：刚装完请等十秒左右；仍然如此就重新运行安装包。
            </li>
            <li>
              <b>提示「Sona Audio Service 未运行」</b>
              ：后台服务离线时会暂时没有声音，launchd
              通常几秒内会重新拉起；一直无声就重新运行安装包。
            </li>
            <li>
              <b>完全没有声音，想先恢复原状</b>
              ：打开「系统设置」→「声音」→「输出」，选回扬声器或耳机。Sona
              只接管被设为系统输出时的声音，随时可以切走。
            </li>
            <li>
              <b>某个应用不在列表里</b>
              ：只有把声音送到系统默认输出的应用才会被接管；自己指定了物理设备的应用（例如部分专业音频软件）不受影响。
            </li>
          </ul>
        </article>
        <article>
          <h3>
            <Trash2 size={18} /> 卸载
          </h3>
          <p>
            先在 Sona 菜单里点「退出
            Sona」，让它把系统输出交还给真实设备，然后在终端运行：
          </p>
          <pre>
            <code>{uninstallCommand}</code>
          </pre>
          <p>
            这会删除应用、驱动和后台服务并重启系统音频服务；设置会保留，加{" "}
            <code>--purge</code> 参数可一并清除。
          </p>
        </article>
        <article>
          <h3>
            <Terminal size={18} /> 从源码安装
          </h3>
          <p>
            {release.sourceUrl ? (
              <>
                更喜欢自己编译？
                <a href={release.sourceUrl} target="_blank" rel="noreferrer">
                  源码仓库
                </a>
                的 README 里有 <code>make</code> 与{" "}
                <code>sudo make install-service</code>、
                <code>sudo make install-driver</code> 的完整步骤。
              </>
            ) : (
              <>
                更喜欢自己编译？仓库 README 的「从源码」一节给出了 <code>make</code>{" "}
                与 <code>sudo make install-service</code>、
                <code>sudo make install-driver</code> 的完整步骤。仓库地址：
                <code>github.com/FfszHy/Sona</code>。
              </>
            )}
          </p>
        </article>
      </div>
    </section>
  );
}
