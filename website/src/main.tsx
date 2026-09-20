import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import {
    ArrowDown, ArrowUpRight, Check, Download, Headphones, Laptop, LifeBuoy, LockOpen, Menu, Monitor,
    Music2, PackageCheck, Play, Radio, ShieldCheck, SlidersHorizontal, Terminal, Trash2, X,
} from 'lucide-react';
import { release } from './release';
import hero from './assets/hero.jpg';
import './style.css';

const sizeMB = (release.sizeBytes / 1048576).toFixed(1);
const versionLabel = release.version ? `预览版 ${release.version}` : '源码预览版';

function Mark({ small = false }: { small?: boolean }) {
    return <span className={`mark ${small ? 'small' : ''}`} aria-hidden="true">{[12, 23, 33, 23, 12].map((h, i) => <i key={i} style={{ height: h }}/>)}</span>;
}

function DownloadLink({ secondary = false }: { secondary?: boolean }) {
    if (!release.downloadUrl) return <a className={`button ${secondary ? 'secondary' : ''}`} href="#download"><Laptop size={18}/> 获取 Sona for Mac</a>;
    return <a className={`button ${secondary ? 'secondary' : ''}`} href={release.downloadUrl} download={release.fileName}>
        <Download size={17}/> 下载 Sona {release.version}{secondary ? '' : ' for Mac'} <span className="button-meta">{sizeMB} MB</span>
    </a>;
}

function Header() {
    const [open, setOpen] = useState(false);
    const close = () => setOpen(false);
    return <header className="header wrap">
        <a className="brand" href="#" aria-label="Sona 首页"><Mark small/>sona<span className="brand-period">.</span></a>
        <button className="mobile-menu" aria-label={open ? '关闭菜单' : '打开菜单'} aria-expanded={open} onClick={() => setOpen(!open)}>{open ? <X /> : <Menu />}</button>
        <nav className={open ? 'open' : ''} aria-label="主导航">
            <a onClick={close} href="#features">功能</a>
            <a onClick={close} href="#how-it-works">使用方式</a>
            <a onClick={close} href="#install">安装说明</a>
            <a onClick={close} href="#faq">常见问题</a>
            <a onClick={close} className="nav-download" href="#download">获取 Sona <ArrowDown size={14}/></a>
        </nav>
    </header>;
}

// The real popover, captured on a Mac: two outputs with their own volume, two apps playing,
// and the per-app output menu open. Not a mock-up.
function Screenshot() {
    return <figure className="shot">
        <div className="shot-frame">
            <img src={hero} width={1448} height={1086} fetchPriority="high" decoding="async"
                 alt="Sona 菜单栏弹窗：输出设备列表里 MacBook Air 扬声器和 BlackHole 各有自己的音量；应用列表里 Safari 和网易云音乐正在播放，右侧展开了为应用选择输出设备的菜单"/>
        </div>
        <figcaption><span className="live-dot"/> 真实界面 · 两个输出设备各自的音量，两个应用各走各的路</figcaption>
    </figure>;
}

const uninstallCommand = 'sudo "/Library/Application Support/Sona/uninstall.sh"';
const terminalInstall = `sudo installer -pkg ~/Downloads/${release.fileName} -target /`;

