// The web page (web.h serves it at /): the PC's controls and a file manager with drag and drop, drawn like a DOS file
// manager's blue panels. Self-contained but for its pixel font (VT323, from Google Fonts when the browser is online).
#pragma once

static const char WEB_PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Tab5 PC</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=VT323&display=swap">
<style>
/* A DOS file manager's blue panels, drawn at today's resolution: double-line frames of two 2 px rules, a cyan menu
   bar, grey dialog boxes, a function-key bar. One look, light or dark. (VT323 if the browser can fetch it, else the
   system's monospace face.) */
:root{--deep:#06104f;--blue:#0c1d9a;--rule:#e9e6d6;--ink:#e9e6d6;--file:#61e2e8;--hi:#2fbcc6;--yellow:#fff35c;
  --grey:#b9b7ad;--black:#04060e;--bad:#ff7a6e;--pix:"VT323","Lucida Console",Consolas,"DejaVu Sans Mono",monospace;
  color-scheme:dark}
*{box-sizing:border-box}
html,body{background:var(--deep)}
body{margin:0;color:var(--ink);font:22px/1.2 var(--pix);
  background-image:repeating-linear-gradient(to bottom,transparent 0 3px,rgb(0 0 0/.16) 3px 4px)}
.menu{background:var(--hi);color:var(--black);padding-block:4px;
  padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
.menu .in{max-width:1000px;margin:0 auto;display:flex;gap:6px 18px;align-items:center;flex-wrap:wrap}
h1{font:inherit;font-size:30px;margin:0;letter-spacing:.06em}
#state{font-size:22px;padding:0 8px;background:var(--black);color:var(--hi);text-transform:uppercase;letter-spacing:.08em}
#state.run{color:#7dff8a}#state.pause{color:var(--yellow)}
.grow{flex:1}
main{max-width:1000px;margin:0 auto;padding-block:18px 48px;display:flex;flex-direction:column;gap:22px;
  padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
button{font:inherit;color:inherit;background:none;border:0;padding:0 4px;cursor:pointer}
button:focus-visible{outline:2px solid var(--yellow);outline-offset:2px}
button:disabled{opacity:.45;cursor:default}
.menu button{color:var(--black);padding:0 6px}
.menu button:hover:not(:disabled){background:var(--black);color:var(--hi)}
.menu button.primary{background:var(--black);color:var(--yellow)}
.menu button.danger{color:#6a0d06}
.box{position:relative;background:var(--blue);border:6px double var(--rule);padding:22px 16px 14px;min-width:0}
.box>.cap{position:absolute;top:-17px;left:50%;transform:translateX(-50%);background:var(--blue);padding:0 10px;
  white-space:nowrap;color:var(--ink);max-width:calc(100% - 40px);overflow:hidden;text-overflow:ellipsis}
.dialog{display:none;background:var(--grey);color:var(--black);border:6px double var(--black);outline:2px solid var(--grey);
  padding:14px 18px;box-shadow:12px 12px 0 rgb(0 0 0/.55)}
.dialog.show{display:block}
.dialog b{color:#6a0d06;font-weight:normal;text-transform:uppercase;letter-spacing:.05em}
.dialog code{font:inherit;background:rgb(0 0 0/.12);padding:0 4px}
.dialog .row{margin-top:10px}
.btn{background:var(--black);color:var(--grey);padding:0 8px}
.btn.primary{color:var(--yellow)}
.btn:hover{background:#2a2a2a}
.row{display:flex;gap:10px 22px;align-items:center;flex-wrap:wrap}
.levels{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,380px),1fr));gap:12px 34px}
label.slider{display:grid;grid-template-columns:7.5em 1fr 3em;gap:12px;align-items:center;color:var(--ink)}
label.slider output{color:var(--yellow);text-align:right;font-variant-numeric:tabular-nums}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:20px;margin:0;cursor:pointer;
  background:repeating-linear-gradient(to right,var(--deep) 0 8px,transparent 8px 10px);border:2px solid var(--rule)}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:26px;background:var(--yellow);border:2px solid var(--black)}
