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

`package.json` 与 `package-lock.json` 固定了依赖版本。本机 `node_modules` 目前是指向另一个项目的符号链接（已忽略）；在其他机器或 Vercel 上 `npm install` / `npm ci` 会按 lock 文件安装独立依赖。