function Install() {
    const pkg = release.fileName;
    return <section className="install wrap" id="install">
        <div className="section-heading">
            <div><span className="eyebrow">INSTALL &amp; GO</span><h2>四步装好，<br />马上能用。</h2></div>
            <p>安装包会同时装好菜单栏应用、虚拟输出设备驱动和后台音频服务。<br />全程需要一次管理员密码，安装结束时正在播放的声音会停顿一两秒。</p>
        </div>
        <ol className="install-steps">
            <li>
                <span className="step-index"><Download size={18}/> 01</span>
                <div>
                    <h3>下载安装包</h3>
                    <p>{release.downloadUrl ? <>下载 <code>{pkg}</code>（{sizeMB} MB）。要求 macOS {release.minMacOS} 或更高版本、Apple Silicon（M 系列芯片）的 Mac。</> : '安装包尚未发布。'}</p>
                    {release.downloadUrl && <DownloadLink secondary/>}
                    {release.sha256 && <p className="checksum">SHA-256 校验：<code>{release.sha256}</code><br />在终端运行 <code>shasum -a 256 ~/Downloads/{pkg}</code> 可核对。</p>}
                </div>
            </li>
            <li>
                <span className="step-index"><LockOpen size={18}/> 02</span>
                <div>
                    <h3>允许打开（只需一次）</h3>
                    {release.notarized
                        ? <p>安装包已经过 Apple 公证，双击即可打开。</p>
                        : <>
                            <p>这个预览版还没有经过 Apple 公证，所以双击安装包时 macOS 会提示「Apple 无法验证…」并拒绝打开。点「完成」，然后：</p>
                            <ol className="sub-steps">
                                <li>打开「系统设置」→「隐私与安全性」，向下滚动到「安全性」一栏。</li>
                                <li>找到「已阻止打开“{pkg}”以保护 Mac」，点「仍要打开」。</li>
                                <li>输入登录密码，再点一次「打开」，安装程序就会启动。</li>
                            </ol>
                            <p className="alt">习惯用终端的话，也可以跳过上面这些，直接安装：</p>
                            <pre><code>{terminalInstall}</code></pre>
                        </>}
                </div>
            </li>
            <li>
                <span className="step-index"><PackageCheck size={18}/> 03</span>
                <div>
                    <h3>按提示完成安装</h3>
                    <p>安装程序会请求管理员密码，然后放置三样东西：</p>
                    <ul className="paths">
                        <li><b>Sona.app</b><span>「应用程序」文件夹里的菜单栏界面</span></li>
                        <li><b>SonaDriver.driver</b><span>/Library/Audio/Plug-Ins/HAL，系统里名为「Sona」的虚拟输出设备</span></li>
                        <li><b>SonaAudioService</b><span>/Library/PrivilegedHelperTools，由 launchd 常驻的后台音频服务，负责混音并输出到真实设备</span></li>
                    </ul>
                    <p>最后一步会重新启动系统音频服务来加载驱动，正在播放的声音停顿一两秒后自动恢复。升级时直接安装新版本的安装包即可。</p>
                </div>
            </li>
            <li>
                <span className="step-index"><Play size={18}/> 04</span>
                <div>
                    <h3>开始使用</h3>
                    <p>安装完成后 Sona 会自动启动，菜单栏右上角出现它的波形图标。</p>
                    <ol className="sub-steps">
                        <li>点开图标，选择「默认输出」——通常就是你现在用的扬声器或耳机。</li>
                        <li>打开总开关，系统输出切换到 Sona；键盘音量键照常控制默认输出。</li>
                        <li>播放任意声音，正在发声的应用会出现在列表里：拖滑块调音量，点喇叭静音，点音箱图标为它选一个或多个输出设备。</li>
                        <li>建议在设置菜单里打开「登录时启动」，这样重启后不用再手动打开。</li>
                    </ol>
                </div>
            </li>
        </ol>
        <div className="install-aside">
            <article>
                <h3><LifeBuoy size={18}/> 检查与排错</h3>
                <ul>
                    <li><b>菜单栏没有图标</b>：在「应用程序」里打开 Sona。</li>
                    <li><b>菜单里提示「需要安装 Sona 驱动」或「等待设备出现」</b>：刚装完请等十秒左右；仍然如此就重新运行安装包。</li>
                    <li><b>提示「Sona Audio Service 未运行」</b>：后台服务离线时会暂时没有声音，launchd 通常几秒内会重新拉起；一直无声就重新运行安装包。</li>
                    <li><b>完全没有声音，想先恢复原状</b>：打开「系统设置」→「声音」→「输出」，选回扬声器或耳机。Sona 只接管被设为系统输出时的声音，随时可以切走。</li>
                    <li><b>某个应用不在列表里</b>：只有把声音送到系统默认输出的应用才会被接管；自己指定了物理设备的应用（例如部分专业音频软件）不受影响。</li>
                </ul>
            </article>
            <article>
                <h3><Trash2 size={18}/> 卸载</h3>
                <p>先在 Sona 菜单里点「退出 Sona」，让它把系统输出交还给真实设备，然后在终端运行：</p>
                <pre><code>{uninstallCommand}</code></pre>
                <p>这会删除应用、驱动和后台服务并重启系统音频服务；设置会保留，加 <code>--purge</code> 参数可一并清除。</p>
            </article>
            <article>
                <h3><Terminal size={18}/> 从源码安装</h3>
                <p>{release.sourceUrl ? <>更喜欢自己编译？<a href={release.sourceUrl} target="_blank" rel="noreferrer">源码仓库</a>的 README 里有 <code>make</code> 与 <code>sudo make install-service</code>、<code>sudo make install-driver</code> 的完整步骤。</> : <>源码仓库公开后，这里会给出 <code>make</code> 与 <code>sudo make install-service</code>、<code>sudo make install-driver</code> 的完整步骤。</>}</p>
            </article>
        </div>
    </section>;
}