input[type=range]::-moz-range-thumb{width:14px;height:26px;background:var(--yellow);border:2px solid var(--black);border-radius:0}
input[type=range]:focus-visible{outline:2px solid var(--yellow);outline-offset:3px}
.drives{display:flex;gap:6px 10px;flex-wrap:wrap;align-items:center;margin-bottom:8px}
.drives::before{content:"Drive";color:var(--yellow);margin-right:4px}
#tabs{display:contents}
#tabs button{color:var(--file);padding:0 6px}
#tabs button.on{background:var(--hi);color:var(--black)}
#tabs button:hover:not(.on){background:var(--deep)}
.crumbs{display:flex;gap:0 2px;align-items:center;flex-wrap:wrap;color:var(--ink);margin-bottom:8px;min-width:0}
.crumbs a{color:var(--ink);cursor:pointer;text-decoration:none;padding:0 3px}
.crumbs a:hover{background:var(--hi);color:var(--black)}
.tablewrap{overflow-x:auto;border-top:2px solid var(--rule);border-bottom:2px solid var(--rule)}
table{width:100%;border-collapse:collapse}
th{color:var(--yellow);font-weight:normal;text-align:left;padding:2px 8px;border-bottom:2px solid var(--rule)}
th.num{text-align:right}
td{padding:1px 8px;white-space:nowrap;color:var(--file)}
td+td,th+th{border-left:2px solid var(--rule)}
td.name{width:100%;white-space:normal;word-break:break-all}
td.num{text-align:right;font-variant-numeric:tabular-nums}
tr.dir td{color:var(--ink)}
tr.dir td.name a{color:var(--ink)}
td.name a{color:inherit;text-decoration:none;cursor:pointer}
tbody tr:hover td{background:var(--hi);color:var(--black)}
tbody tr:hover td a,tbody tr:hover .act button{color:var(--black)}
.act button{color:var(--file);font-size:20px}
.act button::before{content:"["}.act button::after{content:"]"}
.act button:hover{background:var(--black);color:var(--yellow)!important}
.act button.danger{color:var(--bad)}
.empty{color:var(--grey);padding:10px 8px}
#prog{display:none;margin:6px 0}#prog.show{display:block}
.bar{height:16px;border:2px solid var(--rule);margin-top:4px;background:var(--deep)}
.bar i{display:block;height:100%;width:0;background:repeating-linear-gradient(to right,var(--yellow) 0 10px,transparent 10px 12px)}
.dim{color:var(--grey)}.small{font-size:19px}
#hint{margin:8px 0 0;color:var(--grey);font-size:19px;line-height:1.15}
.fkeys{display:flex;flex-wrap:wrap;gap:6px 14px;margin-top:12px}
.fkeys button{display:flex;padding:0;background:var(--hi);color:var(--black)}
.fkeys button kbd{font:inherit;background:var(--black);color:var(--ink);padding:0 5px}
.fkeys button span{padding:0 8px}
.fkeys button:hover span{background:var(--yellow)}
#editor{position:fixed;inset:0;background:rgb(6 16 79/.82);display:none;z-index:8;
  padding:max(16px,env(safe-area-inset-top,0px)) max(16px,env(safe-area-inset-right,0px)) max(16px,env(safe-area-inset-bottom,0px)) max(16px,env(safe-area-inset-left,0px))}
#editor.show{display:flex;align-items:center;justify-content:center}
#editor .box{width:min(960px,100%);height:min(720px,100%);display:flex;flex-direction:column;gap:10px}
#edtext{flex:1;width:100%;resize:none;font:21px/1.15 var(--pix);color:var(--file);background:var(--deep);border:2px solid var(--rule);
  padding:8px 10px;tab-size:8;outline:none}
#edtext:focus{border-color:var(--yellow)}
#drop{position:fixed;inset:0;background:rgb(6 16 79/.86);border:10px double var(--yellow);display:none;align-items:center;
  justify-content:center;text-align:center;font-size:34px;color:var(--yellow);z-index:9;padding:24px}
#drop.show{display:flex}
#editor .row button{color:var(--file);font-size:20px}
#editor .row button:hover{background:var(--black);color:var(--yellow)}
@media (max-width:520px){body{font-size:20px}label.slider{grid-template-columns:6em 1fr 2.6em}}
@media (prefers-reduced-motion:reduce){*{transition:none!important}}
</style></head><body>
<div class="menu"><div class="in"><h1>TAB5 PC</h1><span id="state">…</span><span class="grow"></span>
<button id="pausebtn"></button><button class="danger" id="resetbtn">Reset PC</button></div></div>
<main>
<div class="dialog" id="stopped"><b>The PC is stopped.</b> Its disk images can be deleted or replaced now, and
<code>tiny386.ini</code> changed. <b>Start PC</b> restarts the Tab5, which reads <code>tiny386.ini</code> again.</div>

