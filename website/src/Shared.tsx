import { useState } from "react";
import { ArrowDown, Download, GitBranch, Menu, X } from "lucide-react";
import { release } from "./release";
export const sizeMB = (release.sizeBytes / 1048576).toFixed(1);
export const SOURCE_URL = "https://github.com/FfszHy/Sona";
export function Mark({ small = false }: { small?: boolean }) {
  return (
    <span className={`mark ${small ? "small" : ""}`} aria-hidden="true">
      {[12, 23, 33, 23, 12].map((h, i) => (
        <i key={i} style={{ height: h }} />
      ))}
    </span>
  );
}
export function DownloadLink({ secondary = false }: { secondary?: boolean }) {
  return release.downloadUrl ? (
    <a
      className={`button ${secondary ? "secondary" : ""}`}
      href={release.downloadUrl}
      download={release.fileName}
    >
      <Download size={17} />
      下载 Sona for Mac
      <ArrowDown className="button-arrow" size={16} />
    </a>
  ) : (
    <button className="button" disabled>
      安装包准备中
    </button>
  );
}
export function Header() {
  const [open, setOpen] = useState(false);
  return (
    <header className="header wrap">
      <a className="brand" href="/" aria-label="Sona 首页">
        <Mark small />
        sona.
      </a>
      <button
        className="mobile-menu"
        aria-label={open ? "关闭菜单" : "打开菜单"}
        aria-expanded={open}
        aria-controls="navigation"
        onClick={() => setOpen(!open)}
      >
        {open ? <X /> : <Menu />}
      </button>
      <nav
        id="navigation"
        className={open ? "open" : ""}
        aria-label="主导航"
        onClick={() => setOpen(false)}
      >
        <a href="/#features">产品演示</a>
        <a href="/install">安装说明</a>
        <a href="/#faq">常见问题</a>
        <a
          className="nav-source"
          href={SOURCE_URL}
          target="_blank"
          rel="noreferrer"
        >
          <GitBranch size={14} />
          GitHub
        </a>
        <a className="nav-download" href="/#download">
          获取 Sona <ArrowDown size={14} />
        </a>
      </nav>
    </header>
  );
}
