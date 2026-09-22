# Sona 官网

独立的 React + TypeScript + Vite 官网，不参与原生 App 构建。部署在 Vercel。

## 本地运行

```sh
cd website
npm install
npm run dev
```

## 构建与部署

```sh
npm run build          # tsc + vite，输出到 dist/
npm run preview        # 本地预览 dist/
npx vercel --prod      # 部署到 Vercel（首次先 npx vercel login，再 npx vercel link）
```

`vercel.json` 声明了 Vite 构建、`/download` → 当前安装包的跳转，以及 `.pkg` 的下载头。也可以在 Vercel 控制台把仓库连上，Root Directory 填 `website`，之后每次推送自动部署。

## 安装包与下载信息

- `public/downloads/` 存放当前版本的 `Sona-<版本>.pkg` 和 `.sha256`，由仓库根目录的 `make release` 生成并复制到这里。它随网站一起部署，因此 CLI 部署和 Git 部署都能提供下载。
- `src/release.ts` 由 `make release` 重写，是页面上版本号、下载地址、大小、校验和、是否公证、最低 macOS 的唯一来源。`notarized` 为 false 时页面会展示「仍要打开」的允许步骤，为 true 时自动省略。
- 源码仓库公开后，在 `make release` 前设置 `SONA_SOURCE_URL`（写入页面链接）；安装包改放 GitHub Release 时设置 `SONA_DOWNLOAD_BASE`。

首屏是真实界面截图（`src/assets/hero.jpg`，由 `pic/官网演示图.png` 用 `sips -s format jpeg -s formatOptions 92` 转出）；换图时保持 4:3 左右的比例即可。网页字体使用 Google Fonts，加载失败会回退到本机无衬线字体。

## 依赖

`package.json` 与 `package-lock.json` 固定了依赖版本。`node_modules` 使用独立依赖目录（已忽略）；在其他机器或 Vercel 上运行 `npm ci` 即可复现。

## 产品演示与安装页

首页使用 GSAP ScrollTrigger 驱动原生滚动，CSS sticky 固定每个演示场景。`Scenes.tsx` 定义各幕，`Product.tsx` 复用 Sona 面板、设备菜单和桌面，`motion.ts` 负责进度、十秒播放/暂停与清理。向上滚动可倒放；播放期间再次滚动会交回滚动控制。系统开启「减少动态效果」时移除滚动固定并展示静态结果。

首屏保留原有真实截图，后续演示是依据 `App/Sources/Sona/MenuContentView.swift` 重建的网页 UI，不是实机录屏，也不会改变访问者的系统音量。Sona、Safari、网易云图标来自项目与本机应用资源，桌面背景用 CSS 绘制；不包含个人桌面内容。演示覆盖音量、单/多设备输出、权限边界及偏好恢复。

`install.html` 是独立的 Vite 构建入口，Vercel 将 `/install` 映射到该页面。完整安装、排错、校验和、卸载和源码指南保存在 `Install.tsx`；首页只保留三步入口，FAQ 默认全部折叠。

依赖现在独立安装在 `website/node_modules`，不再链接其他项目。