<div class="dialog" id="banner"><b>The PC is paused</b> while its disk <span id="opendisk"></span> is open here.
<span id="bannermore"></span><div class="row"><button class="btn primary" id="donebtn">[ Done ]</button>
<span class="small" id="donehint"></span></div></div>

<section class="box"><span class="cap">Sound &amp; screen</span><div class="levels">
<label class="slider">Volume <input type="range" id="vol" min="0" max="255"><output id="volv"></output></label>
<label class="slider">Brightness <input type="range" id="bri" min="8" max="255"><output id="briv"></output></label></div></section>

<section class="box" id="files"><span class="cap" id="cap">Files</span>
<div class="drives"><div id="tabs"></div></div>
<div class="crumbs" id="crumbs"></div>
<div id="prog"><span id="progtext" class="small"></span><div class="bar"><i id="progbar"></i></div></div>
<div class="tablewrap"><table id="list"></table></div>
<p id="hint">Drop files or folders anywhere on this page to copy them here.</p>
<div class="fkeys"><button id="upfiles"><kbd>F5</kbd><span>Upload files</span></button><button id="updir"><kbd>F6</kbd><span>Upload folder</span></button>
<button id="mkdirbtn"><kbd>F7</kbd><span>New folder</span></button></div>
</section>
<input type="file" id="fileinput" multiple hidden><input type="file" id="dirinput" webkitdirectory hidden>
</main><div id="editor"><div class="box"><span class="cap" id="edname"></span><div class="row"><span class="small dim" id="edinfo"></span>
<span class="grow"></span><button class="act" id="edcancel">[Esc Cancel]</button><button class="act" id="edsave">[Ctrl+S Save]</button></div>
<textarea id="edtext" spellcheck="false" autocomplete="off"></textarea></div></div><div id="drop">Drop to copy into<br><span id="droppath"></span></div>
<script>
const $=id=>document.getElementById(id);
let st={}, vol='', path='/', busy=false, dragging=0, sliding=null, restarting=false;
const api=(m,u)=>fetch(u,{method:m}).then(async r=>{const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||r.statusText);return j});
const q=(o)=>Object.entries(o).map(([k,v])=>k+'='+encodeURIComponent(v)).join('&');
const fmt=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB';
const isHd=v=>v.startsWith('hd');
const letter=v=>{const i=(st.disks||[]).findIndex(d=>d.vol==v);return i<0?'':String.fromCharCode(67+i)+':'};
const volName=v=>v=='sd'?'SD card':(letter(v)+' ('+v+')');
const dos83=n=>/^[^ .]{1,8}(\.[^ .]{1,3})?$/.test(n);

