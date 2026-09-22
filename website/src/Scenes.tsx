import {
  ArrowDown,
  ArrowUpRight,
  AudioLines,
  Check,
  Headphones,
  MicOff,
  Pause,
  Play,
  RotateCcw,
  ShieldCheck,
  CircleOff,
} from "lucide-react";
import {
  AppIcon,
  Cursor,
  Desktop,
  DeviceIcon,
  Fit,
  MenuBar,
  Panel,
  type AppRow,
  type Device,
} from "./Product";
import { DownloadLink } from "./Shared";
import { release } from "./release";
import { mix, point, range, smooth, useScene } from "./motion";
import hero from "./assets/hero.jpg";
import type { CSSProperties } from "react";

function Controls({ scene }: { scene: ReturnType<typeof useScene> }) {
  return (
    <div className="scene-controls">
      <span className="scene-progress">
        <i style={{ transform: `scaleX(${scene.progress})` }} />
      </span>
      {!scene.reduced && (
        <button
          onClick={scene.play}
          aria-label={scene.playing ? "暂停演示" : "播放十秒演示"}
        >
          {scene.playing ? <Pause size={13} /> : <RotateCcw size={13} />}
          <span>{scene.playing ? "暂停" : "播放演示"}</span>
        </button>
      )}
    </div>
  );
}
export function HeroScene() {
  const s = useScene();
  const p = s.reduced ? 0 : s.progress,
    z = smooth(range(p, 0, 0.28));
  const apps: AppRow[] = [
    { name: "safari", volume: 34, targets: p > 0.63 ? ["airpods"] : ["mac"] },
    {
      name: "music",
      volume: mix(66, 30, range(p, 0.32, 0.48)),
      targets: p > 0.88 ? ["mac", "airpods"] : ["mac"],
    },
  ];
  const menu = p > 0.53 && p < 0.72 ? "safari" : p > 0.76 ? "music" : undefined;
  const bg = Math.round(mix(245, 17, z));
  return (
    <section
      className="hero-scene scene"
      ref={s.ref}
      aria-label="Sona 产品演示：音乐音量从 66% 调到 30%，Safari 输出到耳机，网易云音乐同时输出到两个设备"
    >
      <div
        className="scene-sticky hero-sticky"
        style={{
          backgroundColor: `rgb(${bg},${Math.round(mix(245, 19, z))},${Math.round(mix(240, 24, z))})`,
        }}
      >
        <div className="hero-layout wrap">
          <div
            className="hero-copy"
            style={{
              opacity: 1 - range(p, 0, 0.16),
              transform: `translateY(${-40 * z}px)`,
              visibility: p > 0.2 ? "hidden" : "visible",
            }}
          >
            <div className="eyebrow">
              <span className="live-dot" />为 Mac 而生的音频控制工具
            </div>
            <h1>
              让声音，
              <br />
              <span>各就各位。</span>
            </h1>
            <p>
              音乐轻一点，会议清楚一点。
              <br />
              每个应用的音量和去向，都由你决定。
            </p>
            <div className="hero-actions">
              <DownloadLink />
              <a className="text-link" href="/install">
                安装说明 <ArrowUpRight size={15} />
              </a>
            </div>
            <div className="requirements">
              macOS {release.minMacOS}+ <span>·</span> Apple Silicon{" "}
              <span>·</span> 预览版 {release.version}
            </div>
          </div>
          <div
            className="hero-visual"
            style={
              { "--zoom": z, "--space": range(p, 0.26, 0.43) } as CSSProperties
            }
          >
            <Fit width={900} height={675} className="hero-desktop">
              <Desktop>
                <div
                  className="hero-panel"
                  style={{ opacity: range(p, 0.1, 0.2) }}
                >
                  <Panel apps={apps} menu={menu} />
                </div>
                <img
                  className="hero-photo"
                  src={hero}
                  width="900"
                  height="675"
                  alt="Sona 真实 Mac 界面"
                  fetchPriority="high"
                  style={{ opacity: 1 - range(p, 0.1, 0.2) }}
                />
                <div
                  className="desktop-dim"
                  style={{ opacity: range(p, 0.21, 0.32) * 0.65 }}
                />
                {p > 0.3 && (
                  <Cursor
                    style={{
                      ...point(p, [
                        [0.3, 660, 620],
                        [0.32, 409, 608],
                        [0.48, 341, 608],
                        [0.54, 607, 491],
                        [0.63, 755, 400],
                        [0.71, 607, 491],
                        [0.77, 607, 608],
                        [0.88, 755, 516],
                        [0.95, 800, 645],
                      ]),
                      opacity: 1 - range(p, 0.96, 1),
                    }}
                    click={(p > 0.6 && p < 0.65) || (p > 0.86 && p < 0.9)}
                  />
                )}
              </Desktop>
            </Fit>
          </div>
          <div
            className="hero-scroll"
            style={{ opacity: 1 - range(p, 0, 0.14) }}
          >
            <ArrowDown size={15} />
            <span>向下滚动，让声音各就各位</span>
          </div>
          <div
            className="hero-controls"
            style={{
              opacity: range(p, 0.26, 0.32),
              visibility: p > 0.26 ? "visible" : "hidden",
            }}
          >
            <Controls scene={s} />
          </div>
        </div>
      </div>
    </section>
  );
}
export function VolumeScene() {
  const s = useScene(),
    p = s.progress;
  const apps: AppRow[] = [
    {
      name: "music",
      volume: mix(70, 25, smooth(range(p, 0.12, 0.7))),
      targets: ["mac"],
    },
    { name: "zoom", volume: 90, targets: ["airpods"] },
  ];
  return (
    <section
      className="feature-scene scene volume-scene"
      ref={s.ref}
      id="features"
    >
      <div className="scene-sticky">
        <div className="demo-layout wrap">
          <div className="demo-copy">
            <span className="chapter">
              01 <i /> 独立音量
            </span>
            <h2>
              背景音乐，
              <br />
              就让它待在背景。
            </h2>
          </div>
          <div
            className="demo-visual"
            role="group"
            aria-label="网易云音乐从 70% 降到 25%，Zoom 始终保持 90%"
          >
            <Fit width={560} height={540}>
              <div className="feature-panel">
                <Panel apps={apps} />
              </div>
              <Cursor
                style={{
                  left: mix(262, 189, smooth(range(p, 0.12, 0.7))),
                  top: 397,
                  opacity: p > 0.05 && p < 0.85 ? 1 : 0,
                }}
                click={p > 0.12 && p < 0.7}
              />
            </Fit>
            <Controls scene={s} />
          </div>
        </div>
      </div>
    </section>
  );
}
export function RouteScene() {
  const s = useScene(),
    p = s.progress;
  const apps: AppRow[] = [
    { name: "zoom", volume: 90, targets: p > 0.3 ? ["airpods"] : ["mac"] },
    {
      name: "music",
      volume: 25,
      targets: p > 0.77 ? ["mac", "display"] : ["mac"],
    },
  ];
  return (
    <section className="feature-scene scene route-scene" ref={s.ref}>
      <div className="scene-sticky">
        <div className="demo-layout wrap">
          <div className="demo-copy">
            <span className="chapter">
              02 <i /> 自由分流
            </span>
            <h2>
              每个声音，
              <br />
              都有自己的去向。
            </h2>
          </div>
          <div
            className="demo-visual"
            role="group"
            aria-label="Zoom 切到 AirPods，网易云音乐同时输出到 MacBook 和 Studio Display"
          >
            <Fit>
              <div
                className="feature-panel routing-panel"
                style={{
                  transform: `translateX(${-50 * smooth(range(p, 0, 0.2))}px)`,
                }}
              >
                <Panel apps={apps} menu={p < 0.48 ? "zoom" : "music"} />
              </div>
              <Cursor
                style={{
                  ...point(p, [
                    [0, 451, 375],
                    [0.2, 396, 375],
                    [0.3, 517, 300],
                    [0.42, 396, 375],
                    [0.5, 396, 471],
                    [0.65, 517, 365],
                    [0.77, 517, 427],
                    [1, 630, 550],
                  ]),
                  opacity: p < 0.92 ? 1 : 0,
                }}
                click={(p > 0.27 && p < 0.34) || (p > 0.74 && p < 0.81)}
              />
            </Fit>
            <div className="route-result">
              <span>
                <Headphones size={13} />
                Zoom → {p > 0.3 ? "AirPods" : "MacBook"}
              </span>
              <span>
                <AudioLines size={13} />
                网易云 → {p > 0.77 ? "2 个输出设备" : "MacBook"}
              </span>
            </div>
            <Controls scene={s} />
          </div>
        </div>
      </div>
    </section>
  );
}
export function RoutingScene() {
  const s = useScene(),
    p = s.progress,
    collapse = smooth(range(p, 0.65, 0.96));
  const apps: AppRow[] = [
    { name: "safari", volume: 34, targets: ["mac"] },
    { name: "music", volume: 25, targets: ["mac", "display"] },
    { name: "zoom", volume: 90, targets: ["airpods"] },
    { name: "spotify", volume: 25, targets: ["display"] },
  ];
  const nodes = [
    { name: "safari" as const, x: 65, y: 58 },
    { name: "music" as const, x: 35, y: 172 },
    { name: "zoom" as const, x: 76, y: 298 },
    { name: "spotify" as const, x: 38, y: 420 },
  ];
  return (
    <section className="routing-scene scene" ref={s.ref}>
      <div className="scene-sticky">
        <div className="routing-heading">
          <span className="chapter">一个应用，也可以不止一个目的地。</span>
          <h2>声音，各就各位。</h2>
        </div>
        <div
          className="routing-canvas"
          role="group"
          aria-label="Safari、网易云音乐、Zoom 和 Spotify 的声音分别流向耳机、MacBook 与显示器扬声器，最终收进 Sona 面板"
        >
          <Fit width={900} height={550}>
            <div
              className="route-map"
              style={{
                opacity: 1 - collapse,
                transform: `translateY(${collapse * -30}px) scale(${1 - collapse * 0.35})`,
              }}
            >
              <svg className="sound-paths" viewBox="0 0 900 550">
                <defs>
                  <linearGradient id="flow-color">
                    <stop stopColor="#7596ff" />
                    <stop offset="1" stopColor="#4273ed" />
                  </linearGradient>
                </defs>
                {[
                  "M 155 95 C 365 95 365 275 715 275",
                  "M 125 210 C 410 210 420 275 715 275",
                  "M 125 210 C 425 210 420 435 715 435",
                  "M 166 335 C 405 335 440 115 715 115",
                  "M 128 457 C 435 457 450 435 715 435",
                ].map((d, i) => (
                  <g key={d}>
                    <path d={d} className="path-base" />
                    <path
                      d={d}
                      className="path-active"
                      pathLength={1}
                      style={{
                        strokeDasharray: 1,
                        strokeDashoffset:
                          1 - range(p, 0.06 + i * 0.055, 0.35 + i * 0.055),
                      }}
                    />
                    <circle
                      r="3"
                      fill="#a6bcff"
                      style={{ opacity: p > 0.22 && p < 0.68 ? 1 : 0 }}
                    >
                      <animateMotion
                        dur={`${2.4 + i * 0.3}s`}
                        repeatCount="indefinite"
                        path={d}
                      />
                    </circle>
                  </g>
                ))}
              </svg>
              {nodes.map((n) => (
                <div
                  className="routing-app"
                  key={n.name}
                  style={{ left: n.x, top: n.y }}
                >
                  <AppIcon name={n.name} />
                  <span>
                    {n.name === "music"
                      ? "网易云音乐"
                      : n.name === "safari"
                        ? "Safari"
                        : n.name === "zoom"
                          ? "Zoom"
                          : "Spotify"}
                  </span>
                </div>
              ))}
              {(["airpods", "mac", "display"] as Device[]).map((d, i) => (
                <div
                  className="routing-device"
                  key={d}
                  style={{
                    left: 710,
                    top: 75 + i * 160,
                    opacity: mix(0.4, 1, range(p, 0.12, 0.4)),
                  }}
                >
                  <DeviceIcon device={d} size={33} />
                  <span>
                    {d === "airpods"
                      ? "AirPods"
                      : d === "mac"
                        ? "MacBook"
                        : "Studio Display"}
                  </span>
                </div>
              ))}
            </div>
            <div
              className="map-panel"
              style={{
                opacity: collapse,
                transform: `translate(-50%,-50%) scale(${mix(0.8, 1, collapse)})`,
              }}
            >
              <Panel apps={apps} />
            </div>
          </Fit>
        </div>
      </div>
    </section>
  );
}
export function PrivacyScene() {
  const s = useScene(),
    p = s.progress;
  return (
    <section className="privacy-scene scene" ref={s.ref}>
      <div className="scene-sticky">
        <div className="privacy-layout wrap">
          <div className="demo-copy">
            <span className="chapter">
              03 <i /> 安心使用
            </span>
            <h2>
              只管声音，
              <br />
              不碰你的麦克风。
            </h2>
          </div>
          <div
            className="privacy-demo"
            role="group"
            aria-label="Sona 只处理音频输出，不申请麦克风或系统录音权限"
          >
            <div className="privacy-signal">
              <AppIcon name="music" />
              <span className="wave-bars">
                {Array.from({ length: 24 }, (_, i) => (
                  <i
                    key={i}
                    style={{
                      height: 8 + Math.abs(Math.sin(i * 0.9 + p * 8)) * 24,
                      opacity: mix(0.3, 1, range(p, 0, 0.5)),
                    }}
                  />
                ))}
              </span>
              <img src="/media/sona.png" width="64" height="64" alt="" />
              <span className="wave-bars">
                {Array.from({ length: 12 }, (_, i) => (
                  <i
                    key={i}
                    style={{
                      height: 8 + Math.abs(Math.sin(i * 0.9 + p * 8)) * 16,
                    }}
                  />
                ))}
              </span>
              <Headphones size={29} />
            </div>
            <div className="privacy-permissions">
              <span>
                <MicOff size={17} />
                麦克风 <b>不申请</b>
              </span>
              <span>
                <CircleOff size={17} />
                系统录音 <b>不申请</b>
              </span>
            </div>
            <span className="privacy-status">
              <ShieldCheck size={14} />
              只经过音频输出链路
            </span>
          </div>
        </div>
      </div>
    </section>
  );
}
export function MemoryScene() {
  const s = useScene(),
    p = s.progress;
  const closed = range(p, 0.25, 0.4) * (1 - range(p, 0.58, 0.73));
  const apps: AppRow[] = [
    { name: "music", volume: 25, targets: ["mac", "display"] },
    { name: "zoom", volume: 90, targets: ["airpods"] },
  ];
  return (
    <section className="memory-scene scene" ref={s.ref}>
      <div className="scene-sticky">
        <div className="demo-layout wrap">
          <div className="demo-copy">
            <span className="chapter">
              04 <i /> 恰到好处
            </span>
            <h2>
              记住你的偏好，
              <br />
              回到小小菜单栏。
            </h2>
          </div>
          <div
            className="demo-visual"
            role="group"
            aria-label="关闭并重新打开 Sona，网易云 25%、Zoom 90% 及设备选择保持不变"
          >
            <Fit>
              <div className="memory-menubar">
                <MenuBar />
              </div>
              <div
                className="memory-panel"
                style={{
                  opacity: 1 - closed,
                  transform: `scale(${1 - closed * 0.88}) translateY(${-closed * 40}px)`,
                }}
              >
                <Panel apps={apps} settings={p > 0.8} />
              </div>
              <Cursor
                style={{
                  ...point(p, [
                    [0, 570, 500],
                    [0.25, 590, 135],
                    [0.42, 620, 130],
                    [0.58, 525, 46],
                    [0.73, 500, 82],
                    [1, 474, 100],
                  ]),
                }}
                click={p > 0.58 && p < 0.66}
              />
              <div
                className="remembered"
                style={{ opacity: range(p, 0.76, 0.87) }}
              >
                <Check size={13} />
                25% / 90% · 输出偏好已保留
              </div>
            </Fit>
            <Controls scene={s} />
          </div>
        </div>
      </div>
    </section>
  );
}
export function WorkflowScene() {
  const s = useScene(),
    p = s.reduced ? 0.78 : s.progress;
  const open = range(p, 0.12, 0.2) * (1 - range(p, 0.83, 0.91));
  const apps: AppRow[] =
    p < 0.28
      ? []
      : [
          {
            name: "music",
            volume: mix(70, 25, smooth(range(p, 0.35, 0.5))),
            targets: p > 0.72 ? ["airpods"] : ["mac"],
          },
        ];
  return (
    <section className="workflow-scene scene" id="how-it-works" ref={s.ref}>
      <div className="scene-sticky">
        <div className="workflow-heading">
          <span className="chapter">日常，就这么简单。</span>
          <h2 style={{ opacity: s.reduced ? 1 : range(p, 0.86, 0.98) }}>
            调整一下，回到专注。
          </h2>
        </div>
        <div
          className="workflow-desktop"
          role="group"
          aria-label="完整使用流程：打开 Sona，音乐出现，调低音量，选择 AirPods，最后收回菜单栏"
        >
          <Fit width={900} height={590}>
            <Desktop>
              <div className="focus-clock" style={{ opacity: 1 - open * 0.8 }}>
                09:41<span>留一点安静，给眼前的事。</span>
              </div>
              <div
                className="workflow-panel"
                style={{
                  opacity: open,
                  transform: `scale(${mix(0.8, 1, open)})`,
                }}
              >
                <Panel
                  apps={apps}
                  menu={p > 0.55 && p < 0.81 ? "music" : undefined}
                />
                {apps.length === 0 && (
                  <div className="empty-demo">还没有应用在播放声音</div>
                )}
              </div>
              <Cursor
                style={{
                  ...point(p, [
                    [0, 460, 300],
                    [0.12, 739, 18],
                    [0.23, 720, 100],
                    [0.35, 555, 369],
                    [0.5, 491, 369],
                    [0.56, 697, 369],
                    [0.72, 807, 301],
                    [0.8, 807, 301],
                    [0.85, 340, 300],
                    [1, 280, 340],
                  ]),
                  opacity: 1 - range(p, 0.87, 0.95),
                }}
                click={(p > 0.12 && p < 0.18) || (p > 0.7 && p < 0.75)}
              />
            </Desktop>
          </Fit>
        </div>
        <div className="workflow-controls wrap">
          <span>
            打开 <i /> 调音量 <i /> 分流 <i /> 收回
          </span>
          <Controls scene={s} />
        </div>
      </div>
    </section>
  );
}