const questions: [string, React.ReactNode][] = [
    ['哪些 Mac 可以使用 Sona？', `当前预览版要求 macOS ${release.minMacOS} 或更高版本，App 构建目标为 Apple Silicon（M 系列芯片）。Intel Mac 和更早的 macOS 暂不在已验证的支持范围内。`],
    ['现在可以直接下载安装吗？', release.downloadUrl
        ? <>可以。本页提供 {release.fileName} 安装包，安装步骤见<a href="#install">安装说明</a>。安装包还未经过 Apple 公证，首次打开需要在「隐私与安全性」里允许一次；安装过程中正在播放的声音会短暂中断。</>
        : '目前尚未提供安装包，暂时需要从源码构建并安装驱动和服务。'],
    ['打开安装包时提示「Apple 无法验证」怎么办？', <>这是 macOS 对未公证软件的常规提示，不是安装包损坏。点「完成」，到「系统设置 → 隐私与安全性」底部点「仍要打开」并输入密码即可继续；或者在终端运行 <code>{terminalInstall}</code>。完整步骤见<a href="#install">安装说明</a>。正式的 Developer ID 签名与公证会在后续版本提供。</>],
    ['Sona 会申请麦克风或系统录音权限吗？', '不会。Sona 通过虚拟音频输出设备和独立音频服务处理声音，自身不读取麦克风，也不申请系统录音权限。其他应用的录音指示不由 Sona 控制。'],
    ['一个应用可以同时输出到多个设备吗？', '可以。你可以为应用选择一个或多个输出设备。不过，不同设备之间不保证采样级同步；蓝牙、USB 热插拔和长时间播放等场景仍在验证中。'],
    ['为什么有些应用的声音无法控制？', 'Sona 只处理发送到 Sona 虚拟输出的声音。自行指定物理输出设备的应用（例如部分专业音频软件）不会被接管。Sona 目前也不提供逐应用麦克风路由。'],
    ['如何卸载？', <>先在 Sona 菜单里点「退出 Sona」，然后在终端运行 <code>{uninstallCommand}</code>。它会删除应用、驱动和后台服务；加 <code>--purge</code> 会连设置一起删除。</>],
];