async function status(){
  try{st=await api('GET','/api/status')}catch(e){$('state').textContent=restarting?'restarting…':'offline';return}
  if(restarting&&st.pc!='stopped'){restarting=false;list();return}
  const s=$('state');s.textContent=restarting?'starting':st.pc;s.className=st.pc=='running'?'run':'pause';
  const stopped=st.pc=='stopped';$('stopped').classList.toggle('show',stopped);
  $('pausebtn').textContent=stopped?'Start PC':st.pc=='paused'?'Resume':'Pause';$('pausebtn').disabled=!!st.open||restarting;
  $('pausebtn').className=stopped?'primary':'';$('resetbtn').style.display=stopped?'none':'';
  if(sliding!='vol')$('vol').value=st.volume;if(sliding!='bri')$('bri').value=st.brightness;
  $('volv').value=$('vol').value;$('briv').value=$('bri').value;
  const b=$('banner');b.classList.toggle('show',!!st.open);
  if(st.open){$('opendisk').textContent=volName(st.open);
    $('bannermore').textContent=st.changed?'When you are done, it restarts, so DOS reads the disk afresh.':'Nothing has been changed yet: it will simply continue.';
    $('donehint').textContent='(or by itself '+st.idle_close_s+' s after the last file operation)';}
  const vols=(st.disks||[]).map(d=>d.vol).concat(['sd']);
  if(!vols.includes(vol))vol=st.open||'sd';   // the card first: opening a PC disk pauses the PC
  $('tabs').innerHTML='';vols.forEach(v=>{const t=document.createElement('button');t.textContent=volName(v);t.className=v==vol?'on':'';
    if(isHd(v)&&st.open!=v)t.title='Opening the PC\u2019s disk pauses the PC until you are done';
    t.onclick=()=>{vol=v;path='/';list()};$('tabs').appendChild(t)});
}
async function list(){
  const tb=$('list');
  $('crumbs').innerHTML='';const parts=path.split('/').filter(x=>x);let acc='';
  const crumb=(txt,p)=>{const a=document.createElement('a');a.textContent=txt;a.onclick=()=>{path=p;list()};$('crumbs').appendChild(a)};
  crumb(volName(vol),'/');parts.forEach(x=>{acc+='/'+x;$('crumbs').append(' / ');crumb(x,acc)});
  $('droppath').textContent=volName(vol)+path;$('cap').textContent=volName(vol)+' '+path;
  $('hint').textContent=isHd(vol)?'Drop files or folders anywhere on this page to copy them here. Opening the PC’s disk pauses the PC; DOS sees names longer than 8.3 shortened (LONGNA~1.TXT).':'Drop files or folders anywhere on this page to copy them to the SD card (disk images, CD-ROM .iso files, tiny386.ini).';
  let items;
  try{items=await api('GET','/api/ls?'+q({vol,path}))}catch(e){tb.innerHTML='<tr><td class="empty">'+e.message+'</td></tr>';status();return}
  items.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n));
  tb.innerHTML='';tb.createTHead().innerHTML='<tr><th>Name</th><th class="num">Size</th><th></th></tr>';
  if(path!='/'){const tr=tb.insertRow();tr.className='dir';const c=tr.insertCell();c.className='name';
    const a=document.createElement('a');a.textContent='..';a.onclick=()=>{path=path.slice(0,path.lastIndexOf('/'))||'/';list()};c.appendChild(a);
    tr.insertCell().outerHTML='<td class="num">UP--DIR</td>';tr.insertCell()}
  if(!items.length){const tr=tb.insertRow();tr.innerHTML='<td class="empty" colspan="3">No files here yet</td>'}
  for(const it of items){
    const p=(path=='/'?'':path)+'/'+it.n, tr=tb.insertRow();tr.className=it.d?'dir':'';
    const c=tr.insertCell();c.className='name';const a=document.createElement('a');
    a.textContent=it.n+(it.busy?'  (in use by the PC)':'');
    if(it.d)a.onclick=()=>{path=p;list()};else if(!it.busy){a.href='/api/file?'+q({vol,path:p});a.download=it.n}
    c.appendChild(a);
    tr.insertCell().outerHTML='<td class="num">'+(it.d?'SUB-DIR':fmt(it.s))+'</td>';
    const ac=tr.insertCell();ac.className='act';
    if(!it.d&&!it.busy&&isText(it.n)&&it.s<=EDIT_MAX){const ed=document.createElement('button');ed.textContent='Edit';
      ed.onclick=()=>edit(p,it.n);ac.append(ed,' ')}
    const del=document.createElement('button');del.textContent='Delete';del.className='danger';
    del.onclick=async()=>{
      if(it.busy){if(!confirm('The PC has '+it.n+' open. Stop the PC and delete it?\n\nLike switching the PC off: anything unsaved in DOS is lost. '+
          'It stays stopped until you start it again (Start PC), so you can delete or replace its other files and change tiny386.ini first.'))return}
      else if(!confirm('Delete '+it.n+(it.d?' and everything in it':'')+'?'))return;
      try{if(it.busy)await api('POST','/api/pc?do=stop');await api('DELETE','/api/file?'+q({vol,path:p}))}catch(e){alert(e.message)}
      list();status()};
    ac.appendChild(del)
  }
  status();
}
const CHUNK=256<<10;   // the Tab5 takes files in chunks of up to 256 KB (web.h)
function putChunk(p,offset,blob,done,size){return new Promise((res,rej)=>{const x=new XMLHttpRequest();x.open('PUT','/api/file?'+q(offset?{vol,path:p,offset}:{vol,path:p,offset,size}));
  x.upload.onprogress=e=>done(e.loaded);x.onload=()=>x.status<300?res():rej(new Error((JSON.parse(x.responseText||'{}').error)||x.statusText));
  x.onerror=()=>rej(new Error('network error'));x.send(blob)})}
