const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');
// Keep generated previews separate from the checked-in production assets.
const root = path.resolve(__dirname, '..', 'build', 'icons');
const cfg = JSON.parse(fs.readFileSync(path.join(__dirname,'mark.json'),'utf8'));
// The source uses only absolute M/C/Z commands. Expand x once to set the width.
let coordinate = 0;
const d = cfg.path.replace(/-?\d+(?:\.\d+)?/g, value => {
  const number = Number(value) * ((coordinate++ % 2) === 0 ? cfg.xExpansion : 1);
  return String(Math.round(number * 10000) / 10000);
});
const hash = crypto.createHash('sha256').update(d).digest('hex');
const transform = data => `translate(${data.translate.join(' ')}) scale(${data.scale})`;
const shape = (attributes='') => `<path data-sona-contour="${hash}" d="${d}" ${attributes}/>`;
const svg = (w,h,body,extra='') => `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" ${extra}>${body}</svg>`;
const appTransform = transform(cfg.app);
const menuTransform = transform(cfg.menu);
const appDefs = `<defs>
 <linearGradient id="tile" x1=".14" y1="0" x2=".86" y2="1"><stop stop-color="#234773"/><stop offset=".47" stop-color="#10294b"/><stop offset="1" stop-color="#081629"/></linearGradient>
 <radialGradient id="ambience" cx=".12" cy=".03" r="1"><stop stop-color="#72b8ed" stop-opacity=".33"/><stop offset=".7" stop-color="#246394" stop-opacity="0"/></radialGradient>
 <linearGradient id="tile-edge" x1=".1" y1="0" x2=".85" y2="1"><stop stop-color="#b8ddff" stop-opacity=".65"/><stop offset=".35" stop-color="#81b7e0" stop-opacity=".2"/><stop offset="1" stop-color="#040c19" stop-opacity=".8"/></linearGradient>
 <linearGradient id="pearl" x1=".25" y1="0" x2=".75" y2="1"><stop stop-color="#fff"/><stop offset=".22" stop-color="#f9fcff"/><stop offset=".4" stop-color="#c4d9ec"/><stop offset=".48" stop-color="#96b4d1"/><stop offset=".55" stop-color="#f3faff"/><stop offset=".7" stop-color="#fff"/><stop offset=".9" stop-color="#c4dced"/><stop offset="1" stop-color="#a5bfd8"/></linearGradient>
 <linearGradient id="pearl-edge" x1="0" y1="0" x2=".7" y2="1"><stop stop-color="#fff"/><stop offset=".45" stop-color="#fff" stop-opacity=".25"/><stop offset="1" stop-color="#638aaf" stop-opacity=".55"/></linearGradient>
 <linearGradient id="edge-shade" x1=".1" y1="0" x2=".8" y2="1"><stop stop-color="#173e68" stop-opacity="0"/><stop offset=".48" stop-color="#173e68" stop-opacity=".12"/><stop offset="1" stop-color="#173e68" stop-opacity=".65"/></linearGradient>
 <linearGradient id="edge-light" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#fff" stop-opacity="1"/><stop offset=".5" stop-color="#fff" stop-opacity=".4"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
 <filter id="bevel" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation=".85"/></filter>
 <clipPath id="mark-clip">${shape()}</clipPath>
</defs>`;
const appBody = `${appDefs}<rect x="84" y="84" width="856" height="856" rx="194" fill="url(#tile)"/>
<rect x="84" y="84" width="856" height="856" rx="194" fill="url(#ambience)"/>
<rect x="85" y="85" width="854" height="854" rx="193" fill="none" stroke="url(#tile-edge)" stroke-width="2"/>
<g transform="${appTransform}">${shape('fill="url(#pearl)"')}<g clip-path="url(#mark-clip)">
${shape('fill="none" stroke="url(#edge-shade)" stroke-width="4.8" filter="url(#bevel)"')}
${shape('fill="none" stroke="url(#edge-light)" stroke-width="3.8" filter="url(#bevel)"')}
${shape('fill="none" stroke="url(#pearl-edge)" stroke-width=".2"')}
</g></g>`;
const appSVG = svg(1024,1024,appBody);
const markSVG = `<svg xmlns="http://www.w3.org/2000/svg" width="112" height="104" viewBox="0 0 112 104"><title>Sona Flow S — shared contour v2</title>${shape('fill="#000"')}</svg>`;
const menu = (opacity=1, muted=false) => svg(22,16,
  `<g transform="${menuTransform}" opacity="${opacity}">${shape('fill="#000"')}</g>`+
  (muted?'<path d="M4.1 1.6L18 14.2" fill="none" stroke="#000" stroke-width="1.5" stroke-linecap="round"/>':''));
const states=[['template',1,false],['idle',.82,false],['playing-0',.9,false],['playing-1',1,false],['playing-2',.94,false],['muted',.42,true]];
async function main(){
 for(const directory of ['app', 'menu-bar']) fs.mkdirSync(path.join(root, directory), {recursive:true});
 fs.writeFileSync(path.join(root,'sona-mark.svg'),markSVG);
 fs.writeFileSync(path.join(root,'app','Sona-AppIcon.svg'),appSVG);
 await sharp(Buffer.from(appSVG)).png().toFile(path.join(root,'app','Sona-AppIcon-1024.png'));
 const iconset=path.join(root,'app','Sona.iconset'); fs.mkdirSync(iconset,{recursive:true});
 const input=Buffer.from(appSVG);
 for(const size of [16,32,128,256,512]) for(const density of [1,2]){
  await sharp(input).resize(size*density,size*density).png().toFile(path.join(iconset,`icon_${size}x${size}${density===2?'@2x':''}.png`));
 }
 for(const [state,opacity,muted] of states){
  const content=menu(opacity,muted);
  fs.writeFileSync(path.join(root,'menu-bar',`${state}.svg`),content);
  for(const density of [1,2]) await sharp(Buffer.from(content),{density:72*density}).resize(22*density,16*density).png().toFile(path.join(root,'menu-bar',`${state}@${density}x.png`));
 }
 const contourProof=[];
 for(const file of [path.join(root,'sona-mark.svg'),path.join(root,'app','Sona-AppIcon.svg'),...states.map(([state])=>path.join(root,'menu-bar',`${state}.svg`))]){
  const content=fs.readFileSync(file,'utf8');
  const paths=[...content.matchAll(/<path data-sona-contour="[^"]+" d="([^"]+)"/g)].map(match=>match[1]);
  if(!paths.length||paths.some(value=>value!==d)) throw new Error('Contour drift: '+file);
  contourProof.push({file:path.relative(root,file),pathCount:paths.length,contourSha256:hash});
 }
 fs.writeFileSync(path.join(root,'contour-verification.json'),JSON.stringify({contourSha256:hash,contourProof,note:'All symbol paths are identical. App shading is clipped to this path. Menu states vary opacity only; muted adds an independent slash.'},null,2)+'\n');
 console.log(JSON.stringify({contourSha256:hash,root,files:contourProof.length}));
}
main().catch(error=>{console.error(error);process.exit(1);});