function App() {
    return <>
        <a className="skip" href="#main">跳至正文</a>
        <Header />
        <main id="main">
            <section className="hero wrap">
                <div className="hero-copy">
                    <div className="eyebrow"><span className="live-dot"/> 为 Mac 而生的音频控制工具</div>
                    <h1>让声音，<br /><span>各就各位。</span></h1>
                    <p className="hero-description">音乐轻一点，会议清楚一点。<br />每个应用的音量和去向，都由你决定。</p>
                    <div className="hero-actions"><DownloadLink /><a className="text-link" href="#install">安装说明 <span>↗</span></a></div>
                    <div className="requirements">macOS {release.minMacOS}+ <span>·</span> Apple Silicon <span>·</span> {versionLabel}</div>
                </div>
                <Screenshot />
            </section>
            <div className="promise-strip wrap"><span>小小菜单栏，大大掌控感。</span><div><span><Check size={15}/> 逐应用音量</span><span><Check size={15}/> 多设备输出</span><span><Check size={15}/> 无需录音权限</span></div></div>
            <section className="features wrap" id="features">
                <div className="section-heading"><div><span className="eyebrow">LESS FRICTION. MORE FOCUS.</span><h2>你的声音，<br />不必只有一个音量。</h2></div><p>专注工作，放松听歌，或加入一场会议。<br />让每一种声音，都待在合适的位置。</p></div>
                <div className="feature-layout">
                    <article className="feature-main"><div className="feature-number">01 / 独立音量</div><h3>背景音乐，<br />就让它待在背景。</h3><p>调低音乐，保留会议的清晰。<br />不必再在不同应用的设置之间来回切换。</p><div className="mini-level"><Music2 size={20}/><span>音乐</span><div><i style={{ width: '32%' }}/></div><span>32%</span></div><div className="mini-level"><Radio size={20}/><span>会议</span><div><i style={{ width: '86%' }}/></div><span>86%</span></div></article>
                    <div className="feature-side">
                        <article><Headphones size={27}/><div><span className="feature-number">02 / 自由分流</span><h3>耳机听会议，音箱放音乐。</h3><p>为不同应用指定输出，也能让一个应用同时连接多个设备。</p></div></article>
                        <article><ShieldCheck size={27}/><div><span className="feature-number">03 / 安心使用</span><h3>管理声音，无需打开麦克风。</h3><p>通过音频输出链路工作，不申请麦克风或系统录音权限。</p></div></article>
                        <article><SlidersHorizontal size={27}/><div><span className="feature-number">04 / 恰到好处</span><h3>记住偏好，藏在菜单栏。</h3><p>保存应用的音量与路由设置，需要时一键展开。</p></div></article>
                    </div>
                </div>
            </section>
            <section className="workflow" id="how-it-works">
                <div className="wrap">
                    <div className="section-heading"><div><span className="eyebrow">A LITTLE CONTROL. A LOT OF CALM.</span><h2>调整一下，回到专注。</h2></div><span className="workflow-tag">安装 Sona 后</span></div>
                    <div className="steps">{[['01', '从菜单栏打开', '选好默认物理输出设备，再打开 Sona 总开关。'], ['02', '找到正在发声的应用', '播放声音，独立调整音量，或一键静音。'], ['03', '让声音去对的地方', '选择耳机、扬声器，或多个输出设备。']].map(([n, title, body]) => <article key={n}><span>{n}</span><h3>{title}</h3><p>{body}</p></article>)}</div>
                </div>
            </section>
            <section className="download-section wrap" id="download">
                <div><span className="eyebrow">SOUND, YOUR WAY.</span><h2>给你的 Mac，<br />更细腻的声音掌控。</h2><p>从每一次小小的调整，找到舒服的节奏。</p></div>
                <div className="download-panel">
                    <div className="download-title"><Mark /><div><strong>Sona for Mac</strong><span>{release.version ? `版本 ${release.version} · ${release.releasedAt}` : '源码预览版'}</span></div></div>
                    <div className="compatibility"><span><Check size={15}/> macOS {release.minMacOS} 或更高版本</span><span><Check size={15}/> Apple Silicon · M 系列芯片</span><span><Check size={15}/> 不申请麦克风或录音权限</span></div>
                    {release.downloadUrl
                        ? <>
                            <DownloadLink secondary/>
                            <p className="release-note">{release.fileName} · {sizeMB} MB{release.notarized ? ' · 已公证' : ' · 未公证：首次打开需在「隐私与安全性」里允许一次'}。</p>
                            <a className="text-link" href="#install">安装说明与卸载方法 <ArrowDown size={14}/></a>
                        </>
                        : <><button className="button unavailable" disabled><ArrowDown size={17}/> 安装包准备中</button><p className="release-note">正式安装包尚未发布。<br />当前版本需要自行编译和安装。</p></>}
                    {release.sourceUrl && <a className="text-link" href={release.sourceUrl} target="_blank" rel="noreferrer">查看源码与构建指南 <ArrowUpRight size={14}/></a>}
                </div>
            </section>
            <Install />
            <section className="faq wrap" id="faq">
                <div><span className="eyebrow">GOOD TO KNOW</span><h2>你可能想知道。</h2></div>
                <div className="faq-items">{questions.map(([q, a]) => <details key={q}><summary>{q}<span>+</span></summary><p>{a}</p></details>)}</div>
            </section>
        </main>
        <footer className="wrap"><a className="brand" href="#"><Mark small/>sona.</a><span>Sound, your way.</span><a href="#download">为 Mac 而生 <Monitor size={14}/></a></footer>
    </>;
}

createRoot(document.getElementById('root')!).render(<React.StrictMode><App /></React.StrictMode>);