async function put(p,file,done){let off=0;do{const end=Math.min(off+CHUNK,file.size);await putChunk(p,off,file.slice(off,end),l=>done(off+l),file.size);off=end}while(off<file.size)}
async function upload(files){   // [{rel, file}]
  if(busy||!files.length)return;busy=true;
  const total=files.reduce((s,f)=>s+f.file.size,0);let sent=0;$('prog').classList.add('show');
  const long=isHd(vol)?files.filter(f=>f.rel.split('/').some(n=>!dos83(n))).length:0;
  try{for(const [i,f] of files.entries()){
    const p=(path=='/'?'':path)+'/'+f.rel;
    $('progtext').textContent='Copying '+(i+1)+' of '+files.length+': '+f.rel;
    await put(p,f.file,l=>{$('progbar').style.width=(100*(sent+l)/Math.max(total,1))+'%'});sent+=f.file.size}
    $('progtext').textContent='Copied '+files.length+' file'+(files.length>1?'s':'')+' ('+fmt(total)+')'+(long?'. '+long+' with names DOS will see shortened.':'.');
  }catch(e){$('progtext').textContent='Stopped: '+e.message}
  busy=false;list();
}
async function entries(item,prefix,out){   // walk a dropped folder
  if(item.isFile){await new Promise(r=>item.file(f=>{out.push({rel:prefix+f.name,file:f});r()},r))}
  else if(item.isDirectory){const rd=item.createReader();let batch;
    do{batch=await new Promise(r=>rd.readEntries(r,()=>r([])));for(const e of batch)await entries(e,prefix+item.name+'/',out)}while(batch.length)}
}
addEventListener('dragenter',e=>{e.preventDefault();dragging++;$('drop').classList.add('show')});
addEventListener('dragleave',e=>{if(--dragging<=0){dragging=0;$('drop').classList.remove('show')}});
addEventListener('dragover',e=>e.preventDefault());
addEventListener('drop',async e=>{e.preventDefault();dragging=0;$('drop').classList.remove('show');
  const out=[],its=[...e.dataTransfer.items].map(i=>i.webkitGetAsEntry&&i.webkitGetAsEntry()).filter(x=>x);
  if(its.length){for(const it of its)await entries(it,'',out)}else{for(const f of e.dataTransfer.files)out.push({rel:f.name,file:f})}
  upload(out)});
$('upfiles').onclick=()=>$('fileinput').click();$('updir').onclick=()=>$('dirinput').click();
$('fileinput').onchange=e=>{upload([...e.target.files].map(f=>({rel:f.name,file:f})));e.target.value=''};
$('dirinput').onchange=e=>{upload([...e.target.files].map(f=>({rel:f.webkitRelativePath||f.name,file:f})));e.target.value=''};
$('mkdirbtn').onclick=async()=>{const n=prompt('New folder name'+(isHd(vol)?' (DOS: up to 8 letters)':''));if(!n)return;
  try{await api('POST','/api/mkdir?'+q({vol,path:(path=='/'?'':path)+'/'+n}))}catch(e){alert(e.message)}list()};
$('pausebtn').onclick=async()=>{if(st.pc=='stopped'){restart();return}
  await api('POST','/api/pc?do='+(st.pc=='paused'?'resume':'pause')).catch(e=>alert(e.message));status()};
async function restart(){   // the whole Tab5: it reads tiny386.ini again (back in ~10 s; WiFi reconnects)
  restarting=true;$('pausebtn').disabled=true;$('state').textContent='restarting…';
  await api('POST','/api/pc?do=restart').catch(()=>{})}
// The text editor (tiny386.ini, DOS's batch and config files): UTF-8 if the file is, else one byte per character
// (DOS's code page), and the file's own line ends (CR LF for DOS) kept.
const EDIT_MAX=CHUNK, isText=n=>/\.(ini|txt|bat|cfg|conf|md|log|lst|nfo)$/i.test(n)||/^(fd)?config\.sys$/i.test(n);
let edPath='', edCrlf=false, edUtf8=true;
async function edit(p,name){
  let bytes;try{const r=await fetch('/api/file?'+q({vol,path:p}));if(!r.ok)throw new Error((await r.json().catch(()=>({}))).error||r.statusText);
    bytes=new Uint8Array(await r.arrayBuffer())}catch(e){alert(e.message);return}
  let text;try{text=new TextDecoder('utf-8',{fatal:true}).decode(bytes);edUtf8=true}catch(e){text=Array.from(bytes,b=>String.fromCharCode(b)).join('');edUtf8=false}
  edCrlf=text.includes('\r\n');edPath=p;
  $('edname').textContent=name;$('edinfo').textContent=volName(vol)+' · '+(edCrlf?'CR LF':'LF')+(edUtf8?'':' · one byte per character');
  $('edtext').value=text.replace(/\r\n/g,'\n');$('editor').classList.add('show');$('edtext').focus()}
