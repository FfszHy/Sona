import React from "react";
import { createRoot } from "react-dom/client";
import {
  ArrowDown,
  ArrowRight,
  ArrowUpRight,
  Download,
  AudioLines,
  LockOpen,
} from "lucide-react";
import { release } from "./release";
import { Header, DownloadLink, Mark, sizeMB } from "./Shared";
import { Install } from "./Install";
import { FAQ } from "./FAQ";
import {
  HeroScene,
  VolumeScene,
  RouteScene,
  RoutingScene,
  PrivacyScene,
  MemoryScene,
  WorkflowScene,
} from "./Scenes";
import "./style.css";
import "./install.css";
function DownloadSection() {
  return (
    <section className="download-section wrap" id="download">
      <img
        className="download-icon"
        src="/media/sona.png"
        alt="Sona"
        width="96"
        height="96"
      />
      <h2>Sona for Mac.</h2>
      <p>你的声音，你来决定。</p>
      <DownloadLink />
      <div className="requirements">
        macOS {release.minMacOS}+ · Apple Silicon · {release.version} · {sizeMB}{" "}
        MB
      </div>
      <p className="release-note">
        {release.notarized
          ? "已通过 Apple 公证。"
          : "预览版尚未公证，首次安装需在「隐私与安全性」中允许打开。"}
        <a href="/install">
          查看说明 <ArrowUpRight size={12} />
        </a>
      </p>
    </section>
  );
}
function QuickInstall() {
  return (
    <section className="quick-install wrap" id="install">
      <div>
        <h2>3 分钟，开始使用。</h2>
        <a className="text-link" href="/install">
          查看完整安装说明 <ArrowRight size={16} />
        </a>
      </div>
      <ol>
        {[
          [Download, "下载 Sona"],
          [LockOpen, "允许安装"],
          [AudioLines, "菜单栏打开 Sona"],
        ].map(([Icon, label], i) => {
          const Glyph = Icon as typeof Download;
          return (
            <li key={i}>
              <span>0{i + 1}</span>
              <Glyph size={20} />
              <b>{label as string}</b>
            </li>
          );
        })}
      </ol>
    </section>
  );
}
function Footer() {
  return (
    <>
      <section className="final-cta wrap">
        <h2>Sound, your way.</h2>
        <a href="/#download" aria-label="获取 Sona">
          <ArrowDown size={32} />
        </a>
      </section>
      <footer className="wrap">
        <a className="brand" href="/">
          <Mark small />
          sona.
        </a>
        <span>为 Mac 而生。</span>
        <a href="/install">
          安装与卸载 <ArrowUpRight size={13} />
        </a>
      </footer>
    </>
  );
}
function App() {
  const install = /^\/install\/?$/.test(window.location.pathname);
  return (
    <>
      <a className="skip" href="#main">
        跳至正文
      </a>
      <Header />
      <main id="main">
        {install ? (
          <Install />
        ) : (
          <>
            <HeroScene />
            <VolumeScene />
            <RouteScene />
            <RoutingScene />
            <PrivacyScene />
            <MemoryScene />
            <WorkflowScene />
            <DownloadSection />
            <QuickInstall />
            <FAQ />
          </>
        )}
      </main>
      <Footer />
    </>
  );
}
createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
