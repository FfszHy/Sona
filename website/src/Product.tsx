import {
  useEffect,
  useRef,
  useState,
  type ReactNode,
  type CSSProperties,
} from "react";
import {
  AudioLines,
  BatteryFull,
  Check,
  ChevronDown,
  Headphones,
  Laptop,
  MousePointer2,
  Settings,
  Speaker,
  Video,
  Volume1,
  Volume2,
  Wifi,
} from "lucide-react";
export type AppName = "music" | "safari" | "zoom" | "spotify";
export type Device = "mac" | "airpods" | "display";
export const appNames = {
  music: "网易云音乐",
  safari: "Safari 浏览器",
  zoom: "Zoom",
  spotify: "Spotify",
};
export const deviceNames = {
  mac: "MacBook Air 扬声器",
  airpods: "AirPods Pro",
  display: "Studio Display 扬声器",
};
export function AppIcon({ name }: { name: AppName }) {
  if (name === "music" || name === "safari")
    return (
      <img
        className="app-icon"
        src={`/media/${name}.png`}
        width="28"
        height="28"
        alt=""
      />
    );
  return (
    <span className={`app-icon ${name}`}>
      {name === "zoom" ? (
        <Video size={19} fill="white" />
      ) : (
        <svg viewBox="0 0 28 28" aria-hidden="true">
          <path
            d="M6 10 Q14 6 22 11 M7 14 Q14 11 21 15 M8 18 Q14 15 20 19"
            fill="none"
            stroke="currentColor"
            strokeWidth="2"
            strokeLinecap="round"
          />
        </svg>
      )}
    </span>
  );
}
export function DeviceIcon({
  device,
  size = 15,
}: {
  device: Device;
  size?: number;
}) {
  return device === "mac" ? (
    <Laptop size={size} />
  ) : device === "airpods" ? (
    <Headphones size={size} />
  ) : (
    <Speaker size={size} />
  );
}
export interface AppRow {
  name: AppName;
  volume: number;
  targets: Device[];
  active?: boolean;
}
export function Panel({
  apps,
  menu,
  settings = false,
  className = "",
}: {
  apps: AppRow[];
  menu?: AppName;
  settings?: boolean;
  className?: string;
}) {
  const hasDisplay = apps.some((app) => app.targets.includes("display"));
  const selected = apps.find((a) => a.name === menu)?.targets ?? ["mac"];
  return (
    <div className={`sona-panel ${className}`} aria-hidden="true">
      <div className="panel-header">
        <img src="/media/sona.png" width="36" height="36" alt="" />
        <div>
          <strong>Sona</strong>
          <span>Sound, your way.</span>
        </div>
        <Settings size={19} />
      </div>
      <div
        className={`glass-card outputs ${hasDisplay ? "outputs-extra" : ""}`}
      >
        <div className="panel-label">输出设备</div>
        <div className="output-title">
          <Laptop size={15} />
          <b>MacBook Air 扬声器</b>
          <span>默认</span>
        </div>
        <Level value={48} targets={["mac"]} />
        <div className="output-title">
          <Headphones size={15} />
          <b>AirPods Pro</b>
        </div>
        <Level value={67} targets={["airpods"]} />
        {hasDisplay && (
          <>
            <div className="output-title">
              <Speaker size={15} />
              <b>Studio Display 扬声器</b>
            </div>
            <Level value={72} targets={["display"]} />
          </>
        )}
      </div>
      <div className="apps-label">
        <b>应用</b>
        <span>{apps.filter((a) => a.active !== false).length} 个正在播放</span>
      </div>
      {apps.map((app) => (
        <div
          className={`glass-card app-row ${menu === app.name ? "selected" : ""}`}
          data-app={app.name}
          key={app.name}
        >
          <div className="app-title">
            <AppIcon name={app.name} />
            <b>{appNames[app.name]}</b>
            <span>
              <i className={app.active === false ? "idle" : ""} />
              {app.active === false ? "空闲" : "正在播放"}
            </span>
          </div>
          <Level value={app.volume} targets={app.targets} />
          {menu === app.name && (
            <div className="device-menu">
              <span className="menu-label">默认输出</span>
              <div className="device-option">
                <Laptop size={14} />
                MacBook Air 扬声器
              </div>
              <hr />
              <span className="menu-label">自定义输出</span>
              {(["mac", "airpods", "display"] as Device[]).map((d) => (
                <div
                  className={`device-option ${selected.includes(d) ? "checked" : ""}`}
                  key={d}
                >
                  <DeviceIcon device={d} />
                  <span>{deviceNames[d]}</span>
                  {selected.includes(d) && <Check size={14} />}
                </div>
              ))}
              <hr />
              <span className="menu-foot">可以选多个设备同时输出</span>
            </div>
          )}
        </div>
      ))}
      {settings && (
        <div className="settings-menu">
          <span>
            <Check size={13} />
            登录时启动
          </span>
          <span>
            <Check size={13} />
            新设备接入时自动切换
          </span>
          <hr />
          <span>退出 Sona</span>
        </div>
      )}
    </div>
  );
}
function Level({ value, targets }: { value: number; targets: Device[] }) {
  return (
    <div className="volume-row">
      <Volume1 size={14} />
      <div className="slider-track">
        <i style={{ width: `${value}%` }} />
        <span style={{ left: `${value}%` }} />
      </div>
      <span className="volume-value">{Math.round(value)}%</span>
      <span className="mute-control">
        <Volume2 size={13} />
      </span>
      <span className="route-control">
        {targets.length > 1 ? (
          <Speaker size={13} />
        ) : (
          <DeviceIcon device={targets[0]} />
        )}
        <ChevronDown size={8} />
      </span>
    </div>
  );
}
export function Fit({
  children,
  width = 700,
  height = 560,
  className = "",
  style,
}: {
  children: ReactNode;
  width?: number;
  height?: number;
  className?: string;
  style?: CSSProperties;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const [scale, setScale] = useState(1);
  useEffect(() => {
    const observer = new ResizeObserver(([e]) =>
      setScale(e.contentRect.width / width),
    );
    if (ref.current) observer.observe(ref.current);
    return () => observer.disconnect();
  }, [width]);
  return (
    <div
      ref={ref}
      className={`fit ${className}`}
      style={{ aspectRatio: `${width}/${height}`, ...style }}
    >
      <div
        className="fit-content"
        style={{ width, height, transform: `scale(${scale})` }}
      >
        {children}
      </div>
    </div>
  );
}
export function Cursor({
  style,
  click = false,
}: {
  style: CSSProperties;
  click?: boolean;
}) {
  return (
    <div
      className={`demo-cursor ${click ? "click" : ""}`}
      style={style}
      aria-hidden="true"
    >
      <MousePointer2
        size={25}
        fill="white"
        stroke="#172131"
        strokeWidth={1.3}
      />
      <span />
    </div>
  );
}
export function MenuBar() {
  return (
    <div className="mac-menubar" aria-hidden="true">
      <span className="mac-apple">●</span>
      <b>Finder</b>
      <span>文件</span>
      <span>编辑</span>
      <span>显示</span>
      <div className="menubar-right">
        <span className="sona-tray">
          <AudioLines size={17} />
        </span>
        <Wifi size={16} />
        <BatteryFull size={20} />
        <span>周二 09:41</span>
      </div>
    </div>
  );
}
export function Desktop({
  children,
  className = "",
}: {
  children: ReactNode;
  className?: string;
}) {
  return (
    <div className={`desktop ${className}`}>
      <div className="wallpaper">
        <i />
        <i />
        <i />
      </div>
      <MenuBar />
      {children}
    </div>
  );
}