const edClose=()=>{$('editor').classList.remove('show');edPath=''};
$('edcancel').onclick=edClose;
addEventListener('keydown',e=>{
  if(edPath){if(e.key=='Escape')edClose();else if(e.key=='s'&&(e.ctrlKey||e.metaKey)){e.preventDefault();$('edsave').click()}return}
  const k={F5:'upfiles',F6:'updir',F7:'mkdirbtn'}[e.key];   // (the function-key bar's keys)
  if(k&&!e.ctrlKey&&!e.altKey&&!e.metaKey){e.preventDefault();$(k).click()}});
$('edsave').onclick=async()=>{
  let text=$('edtext').value;if(edCrlf)text=text.replace(/\n/g,'\r\n');
  const bytes=edUtf8?new TextEncoder().encode(text):Uint8Array.from(text,c=>c.charCodeAt(0)&255);
  if(bytes.length>CHUNK){alert('Too long to save here (up to 256 KB)');return}
  try{await putChunk(edPath,0,new Blob([bytes]),()=>{},bytes.length)}catch(e){alert('Not saved: '+e.message);return}
  const ini=vol=='sd'&&/^\/tiny386\.ini$/i.test(edPath);edClose();list();
  if(ini&&st.pc!='stopped'&&confirm('Saved. Restart the Tab5 now, so the PC starts with the new tiny386.ini?\n\nLike switching the PC off: anything unsaved in DOS is lost.'))restart()};
$('resetbtn').onclick=async()=>{if(!confirm('Reset the PC? Anything unsaved in DOS is lost.'))return;await api('POST','/api/pc?do=reset').catch(e=>alert(e.message));status()};
$('donebtn').onclick=async()=>{await api('POST','/api/pc?do=done').catch(e=>alert(e.message));status()};
for(const [id,key] of [['vol','volume'],['bri','brightness']]){const el=$(id);
  el.oninput=()=>{sliding=id;$(id+'v').value=el.value;clearTimeout(el.t);el.t=setTimeout(()=>api('POST','/api/set?'+key+'='+el.value),120)};
  el.onchange=()=>{sliding=null;api('POST','/api/set?'+key+'='+el.value)}}
// The Tab5's clock and time zone from this browser's (clock.h: screenshots' names, file dates), once per visit. The
// zone as a POSIX TZ string, as the Tab5's C library reads it: the offsets and this year's changes found here.
function posixTZ(){
  const y=new Date().getFullYear(),east=t=>-new Date(t).getTimezoneOffset();   // minutes east of UTC at t (ms)
  const hm=m=>Math.floor(m/60)+(m%60?':'+String(m%60).padStart(2,'0'):'');
  const off=e=>(e>0?'-':'')+hm(Math.abs(e));   // POSIX counts hours west of UTC
  const t0=Date.UTC(y,0,1),t1=Date.UTC(y+1,0,1),tr=[];let prev=east(t0);
  for(let t=t0+3600e3;t<=t1;t+=3600e3){const e=east(t);if(e==prev)continue;
    let lo=t-3600e3,hi=t;while(hi-lo>60e3){const mid=lo+Math.floor((hi-lo)/120e3)*60e3;if(east(mid)==prev)lo=mid;else hi=mid}
    tr.push({t:hi,from:prev,to:e});prev=e}
  if(tr.length!=2)return 'LOC'+off(east(Date.now()));   // no daylight saving (or not a plain pair of changes)
  const std=Math.min(tr[0].from,tr[0].to),dst=Math.max(tr[0].from,tr[0].to);
  const rule=x=>{const d=new Date(x.t+x.from*60e3);   // the local time it happened, in the offset before
    const m=d.getUTCMonth()+1,day=d.getUTCDate(),dim=new Date(Date.UTC(d.getUTCFullYear(),m,0)).getUTCDate();
    return 'M'+m+'.'+(day+7>dim?5:Math.ceil(day/7))+'.'+d.getUTCDay()+'/'+hm(d.getUTCHours()*60+d.getUTCMinutes())};
  return 'LOC'+off(std)+'LDT'+off(dst)+','+rule(tr.find(x=>x.to==dst))+','+rule(tr.find(x=>x.to==std))}
api('POST','/api/clock?'+q({utc:Math.round(Date.now()/1000),tz:posixTZ()})).catch(()=>{});
status().then(list);setInterval(()=>{if(!busy)status()},2000);
</script></body></html>)HTML";
