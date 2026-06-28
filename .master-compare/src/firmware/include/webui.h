#ifndef WEBUI_H
#define WEBUI_H
#include <Arduino.h>
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no,maximum-scale=1">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="default">
<meta name="apple-mobile-web-app-title" content="IR遥控">
<title>IR 遥控器</title>
<style>
:root{--bg:#f2f2f7;--card:#fff;--blue:#007AFF;--green:#34C759;--red:#FF3B30;
--orange:#FF9500;--purple:#AF52DE;--teal:#5AC8FA;--pink:#FF2D55;
--gray:#8e8e93;--gray3:#c7c7cc;--gray4:#d1d1d6;--gray5:#e5e5ea;--gray6:#f2f2f7;
--text:#1c1c1e;--text2:#3c3c43;--text3:#636366;--r:14px;--max:560px}
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{font-family:-apple-system,BlinkMacSystemFont,"SF Pro Text","Helvetica Neue",sans-serif;
background:var(--bg);color:var(--text);min-height:100vh;min-height:100dvh;
-webkit-font-smoothing:antialiased}
.hdr{position:sticky;top:0;z-index:50;background:rgba(242,242,247,.92);
backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);
padding:12px 20px 10px;border-bottom:.5px solid var(--gray5);text-align:center}
.hdr h1{font-size:17px;font-weight:600}
nav.bar{position:fixed;bottom:0;left:0;right:0;background:rgba(249,249,249,.94);
backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);
border-top:.5px solid var(--gray4);display:flex;z-index:100;
padding-bottom:env(safe-area-inset-bottom)}
body{padding-bottom:80px}
nav.bar a{flex:1;display:flex;flex-direction:column;align-items:center;padding:6px 0 4px;
color:var(--gray);text-decoration:none;font-size:10px;font-weight:500;transition:color .15s;cursor:pointer}
nav.bar a.active{color:var(--blue)}
nav.bar a i{font-size:22px;line-height:1.2;margin-bottom:1px;font-style:normal}
.pg{display:none;padding:16px;max-width:var(--max);margin:0 auto}
.pg.active{display:block}
.pg-title{font-size:28px;font-weight:700;letter-spacing:-.5px;margin-bottom:14px}
.pg-sub{font-size:13px;color:var(--gray);margin-top:-10px;margin-bottom:14px}
.cd{background:var(--card);border-radius:var(--r);box-shadow:0 .5px 2px rgba(0,0,0,.06);margin-bottom:12px;overflow:hidden}
.cd-h{padding:10px 16px 4px;font-size:12px;text-transform:uppercase;color:var(--gray);font-weight:700;letter-spacing:.5px}
.cd-b{padding:8px 16px 12px}
.it{display:flex;align-items:center;justify-content:space-between;padding:11px 16px;min-height:44px;cursor:pointer}
.it+.it{border-top:.5px solid var(--gray6)}
.it span{font-size:16px}
.it .arr{color:var(--gray3)}
.it .arr::after{content:'';display:inline-block;width:7px;height:7px;
border-right:1.5px solid var(--gray3);border-bottom:1.5px solid var(--gray3);
transform:rotate(-45deg);margin-left:6px}
.b{display:inline-flex;align-items:center;justify-content:center;gap:5px;padding:11px 20px;
border-radius:12px;font-size:16px;font-weight:600;border:none;cursor:pointer;
transition:opacity .12s,transform .08s;-webkit-user-select:none;user-select:none}
.b:active{opacity:.55;transform:scale(.96)}
.b-bl{background:var(--blue);color:#fff}
.b-gn{background:var(--green);color:#fff}
.b-rd{background:var(--red);color:#fff}
.b-or{background:var(--orange);color:#fff}
.b-pp{background:var(--purple);color:#fff}
.b-ol{background:transparent;color:var(--blue);border:1.5px solid var(--blue)}
.b-gy{background:var(--gray5);color:var(--text)}
.b-fw{width:100%}
.seg{display:flex;background:var(--gray6);border-radius:10px;padding:2px;margin:6px 0}
.seg button{flex:1;padding:7px 2px;border:none;background:transparent;
font-size:13px;font-weight:500;color:var(--text3);border-radius:8px;cursor:pointer;transition:all .2s}
.seg button.active{background:#fff;color:var(--text);box-shadow:0 1px 3px rgba(0,0,0,.08)}
.temp{display:flex;align-items:center;justify-content:center;gap:20px;padding:12px 0}
.temp-v{font-size:64px;font-weight:200;letter-spacing:-3px;min-width:100px;text-align:center}
.temp-u{font-size:22px;font-weight:300;color:var(--gray)}
.temp-b{width:50px;height:50px;border-radius:50%;border:none;font-size:22px;font-weight:300;
cursor:pointer;background:var(--gray6);color:var(--text);transition:transform .08s}
.temp-b:active{transform:scale(.9)}
select{width:100%;padding:11px 16px;border-radius:10px;border:.5px solid var(--gray4);
font-size:16px;background:var(--card);color:var(--text);appearance:none;
background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12'%3E%3Cpath d='M2 4l4 4 4-4' fill='none' stroke='%238e8e93' stroke-width='1.5'/%3E%3C/svg%3E");
background-repeat:no-repeat;background-position:right 14px center}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;padding:4px 0}
.grid-b{padding:13px 6px;border-radius:12px;border:1.5px solid var(--gray5);
background:var(--card);font-size:13px;font-weight:500;cursor:pointer;
display:flex;flex-direction:column;align-items:center;gap:3px;transition:all .12s}
.grid-b:active{background:var(--gray6);transform:scale(.95)}
.grid-b .ic{font-size:22px}
.toast{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%) scale(.8);
background:rgba(0,0,0,.78);color:#fff;padding:12px 24px;border-radius:14px;
font-size:15px;font-weight:500;z-index:999;opacity:0;transition:all .2s;pointer-events:none;
text-align:center;max-width:260px}
.toast.show{opacity:1;transform:translate(-50%,-50%) scale(1)}
input[type=text]{width:100%;padding:11px 14px;border-radius:10px;
border:.5px solid var(--gray4);font-size:16px;background:var(--card);color:var(--text);outline:none}
input:focus{border-color:var(--blue)}
.raw-box{background:var(--gray6);border-radius:8px;padding:10px;font-family:"SF Mono",Menlo,monospace;
font-size:12px;word-break:break-all;max-height:80px;overflow-y:auto;margin:8px 0;color:var(--text3)}
.learn-pulse{animation:lp 1.5s infinite}
@keyframes lp{0%,100%{box-shadow:0 0 0 0 rgba(255,59,48,.25)}50%{box-shadow:0 0 0 10px rgba(255,59,48,0)}}
.pair-pulse{animation:pp 1.2s infinite}
@keyframes pp{0%,100%{box-shadow:0 0 0 0 rgba(0,122,255,.3)}50%{box-shadow:0 0 0 12px rgba(0,122,255,0)}}
.slave-dot{width:8px;height:8px;border-radius:50%;background:#34C759;display:inline-block;margin-right:8px}
.slave-row{display:flex;align-items:center;padding:8px 0;font-size:13px;border-bottom:.5px solid var(--gray6)}
.slave-row:last-child{border-bottom:none}
.slave-mac{flex:1;color:var(--gray)}
.slave-ip{color:var(--gray);font-size:12px;margin-right:8px}
.hist-item{display:flex;align-items:center;padding:10px 16px;gap:10px}
.hist-item+.hist-item{border-top:.5px solid var(--gray6)}
.hist-info{flex:1;min-width:0}
.hist-proto{font-size:13px;font-weight:600}
.hist-bits{font-size:11px;color:var(--gray)}
.hist-raw{font-size:11px;color:var(--text3);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.empty{padding:30px 16px;text-align:center;color:var(--gray);font-size:14px}
.badge{display:inline-block;padding:2px 7px;border-radius:6px;font-size:11px;font-weight:600}
.badge-gy{background:var(--gray6);color:var(--gray)}
.modal-bg{position:fixed;inset:0;background:rgba(0,0,0,.4);z-index:200;display:none;align-items:center;justify-content:center}
.modal-bg.show{display:flex}
.modal{background:var(--card);border-radius:16px;padding:24px;width:90%;max-width:320px}
.modal h3{font-size:18px;font-weight:600;margin-bottom:14px;text-align:center}
.modal input{margin-bottom:14px}
.modal .btns{display:flex;gap:10px}
.modal .btns button{flex:1}
@media(min-width:768px){
body{background:#e8e8ed}
.hdr{background:rgba(242,242,247,.88)}
.pg{padding:20px}
.pg-title{font-size:34px;letter-spacing:-.8px}
nav.bar{max-width:var(--max);left:50%;transform:translateX(-50%);border-radius:var(--r) var(--r) 0 0}
}
</style>
</head>
<body>
<div class="hdr"><h1>IR 遥控器</h1></div>
<div id="toast" class="toast"></div>
<div id="rename-modal" class="modal-bg">
<div class="modal">
<h3>重命名</h3>
<input type="text" id="rename-input" placeholder="新名称">
<div class="btns">
<button class="b b-gy" onclick="closeRename()">取消</button>
<button class="b b-bl" onclick="doRename()">确定</button>
</div>
</div>
</div>
<div id="pg-ac" class="pg active">
<div class="pg-title">空调</div>
<div class="cd"><div class="cd-h">品牌</div><div class="cd-b">
<select id="ac-v">
<optgroup label="常用">
<option value="GREE">格力</option>
<option value="WAHIN">华凌</option>
<option value="MIDEA">美的</option>
</optgroup>
<optgroup label="国产主流">
<option value="GREE112">格力 (112bit)</option>
<option value="MIDEA24">美的 (24bit)</option>
<option value="TECO">TECO</option>
<option value="HAIER_AC">海尔</option>
<option value="HAIER_AC_YRWO2">海尔 (YRW02)</option>
<option value="KELON">科龙</option>
<option value="KELON168">科龙 (168bit)</option>
<option value="TCL112AC">TCL</option>
<option value="TCL96AC">TCL (96bit)</option>
</optgroup>
<optgroup label="日系">
<option value="DAIKIN">大金</option>
<option value="DAIKIN128">大金 (128bit)</option>
<option value="DAIKIN2">大金2</option>
<option value="TOSHIBA_AC">东芝</option>
<option value="MITSUBISHI_AC">三菱</option>
<option value="MITSUBISHI112">三菱 (112bit)</option>
<option value="MITSUBISHI_HEAVY_88">三菱重工 (88bit)</option>
<option value="MITSUBISHI_HEAVY_152">三菱重工 (152bit)</option>
<option value="PANASONIC_AC">松下</option>
<option value="FUJITSU_AC">富士通</option>
<option value="SHARP_AC">夏普</option>
<option value="SANYO_AC">三洋</option>
<option value="HITACHI_AC">日立</option>
</optgroup>
<optgroup label="韩系/其他">
<option value="SAMSUNG_AC">三星</option>
<option value="LG">LG</option>
<option value="LG2">LG2</option>
<option value="ELECTRA_AC">Electra</option>
<option value="WHIRLPOOL_AC">惠而浦</option>
<option value="BOSCH144">博世</option>
<option value="CARRIER_AC64">Carrier</option>
<option value="YORK_AC">York</option>
</optgroup>
<optgroup label="万能">
<option value="COOLIX">Coolix 万能</option>
</optgroup>
</select>
</div></div>
<div class="cd"><div class="cd-h">模式</div><div class="cd-b">
<div class="seg" id="s-mode">
<button class="active" data-v="Cool">&#10052;&#65039; 制冷</button>
<button data-v="Heat">&#128293; 制热</button>
<button data-v="Auto">&#128260; 自动</button>
<button data-v="Dry">&#128167; 除湿</button>
<button data-v="Fan">&#127744; 送风</button>
</div></div></div>
<div class="cd"><div class="cd-h">温度</div><div class="cd-b">
<div class="temp">
<button class="temp-b" onclick="tAdj(-1)">&#8722;</button>
<div><span class="temp-v" id="t-val">26</span><span class="temp-u">&#176;C</span></div>
<button class="temp-b" onclick="tAdj(1)">+</button>
</div></div></div>
<div class="cd"><div class="cd-h">风速</div><div class="cd-b">
<div class="seg" id="s-fan">
<button class="active" data-v="Auto">自动</button>
<button data-v="Low">低</button>
<button data-v="Medium">中</button>
<button data-v="High">高</button>
</div></div></div>
<div style="display:flex;gap:10px">
<button class="b b-bl b-fw" onclick="acCmd('On')">开机</button>
<button class="b b-rd b-fw" onclick="acCmd('Off')">关机</button>
</div>
</div>

<div id="pg-learn" class="pg">
<div class="pg-title">学习</div>
<div class="pg-sub">
<span class="badge badge-gy" id="learn-st">就绪</span>
</div>
<div class="cd" id="learn-card">
<div class="cd-h">捕获信号</div>
<div class="cd-b">
<div style="display:flex;gap:8px;margin-bottom:12px">
<button class="b b-rd b-fw" id="btn-rec" onclick="toggleRec()">&#9679; 开始学习</button>
</div>
<div id="rec-hint" style="display:none;font-size:14px;color:var(--text3);text-align:center;padding:8px">
&#128308; 正在监听... 把遥控器对着板子按键
</div>
<div id="latest-signal" style="display:none">
<div class="raw-box" id="raw-preview"></div>
<input type="text" id="sig-name" placeholder="命名，如：开机、音量+">
<div style="display:flex;gap:8px;margin-top:10px">
<button class="b b-gn" style="flex:1" onclick="saveSig()">保存</button>
<button class="b b-ol" style="flex:1" onclick="replayLatest()">&#9654; 重放</button>
</div>
</div>
</div></div>
<div class="cd"><div class="cd-h">捕获历史</div>
<div id="hist-list"><div class="empty">还没有捕获信号</div></div></div>
<button class="b b-gy b-fw" style="font-size:14px" onclick="clearHist()">清除历史</button>
</div>
<div id="pg-remote" class="pg">
<div class="pg-title">遥控器</div>
<div class="pg-sub">学习保存的按钮，点击发送</div>
<div id="remote-devices"><div class="empty">还没有保存的按钮<br>去「学习」捕获信号吧</div></div>
</div>
<div id="pg-settings" class="pg">
<div class="pg-title">设置</div>
<div class="cd"><div class="cd-h">WiFi</div><div class="cd-b">
<div id="wifi-status" style="font-size:14px;color:var(--text3);margin-bottom:10px"></div>
<button class="b b-bl b-fw" style="margin-bottom:10px" onclick="scanWifi()">&#128269; 扫描WiFi</button>
<div id="wifi-list" style="display:none">
<div style="max-height:200px;overflow-y:auto;border:.5px solid var(--gray4);border-radius:10px;margin-bottom:10px" id="wifi-scan-results"></div>
<input type="text" id="wifi-ssid" placeholder="WiFi 名称（SSID）" style="margin-bottom:8px">
<input type="text" id="wifi-pass" placeholder="WiFi 密码" style="margin-bottom:10px">
<button class="b b-gn b-fw" onclick="connectWifi()">连接并重启</button>
</div>
<div id="wifi-forget-wrap" style="display:none;margin-top:10px">
<button class="b b-rd b-fw" style="font-size:14px" onclick="forgetWifi()">忘记网络（切回AP模式）</button>
</div>
</div></div>
<div class="cd"><div class="cd-h">热点配置</div><div class="cd-b">
<div style="font-size:13px;color:var(--text3);margin-bottom:8px">
配置 AP 热点名称和密码。从机使用相同配置自动组网，恢复出厂后回到默认值。</div>
<input type="text" id="ap-ssid" placeholder="热点名称（默认 IR-AC）" style="margin-bottom:8px">
<input type="text" id="ap-pass" placeholder="热点密码（至少8位，默认 12345678）" style="margin-bottom:10px">
<button class="b b-bl b-fw" onclick="saveApConfig()">保存热点配置并重启</button>
</div></div>
<div class="cd"><div class="cd-h">MQTT</div><div class="cd-b">
<input type="text" id="mqtt-host" placeholder="服务器地址（如 192.168.1.100）" style="margin-bottom:8px">
<div style="display:flex;gap:8px;margin-bottom:8px">
<input type="text" id="mqtt-port" placeholder="端口" value="1883" style="flex:1">
<input type="text" id="mqtt-user" placeholder="用户名（可选）" style="flex:1">
</div>
<input type="text" id="mqtt-pass" placeholder="密码（可选）" style="margin-bottom:8px">
<input type="text" id="mqtt-topic" placeholder="Topic 前缀" value="ir_ac" style="margin-bottom:10px">
<button class="b b-pp b-fw" onclick="saveMqtt()">保存MQTT配置并重启</button>
</div></div>
<div class="cd"><div class="cd-h">数据</div>
<div class="it" onclick="exportAll()"><span>导出配置</span><span class="arr"></span></div>
<div class="it" onclick="importAll()"><span>导入配置</span><span class="arr"></span></div>
<div class="it" onclick="clearAll()"><span style="color:var(--red)">清除所有数据</span><span class="arr"></span></div>
<div class="it" onclick="factoryReset()"><span style="color:var(--red)">恢复出厂设置</span><span class="arr"></span></div>
</div>
<div class="cd" id="slaves-card" style="display:none"><div class="cd-h">从机设备</div>
<div id="slave-list" style="padding:8px 16px"><div style="color:var(--gray);font-size:13px">加载中...</div></div>
<div style="padding:0 16px 12px">
<button class="b b-or b-fw" id="pair-btn" onclick="togglePair()">配对新设备</button>
</div></div>
<div class="cd" id="devmode-card" style="display:none"><div class="cd-h">设备模式</div><div class="cd-b">
<div class="seg" id="s-devmode">
<button data-v="auto" class="active">自动</button>
<button data-v="ap">AP主机</button>
<button data-v="slave">STA从机</button>
<button data-v="home">Home</button>
</div>
<div style="font-size:13px;color:var(--text3);margin-top:8px">强制模式覆盖自动检测，保存后重启</div>
<button class="b b-bl b-fw" style="margin-top:10px" onclick="saveForceMode()">保存并重启</button>
</div></div>
<div class="cd"><div class="cd-h">关于</div>
<div class="it"><span>版本</span><span style="color:var(--gray)">v2.3</span></div>
<div class="it"><span>MAC</span><span id="sys-mac" style="color:var(--gray)">--</span></div>
<div class="it"><span>设备</span><span style="color:var(--gray)">IR Mini V105</span></div>
<div class="it"><span>协议库</span><span style="color:var(--gray)">IRremoteESP8266</span></div>
</div>
<div class="cd"><div class="cd-h">固件更新 (OTA)</div><div class="cd-b">
<div style="font-size:13px;color:var(--text3);margin-bottom:8px">
<div>当前分区: ROM <span id="ota-rom">-</span></div>
<div style="margin-top:4px">上传新固件将写入另一个分区，启动失败自动回退</div>
</div>
<form method="POST" action="/update" enctype="multipart/form-data" id="ota-form">
<input type="file" name="firmware" accept=".bin" style="margin:8px 0;font-size:14px" required>
<button type="submit" class="b b-or b-fw" id="ota-btn">上传并更新</button>
</form>
<div id="ota-msg" style="font-size:13px;margin-top:8px"></div>
</div></div></div>
<nav class="bar">
<a class="active" onclick="go('ac')"><i>&#10052;&#65039;</i>空调</a>
<a onclick="go('learn')"><i>&#128225;</i>学习</a>
<a onclick="go('remote')"><i>&#127903;</i>遥控</a>
<a onclick="go('settings')"><i>&#9881;</i>设置</a>
</nav>
<script>
var sigs=JSON.parse(localStorage.getItem('ir_sigs')||'[]');
var hist=[],recording=false,latestRaw='',latestProto='',latestBits=0,pollTimer=null;
var renameIdx=-1;
fetch('/api/ota/status').then(function(r){return r.json()}).then(function(d){var el=$('ota-rom');if(el)el.textContent=d.current_rom}).catch(function(){});
(function(){var f=$('ota-form');if(f)f.onsubmit=function(){var b=$('ota-btn');if(b)b.textContent='上传中...';var m=$('ota-msg');if(m)m.textContent='固件上传中，请勿断电。完成后设备自动重启。'}})();
function $(i){return document.getElementById(i)}
function toast(m){var t=$('toast');t.textContent=m;t.classList.add('show');setTimeout(function(){t.classList.remove('show')},1500)}
function go(p){
document.querySelectorAll('.pg').forEach(function(e){e.classList.remove('active')});
document.querySelectorAll('nav.bar a').forEach(function(e){e.classList.remove('active')});
$('pg-'+p).classList.add('active');
var t=['ac','learn','remote','settings'];
document.querySelectorAll('nav.bar a')[t.indexOf(p)].classList.add('active');
if(p==='learn')startPoll();else stopPoll();
if(p==='remote')renderRemote();
}
function segInit(id){
var bs=$(id).querySelectorAll('button');
bs.forEach(function(b){b.onclick=function(){bs.forEach(function(x){x.classList.remove('active')});b.classList.add('active');acCmd('On')}});
}
function segVal(id){return $(id).querySelector('.active').dataset.v}
segInit('s-mode');segInit('s-fan');
function tAdj(d){var e=$('t-val'),v=parseInt(e.textContent)+d;if(v<16)v=16;if(v>30)v=30;e.textContent=v;acCmd('On')}
function acCmd(pwr){
var p='vendor='+$('ac-v').value+'&power='+pwr+'&mode='+segVal('s-mode')+
'&temp='+$('t-val').textContent+'&fan='+segVal('s-fan')+'&swing=Off';
fetch('/api/hvac',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p})
.then(function(r){return r.json()}).then(function(d){toast(d.ok?'\u2713':'\u2717 失败')}).catch(function(){toast('\u2717 失败')});
}
var sv_=localStorage.getItem('ir_ac_v');if(sv_)$('ac-v').value=sv_;
$('ac-v').onchange=function(){localStorage.setItem('ir_ac_v',this.value)};
function startPoll(){
$('learn-st').textContent='监听中';
if(pollTimer)return;
pollTimer=setInterval(function(){
fetch('/api/capture').then(function(r){return r.json()}).then(function(d){
if(d.raw)onIR(d);
}).catch(function(){});
},500);
}
function stopPoll(){
if(pollTimer){clearInterval(pollTimer);pollTimer=null}
$('learn-st').textContent='已停止';
}
function onIR(d){
var raw=d.raw,proto=d.proto||'?',bits=d.bits||0;
if(!raw||raw===latestRaw)return;
latestRaw=raw;latestProto=proto;latestBits=bits;
hist.unshift({proto:proto,bits:bits,raw:raw,time:Date.now()});
if(hist.length>30)hist.length=30;
renderHist();
if(!recording)return;
$('latest-signal').style.display='block';
$('raw-preview').textContent=raw;
$('sig-name').value='';$('sig-name').focus();
}
function toggleRec(){
recording=!recording;
var b=$('btn-rec'),h=$('rec-hint'),ls=$('latest-signal');
if(recording){
b.textContent='\u25A0 停止';b.className='b b-gy b-fw';
h.style.display='block';$('learn-card').classList.add('learn-pulse');
startPoll();
}else{
b.textContent='\u25CF 开始学习';b.className='b b-rd b-fw';
h.style.display='none';ls.style.display='none';
$('learn-card').classList.remove('learn-pulse');
}
}
function saveSig(){
var name=$('sig-name').value.trim();
if(!name){toast('请命名');return}
if(!latestRaw){toast('没有信号');return}
sigs.push({name:name,raw:latestRaw,proto:latestProto,bits:latestBits});
localStorage.setItem('ir_sigs',JSON.stringify(sigs));
$('latest-signal').style.display='none';latestRaw='';
toast('\u2713 已保存：'+name);
}
function sendRaw(raw){
fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'raw='+encodeURIComponent(raw)})
.then(function(r){return r.json()}).then(function(d){toast(d.ok?'\u2713 已发送':'\u2717 失败')})
.catch(function(){toast('\u2717 失败')});
}
function replayLatest(){if(latestRaw)sendRaw(latestRaw)}
function renderHist(){
var el=$('hist-list');
if(!hist.length){el.innerHTML='<div class="empty">还没有捕获信号</div>';return}
var h='';
hist.forEach(function(s){
h+='<div class="hist-item"><div class="hist-info"><div class="hist-proto">'+s.proto+
' <span class="badge badge-gy">'+s.bits+'b</span></div><div class="hist-raw">'+s.raw.substring(0,50)+
'</div></div><button class="b b-ol" style="padding:6px 12px;font-size:13px" onclick="sendRaw(\''+
s.raw.replace(/'/g,"\\'")+'\')">&#9654;</button></div>';
});
el.innerHTML=h;
}
function clearHist(){hist=[];renderHist()}
function renderRemote(){
var el=$('remote-devices');
if(!sigs.length){el.innerHTML='<div class="empty">还没有保存的按钮<br>去「学习」捕获信号吧</div>';return}
var h='<div class="grid">';
sigs.forEach(function(s,i){
h+='<button class="grid-b" onclick="sendRaw(\''+
s.raw.replace(/'/g,"\\'")+'\')"><span class="ic">'+(s.icon||'\u26AB')+
'</span>'+s.name+'</button>';
});
h+='</div>';
h+='<div style="margin-top:14px;display:flex;gap:8px">';
h+='<button class="b b-ol b-fw" style="font-size:14px" onclick="editMode()">&#9998;&#65039; 编辑</button></div>';
el.innerHTML=h;
}
var editing=false;
function editMode(){
editing=!editing;if(!editing){renderRemote();return}
var el=$('remote-devices'),h='<div class="grid">';
sigs.forEach(function(s,i){
h+='<div class="grid-b" onclick="editBtn('+i+')" style="cursor:pointer">'+
'<span class="ic" style="color:var(--blue)">&#9998;</span>'+s.name+'</div>';
});
h+='</div>';
h+='<div style="margin-top:10px;display:flex;gap:8px">';
sigs.forEach(function(s,i){
h+='<button class="b b-rd" style="flex:1;font-size:13px;padding:8px" onclick="delSig('+i+')">&#10005; '+s.name+'</button>';
if((i+1)%3===0)h+='</div><div style="margin-top:6px;display:flex;gap:8px">';
});
h+='</div>';
h+='<div style="margin-top:12px"><button class="b b-bl b-fw" onclick="editMode()">完成</button></div>';
el.innerHTML=h;
}
function editBtn(i){
renameIdx=i;
$('rename-input').value=sigs[i].name;
$('rename-modal').classList.add('show');
$('rename-input').focus();
}
function closeRename(){$('rename-modal').classList.remove('show');renameIdx=-1}
function doRename(){
if(renameIdx<0)return;
var n=$('rename-input').value.trim();
if(!n){toast('请输入名称');return}
sigs[renameIdx].name=n;
localStorage.setItem('ir_sigs',JSON.stringify(sigs));
closeRename();editMode();toast('\u2713 已重命名');
}
function delSig(i){
sigs.splice(i,1);localStorage.setItem('ir_sigs',JSON.stringify(sigs));editMode();
}
function exportAll(){
var d={sigs:sigs,ac:$('ac-v').value};
var b=new Blob([JSON.stringify(d,null,2)],{type:'application/json'});
var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='ir-remote.json';a.click();
}
function importAll(){
var inp=document.createElement('input');inp.type='file';inp.accept='.json';
inp.onchange=function(e){
var f=e.target.files[0];if(!f)return;
var r=new FileReader();r.onload=function(ev){
try{
var d=JSON.parse(ev.target.result);
if(d.sigs){sigs=d.sigs;localStorage.setItem('ir_sigs',JSON.stringify(sigs))}
if(d.ac){$('ac-v').value=d.ac;localStorage.setItem('ir_ac_v',d.ac)}
toast('\u2713 导入成功');renderRemote();
}catch(e){toast('\u2717 格式错误')}
};r.readAsText(f);
};inp.click();
}
function clearAll(){
if(!confirm('确定清除所有数据？'))return;
localStorage.removeItem('ir_sigs');localStorage.removeItem('ir_ac_v');
sigs=[];renderRemote();toast('已清除');
}
renderRemote();
function loadWifiStatus(){
  fetch('/api/wifi/status').then(function(r){return r.json()}).then(function(d){
    var s=$('wifi-status');
    var modeTxt=d.mode==='sta'?'STA 模式':d.mode==='slave'?'从机模式':'AP 模式';
    s.innerHTML='<b>'+modeTxt+'</b> &nbsp; SSID: '+d.ssid+' &nbsp; IP: '+d.ip+(d.rssi?' &nbsp; RSSI: '+d.rssi:'');
    if(d.mode==='sta'){$('wifi-forget-wrap').style.display='block'}
    if(d.mqtt){s.innerHTML+='<br><span style="color:var(--green)">MQTT: 已连接 ('+d.mqtt_host+')</span>'}
    else if(d.mqtt_host){s.innerHTML+='<br><span style="color:var(--gray)">MQTT: 未连接</span>'}
  }).catch(function(){});
}
loadWifiStatus();
function scanWifi(){
  var el=$('wifi-scan-results');el.innerHTML='<div style="padding:12px;text-align:center;color:var(--gray)">扫描中...</div>';
  $('wifi-list').style.display='block';
  fetch('/api/wifi/scan').then(function(r){return r.json()}).then(function(list){
    if(!list.length){el.innerHTML='<div style="padding:12px;text-align:center;color:var(--gray)">没有发现WiFi</div>';return}
    var h='';
    list.sort(function(a,b){return b.rssi-a.rssi});
    list.forEach(function(w){
      var q=w.rssi>-50?'●●●':w.rssi>-70?'●●○':'●○○';
      h+='<div class="it" onclick="pickWifi(\''+w.ssid.replace(/'/g,"\\'")+'\')">'
        +'<span>'+w.ssid+' <span style="color:var(--gray);font-size:12px">'+q+' '+w.rssi+'dBm</span></span>'
        +(w.enc?'<span style="color:var(--gray);font-size:12px">&#128274;</span>':'')
        +'</div>';
    });
    el.innerHTML=h;
  }).catch(function(){el.innerHTML='<div style="padding:12px;text-align:center;color:var(--red)">扫描失败</div>'});
}
function pickWifi(ssid){$('wifi-ssid').value=ssid;$('wifi-pass').focus()}
function connectWifi(){
  var ssid=$('wifi-ssid').value.trim();if(!ssid){toast('请输入WiFi名称');return}
  toast('正在连接...');
  fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent($('wifi-pass').value)})
  .then(function(r){return r.json()}).then(function(d){toast(d.ok?'重启中，请等待30秒...':'失败: '+d.error)});
}
function forgetWifi(){
  if(!confirm('确定忘记网络？设备将重启为AP模式'))return;
  fetch('/api/wifi/forget',{method:'POST'}).then(function(){toast('重启中...')});
}
function loadMqttConfig(){
  fetch('/api/mqtt/config').then(function(r){return r.json()}).then(function(d){
    if(d.host)$('mqtt-host').value=d.host;
    if(d.port)$('mqtt-port').value=d.port;
    if(d.user)$('mqtt-user').value=d.user;
    if(d.topic)$('mqtt-topic').value=d.topic;
  }).catch(function(){});
}
loadMqttConfig();
function saveMqtt(){
  var host=$('mqtt-host').value.trim();if(!host){toast('请输入MQTT服务器地址');return}
  toast('保存中...');
  fetch('/api/mqtt/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'host='+encodeURIComponent(host)+'&port='+$('mqtt-port').value
    +'&user='+encodeURIComponent($('mqtt-user').value)
    +'&pass='+encodeURIComponent($('mqtt-pass').value)
    +'&topic='+encodeURIComponent($('mqtt-topic').value)})
  .then(function(r){return r.json()}).then(function(d){toast(d.ok?'重启中...':'失败')});
}
function loadApConfig(){
  fetch('/api/ap/config').then(function(r){return r.json()}).then(function(d){
    if(d.ssid)$('ap-ssid').value=d.ssid;
    if(d.pass)$('ap-pass').value=d.pass;
  }).catch(function(){});
}
loadApConfig();
function saveApConfig(){
  var ssid=$('ap-ssid').value.trim();if(!ssid){toast('请输入热点名称');return}
  if(ssid.length>32){toast('名称不能超过32个字符');return}
  var pass=$('ap-pass').value;
  if(pass.length>0&&pass.length<8){toast('密码至少8位');return}
  toast('保存中...');
  fetch('/api/ap/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})
  .then(function(r){return r.json()}).then(function(d){toast(d.ok?'重启中，请等待30秒...':'失败: '+(d.error||''))});
}
function factoryReset(){
  if(!confirm('确定恢复出厂设置？\n\n所有配置将被清除（WiFi、MQTT、热点配置）。\n设备将以默认 IR-AC / 12345678 启动。'))return;
  toast('恢复出厂中...');
  fetch('/api/factory/reset',{method:'POST'}).then(function(r){return r.json()}).then(function(d){toast(d.ok?'重启中...':'失败')}).catch(function(){toast('失败')});
}
var slavePollTimer=null;
function loadSysInfo(){
  fetch('/api/system/info').then(function(r){return r.json()}).then(function(d){
    if(d.mac)$('sys-mac').textContent=d.mac;
    var seg=$('s-devmode');if(seg){
      seg.querySelectorAll('button').forEach(function(b){
        var fm=d.forceMode||0;
        b.classList.toggle('active',(b.dataset.v==='auto'&&fm===0)||(b.dataset.v==='ap'&&fm===1)||(b.dataset.v==='slave'&&fm===2)||(b.dataset.v==='home'&&fm===3));
      });
    }
    if(d.mode==='ap'){$('slaves-card').style.display='';$('devmode-card').style.display='';startSlavePoll()}
    else{$('slaves-card').style.display='none';$('devmode-card').style.display='';stopSlavePoll()}
  }).catch(function(){});
}
loadSysInfo();
function initDevModeSeg(){
  var seg=$('s-devmode');if(!seg)return;
  var bs=seg.querySelectorAll('button');
  bs.forEach(function(b){b.onclick=function(){bs.forEach(function(x){x.classList.remove('active')});b.classList.add('active')}});
}
initDevModeSeg();
function saveForceMode(){
  var seg=$('s-devmode');var v='auto';
  if(seg){var act=seg.querySelector('.active');if(act)v=act.dataset.v}
  toast('保存中...');
  fetch('/api/mode/force',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mode='+v})
  .then(function(r){return r.json()}).then(function(d){toast(d.ok?'重启中...':'失败')}).catch(function(){toast('失败')});
}
function startSlavePoll(){if(slavePollTimer)return;slavePollTimer=setInterval(loadSlaves,3000);loadSlaves()}
function stopSlavePoll(){if(slavePollTimer){clearInterval(slavePollTimer);slavePollTimer=null}}
function loadSlaves(){
  fetch('/api/slaves').then(function(r){return r.json()}).then(function(d){
    var el=$('slave-list');if(!el)return;
    var h='';
    if(d.slaves&&d.slaves.length>0){
      d.slaves.forEach(function(s){h+='<div class="slave-row"><span class="slave-dot"></span><span class="slave-mac">'+s.mac+'</span><span class="slave-ip">'+s.ip+'</span></div>'})
    }else{h='<div style="color:var(--gray);font-size:13px;padding:8px 0">暂无从机连接</div>'}
    el.innerHTML=h;
    var pb=$('pair-btn');
    if(pb){
      if(d.pairing){pb.textContent='配对中 ('+d.pairingLeft+'s)...';pb.classList.add('pair-pulse')}
      else{pb.textContent='配对新设备';pb.classList.remove('pair-pulse')}
    }
  }).catch(function(){});
}
function togglePair(){
  fetch('/api/slaves').then(function(r){return r.json()}).then(function(d){
    if(d.pairing){fetch('/api/pair/stop',{method:'POST'}).then(function(){loadSlaves()})}
    else{toast('配对模式已开启');fetch('/api/pair/start',{method:'POST'}).then(function(){loadSlaves()})}
  });
}
</script>
</body></html>)rawliteral";
#endif
