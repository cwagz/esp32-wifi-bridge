/*
 * Web UI Assets - SVG icons and CSS styles
 * Separated from main.c to reduce visual noise
 */

#ifndef WEB_UI_H
#define WEB_UI_H

// ===== Favicon (SVG data URI - bridge/connection icon) =====

#define FAVICON_SVG "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='6' fill='%233b82f6'/%3E%3Cpath d='M6 16h20M16 8v16M10 12l-4 4 4 4M22 12l4 4-4 4' stroke='white' stroke-width='2' fill='none' stroke-linecap='round' stroke-linejoin='round'/%3E%3C/svg%3E"

// ===== SVG Icons (inline, no external fonts needed) =====

#define ICON_WIFI "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M1 9l2 2c4.97-4.97 13.03-4.97 18 0l2-2C16.93 2.93 7.08 2.93 1 9zm8 8l3 3 3-3c-1.65-1.66-4.34-1.66-6 0zm-4-4l2 2c2.76-2.76 7.24-2.76 10 0l2-2C15.14 9.14 8.87 9.14 5 13z\"/></svg>"

#define ICON_SIGNAL "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M2 22h20V2L2 22z\"/></svg>"

#define ICON_BATTERY "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M16 4h-2V2h-4v2H8C6.9 4 6 4.9 6 6v14c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V6c0-1.1-.9-2-2-2zm0 16H8V6h8v14z\"/></svg>"

#define ICON_DNS "<svg class=\"i\" viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"10\"/><circle cx=\"12\" cy=\"12\" r=\"3\" fill=\"#1e293b\"/></svg>"

#define ICON_SETTINGS "<svg class=\"i\" viewBox=\"0 0 1200 1200\"><path d=\"m1061.8 517.5-59.5-13.5c-10.3-42.8-27-83.3-50.3-119.8l32.8-52.5c12.2-19.7 9.5-44.8-7-61l-48.5-48.5c-16.3-16.5-41.3-19.3-61-7l-51.8 32.5c-37.3-23-77.8-39.8-120.2-49.3l-13.7-60.2c-5.3-22.5-25-38.3-48.2-38.3h-68.5c-23.3 0-43 15.8-48.2 38.3l-13.5 59.5c-42.8 10.3-83.3 27-119.8 50.3l-52.5-32.8c-19.7-12.2-44.8-9.5-61 7l-48.5 48.5c-16.5 16.3-19.3 41.3-7 61l32.5 51.8c-23 37.3-39.8 77.8-49.3 120.2l-60.5 14c-22.3 5-38 24.8-38 47.8v69c0 23 15.8 42.8 38.3 48l59.5 13.5c10.3 42.8 27 83.3 50.3 119.8l-32.8 52.5c-12.2 19.7-9.5 44.8 7 61l48.5 48.5c16.3 16.5 41.3 19.3 61 7l51.8-32.5c37.3 23 77.8 39.8 120.2 49.3l13.7 60.2c5.3 22.5 25 38.3 48.2 38.3h68.5c23.3 0 43-15.8 48.2-38.3l13.5-59.5c42.8-10.3 83.3-27 119.8-50.3l52.5 32.8c19.7 12.2 44.8 9.5 61-7l48.5-48.5c16.5-16.3 19.3-41.3 7-61l-32.5-51.8c23-37.3 39.8-77.8 49.3-120.2l60.5-14c22.3-5 38-24.8 38-47.8v-69c0-23-15.8-42.8-38.3-48zM855.8 600c0 141-114.8 255.7-255.7 255.7-339.3-14-339.2-497.5 0-511.5 141 0 255.7 114.8 255.7 255.8z\"/></svg>"

#define ICON_SEARCH "<svg class=\"i\" viewBox=\"0 0 24 24\"><circle cx=\"10\" cy=\"10\" r=\"7\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/><path d=\"M15 15l6 6\" stroke=\"currentColor\" stroke-width=\"2\"/></svg>"

#define ICON_SAVE "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M17 3H5a2 2 0 00-2 2v14a2 2 0 002 2h14a2 2 0 002-2V7l-4-4zm-5 16a3 3 0 110-6 3 3 0 010 6zm3-10H5V5h10v4z\"/></svg>"

#define ICON_MEMORY "<svg class=\"i\" viewBox=\"0 0 24 24\"><rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\"/><rect x=\"8\" y=\"8\" width=\"8\" height=\"8\" fill=\"#1e293b\"/></svg>"

#define ICON_LAN "<svg class=\"i\" viewBox=\"0 0 24 24\"><path fill-rule=\"evenodd\" d=\"M3 5h18a2 2 0 012 2v8a2 2 0 01-2 2h-3.2L16 19H8l-1.8-2H3a2 2 0 01-2-2V7a2 2 0 012-2zm3 3h2v5H6V8zm4 0h2v5h-2V8zm4 0h2v5h-2V8zm4 0h2v5h-2V8z\"/></svg>"

#define ICON_SWAP "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M6 9l-4 4 4 4v-3h8v-2H6V9zm12 6l4-4-4-4v3H10v2h8v3z\"/></svg>"

#define ICON_UPDATE "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M12 4V1L8 5l4 4V6a6 6 0 11-6 6H4a8 8 0 108-8z\"/></svg>"

#define ICON_UPLOAD "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M9 16h6v-6h4l-7-7-7 7h4v6zm-4 2h14v2H5v-2z\"/></svg>"

#define ICON_HISTORY "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M12 4a8 8 0 00-8 8H1l4 4 4-4H6a6 6 0 116 6v2a8 8 0 000-16zm-1 5v4l3 2 1-1-2.5-1.5V9h-1.5z\"/></svg>"

#define ICON_LOCK "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M17 8h-1V6a4 4 0 00-8 0v2H7a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2V10a2 2 0 00-2-2zM9 6a3 3 0 016 0v2H9V6zm3 9a1.5 1.5 0 110-3 1.5 1.5 0 010 3z\"/></svg>"

#define ICON_WARN "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M1 21h22L12 2 1 21zm12-3h-2v-2h2v2zm0-4h-2v-4h2v4z\"/></svg>"

#define ICON_EXPAND "<svg class=\"i\" style=\"width:.6rem;height:.6rem\" viewBox=\"0 0 24 24\"><path d=\"M7 10l5 5 5-5H7z\"/></svg>"

#define ICON_ROUTER "<svg class=\"i\" viewBox=\"0 0 24 24\"><rect x=\"3\" y=\"13\" width=\"18\" height=\"8\" rx=\"2\"/><circle cx=\"7\" cy=\"17\" r=\"1.5\"/><circle cx=\"12\" cy=\"17\" r=\"1.5\"/><path d=\"M12 3v7M8 6l4-3 4 3\"/></svg>"

// ===== Chart Icon =====

#define ICON_CHART "<svg class=\"i\" viewBox=\"0 0 24 24\"><path d=\"M3 3v18h18V3H3zm16 16H5V5h14v14zM7 12h2v5H7v-5zm4-3h2v8h-2V9zm4-3h2v11h-2V6z\"/></svg>"

// ===== Dark Mode CSS (Tailwind-inspired) =====

static const char *DARK_CSS =
    // Reset and base
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh;padding:1.5rem}"
    ".container{max-width:42rem;margin:0 auto}"
    // Cards
    ".card{background:#1e293b;border-radius:0.75rem;padding:1.5rem;margin-bottom:1rem;border:1px solid #334155}"
    // Typography
    "h1{font-size:1.5rem;font-weight:600;margin-bottom:1rem;color:#f8fafc}"
    "h2{font-size:1.125rem;font-weight:600;margin-bottom:0.75rem;color:#f1f5f9}"
    // Grid layout
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:0.75rem}"
    ".status-item{background:#0f172a;padding:0.75rem;border-radius:0.5rem}"
    // Labels and values
    ".label{font-size:0.75rem;color:#94a3b8;text-transform:uppercase;letter-spacing:0.05em}"
    ".value{font-size:1rem;font-weight:500;margin-top:0.25rem;font-family:ui-monospace,monospace}"
    // Status indicators
    ".status-dot{display:inline-block;width:0.5rem;height:0.5rem;border-radius:50%;margin-right:0.5rem}"
    ".status-ok{background:#22c55e}"
    ".status-warn{background:#eab308}"
    ".status-err{background:#ef4444}"
    // Form elements
    "input,select{width:100%;padding:0.625rem;border-radius:0.375rem;border:1px solid #475569;background:#0f172a;color:#e2e8f0;font-size:0.875rem;margin-top:0.25rem}"
    "input:focus,select:focus{outline:none;border-color:#3b82f6;box-shadow:0 0 0 2px rgba(59,130,246,0.3)}"
    // Buttons
    ".btn{padding:0.625rem 1.25rem;border-radius:0.375rem;font-weight:500;cursor:pointer;border:none;font-size:0.875rem;transition:all 0.15s}"
    ".btn-primary{background:#3b82f6;color:#fff}"
    ".btn-primary:hover{background:#2563eb}"
    ".btn-danger{background:#dc2626;color:#fff}"
    ".btn-danger:hover{background:#b91c1c}"
    ".btn-secondary{background:#475569;color:#fff}"
    ".btn-secondary:hover{background:#64748b}"
    // Utilities
    ".form-group{margin-bottom:1rem}"
    ".flex{display:flex;gap:0.5rem;align-items:center}"
    ".mt-1{margin-top:0.5rem}"
    ".mt-2{margin-top:1rem}"
    ".text-sm{font-size:0.875rem}"
    ".text-xs{font-size:0.75rem}"
    ".text-muted{color:#64748b}"
    // Alerts
    ".alert{padding:0.75rem;border-radius:0.375rem;font-size:0.875rem}"
    ".alert-warn{background:rgba(234,179,8,0.1);border:1px solid #eab308;color:#fbbf24}"
    // Dividers
    "hr{border:none;border-top:1px solid #334155;margin:1rem 0}"
    "details.adminpw>summary{cursor:pointer;list-style:none}"
    "details.adminpw>summary::-webkit-details-marker{display:none}"
    "details.adminpw>summary h2{margin-bottom:0;display:flex;align-items:center;gap:.4rem}"
    "details.adminpw[open]>summary{margin-bottom:.75rem}"
    "details.adminpw[open]>summary svg.i:last-child{transform:rotate(180deg)}"
    // Animations
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}"
    ".animate-pulse{animation:pulse 2s infinite}"
    // Chart styles
    ".chart-container{position:relative;height:180px;background:#0f172a;border-radius:0.375rem;padding:0.5rem 1rem 0.5rem 0.5rem;margin-top:0.5rem;overflow:hidden}"
    ".chart-container canvas{display:block;width:100%;height:100%}"
    ".chart-legend{display:flex;gap:1rem;justify-content:center;margin-top:0.5rem;font-size:0.75rem}"
    ".chart-legend span{display:flex;align-items:center;gap:0.25rem}"
    ".chart-legend .dot{width:8px;height:8px;border-radius:50%}";

// ===== JavaScript for Auto-refresh =====

#define WEB_UI_SCRIPT \
    "<script>" \
    "function fillSel(s,t){s.textContent='';var o=document.createElement('option');o.textContent=t;s.appendChild(o);}" \
    "function scanWifi(){" \
    "var s=document.getElementById('wl');" \
    "fillSel(s,'Scanning...');s.style.display='block';" \
    "fetch('/wifi/scan').then(r=>r.json()).then(d=>{" \
    "s.textContent='';(d.networks||[]).forEach(function(n){" \
    "var o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm)';s.appendChild(o);});" \
    "if(!s.options.length)fillSel(s,'No networks');" \
    "}).catch(e=>{fillSel(s,'Scan failed');});}" \
    "function sigQ(r){return r>-50?'Excellent':r>-60?'Good':r>-70?'Fair':'Weak';}" \
    "function fmtAge(s){return s>=3600?Math.floor(s/3600)+'h':s>=60?Math.floor(s/60)+'m':s+'s';}" \
    "function fmtBytes(b){if(b>=1073741824)return(b/1073741824).toFixed(1)+' GB';" \
    "if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(1)+' KB';return b+' B';}" \
    "function fmtUptime(s){var d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),sec=s%60;" \
    "return d+'d '+h+'h '+m+'m '+sec+'s';}" \
    "var lastOk=Date.now(),fetching=false,dashDown=false;" \
    "function updAge(){var s=Math.floor((Date.now()-lastOk)/1000);var el=document.getElementById('lastref');" \
    "if(s>30){el.innerHTML='<span style=\"color:#ef4444\">'+s+'s ago (stale)</span>';}else{el.textContent=s+'s ago';}}" \
    "function lvlColor(l){return l==1?'#ef4444':l==2?'#eab308':l==3?'#22c55e':'#94a3b8';}" \
    "function escHtml(t){return t.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}" \
    "function dlLogs(){fetch('/logs.txt').then(function(r){if(r.status===401){location.href='/login';return Promise.reject();}" \
    "var dis=r.headers.get('Content-Disposition')||'',m=dis.match(/filename=\"([^\"]+)\"/);var name=m?m[1]:'bridge-logs.txt';" \
    "return r.blob().then(function(b){var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download=name;a.click();URL.revokeObjectURL(a.href);});});}" \
    "function refresh(){if(fetching)return;fetching=true;" \
    "Promise.all([fetch('/api/status'),fetch('/api/requests'),fetch('/api/logs')])" \
    ".then(function(rs){if(rs[0].status===401){location.href='/login';return Promise.reject();}" \
    "return Promise.all(rs.map(function(r){return r.json();}));})" \
    ".then(function(d){if(dashDown){location.reload();return;}fetching=false;lastOk=Date.now();" \
    "var st=d[0],r=st.wifi.rssi,sigEl=document.getElementById('sig');" \
    "if(r){var sc=r>-50?'#22c55e':r>-60?'#84cc16':r>-70?'#eab308':'#ef4444';" \
    "sigEl.innerHTML='<span style=\"color:'+sc+'\">'+r+' dBm ('+sigQ(r)+')</span>';}else{sigEl.textContent='-';}" \
    "document.getElementById('cpu').textContent=st.cpu+'%';" \
    "var tEl=document.getElementById('temp');" \
    "if(tEl){if(st.temp_c==null||typeof st.temp_c!=='number'){tEl.textContent='—';tEl.style.color='#94a3b8';}" \
    "else{tEl.textContent=st.temp_c.toFixed(1)+' °C';" \
    "tEl.style.color=st.temp_c>=80?'#ef4444':st.temp_c>=65?'#eab308':'#22c55e';}" \
    "var tm=document.getElementById('tempmax');" \
    "if(tm){tm.textContent=(typeof st.temp_max_c==='number')?'max '+st.temp_max_c.toFixed(1):'';}}" \
    "var wEl=document.getElementById('wdog');" \
    "if(wEl&&st.watchdog){var w=st.watchdog;" \
    "if(!w.armed){wEl.textContent='Idle';wEl.style.color='#94a3b8';}" \
    "else{var ws=w.last_s||0,lim=w.timeout_s||600;" \
    "wEl.textContent=ws>=60?'Armed · '+Math.floor(ws/60)+'m':'Armed · '+ws+'s';" \
    "wEl.style.color=ws>=lim*0.9?'#ef4444':ws>=lim*0.75?'#eab308':'#22c55e';}}" \
    "if(st.eth){var e1=document.getElementById('ethip');if(e1)e1.textContent=st.eth.ip||'N/A';" \
    "var e2=document.getElementById('ethip2');if(e2)e2.textContent=st.eth.ip||'N/A';}" \
    "document.getElementById('uptime').textContent=fmtUptime(st.uptime);" \
    "document.getElementById('reqcnt').textContent=st.total_requests;" \
    "var sr=st.total_requests>0?Math.round(st.successful_requests*100/st.total_requests):0;" \
    "var srEl=document.getElementById('succrate');srEl.textContent=sr+'%';" \
    "srEl.style.color=sr>=90?'#22c55e':sr>=70?'#eab308':'#ef4444';" \
    "document.getElementById('failcnt').textContent=st.failed_requests;" \
    "document.getElementById('bytesin').textContent=fmtBytes(st.total_bytes_in);" \
    "document.getElementById('bytesout').textContent=fmtBytes(st.total_bytes_out);" \
    "var req=d[1];document.getElementById('avgttfb').textContent=req.avg_ttfb;" \
    "var h='';req.requests.forEach(function(e){" \
    "var c=e.ok?'#22c55e':'#ef4444';" \
    "h+='<tr><td>'+fmtAge(e.age)+'</td><td>'+e.ip+'</td><td>'+e.in+'/'+e.out+'</td><td>'+e.ttfb+'-'+e.ttlb+'ms</td><td style=\"color:'+c+'\">'+(e.ok?'OK':'ERR')+'</td></tr>';});" \
    "document.getElementById('reqtbl').innerHTML=h;" \
    "var logs=d[2];var lh='';logs.logs.forEach(function(l){" \
    "lh+='<div style=\"color:'+lvlColor(l.lvl)+';white-space:pre-wrap;word-break:break-word\">'+escHtml(l.msg)+'</div>';});" \
    "document.getElementById('logview').innerHTML=lh;" \
    "updAge();})" \
    ".catch(function(){fetching=false;dashDown=true;updAge();});}" \
    "setInterval(refresh,5000);setInterval(updAge,1000);refresh();" \
    "function checkUpdate(){" \
    "otaPhase='check';" \
    "document.getElementById('update-status').innerHTML='<span class=\"animate-pulse\">Checking...</span>';" \
    "fetch('/api/check-update',{method:'POST'}).then(function(){" \
    "setTimeout(refreshUpdateStatus,2000);});}" \
    "var otaPhase='';" \
    "function refreshUpdateStatus(){" \
    "fetch('/api/update').then(r=>r.json()).then(function(u){" \
    "var st=document.getElementById('update-status');" \
    "var act=document.getElementById('update-actions');" \
    "var rev=document.getElementById('revert-btn');" \
    "if(otaPhase==='install'&&!u.install_in_progress){" \
    "if(u.last_error){otaPhase='';}else{location.reload();return;}}" \
    "if(u.check_in_progress){st.innerHTML='<span class=\"animate-pulse\">Checking...</span>';setTimeout(refreshUpdateStatus,1000);return;}" \
    "if(u.install_in_progress){st.innerHTML='<span class=\"animate-pulse\" style=\"color:#3b82f6\">Installing...</span>';setTimeout(refreshUpdateStatus,2000);return;}" \
    "if(u.update_available){" \
    "st.innerHTML='<span style=\"color:#22c55e\">Update available: '+u.available_version+'</span> ('+Math.round(u.firmware_size/1024)+' KB)';" \
    "act.style.display='flex';" \
    "}else if(u.available_version){" \
    "st.textContent='Up to date ('+u.current_version+')';act.style.display='none';" \
    "}else if(u.last_error){" \
    "st.textContent='Check failed: '+u.last_error;act.style.display='none';" \
    "}else{st.textContent='Not checked';act.style.display='none';}" \
    "if(u.previous_available){rev.style.display='inline-block';}else{rev.style.display='none';}" \
    "otaPhase='';" \
    "}).catch(function(){" \
    "var st=document.getElementById('update-status');" \
    "if(otaPhase==='install'){" \
    "st.innerHTML='<span class=\"animate-pulse\" style=\"color:#eab308\">Rebooting, waiting for device...</span>';" \
    "setTimeout(refreshUpdateStatus,2000);return;}" \
    "st.textContent='Check failed (no response)';otaPhase='';});}" \
    "function installUpdate(){" \
    "if(!confirm('Install firmware update? Device will reboot.'))return;" \
    "otaPhase='install';" \
    "document.getElementById('update-status').innerHTML='<span class=\"animate-pulse\" style=\"color:#3b82f6\">Installing...</span>';" \
    "fetch('/api/install-update',{method:'POST'}).then(function(){setTimeout(refreshUpdateStatus,2000);});}" \
    "function revertFirmware(){" \
    "if(!confirm('Revert to previous version? Device will reboot.'))return;" \
    "otaPhase='install';" \
    "document.getElementById('update-status').innerHTML='<span class=\"animate-pulse\" style=\"color:#eab308\">Reverting...</span>';" \
    "fetch('/api/revert',{method:'POST'}).then(function(){setTimeout(refreshUpdateStatus,2000);});}" \
    "function drawWifiChart(){" \
    "fetch('/api/wifi-history').then(r=>r.json()).then(function(d){" \
    "var c=document.getElementById('wifichart');if(!c)return;" \
    "var box=c.parentElement,dpr=window.devicePixelRatio||1;" \
    "var cs=getComputedStyle(box);" \
    "var w=Math.max(120,box.clientWidth-parseFloat(cs.paddingLeft)-parseFloat(cs.paddingRight));" \
    "var h=Math.max(100,box.clientHeight-parseFloat(cs.paddingTop)-parseFloat(cs.paddingBottom));" \
    "c.style.width=w+'px';c.style.height=h+'px';" \
    "c.width=Math.round(w*dpr);c.height=Math.round(h*dpr);" \
    "var ctx=c.getContext('2d');ctx.setTransform(dpr,0,0,dpr,0,0);" \
    "ctx.clearRect(0,0,w,h);" \
    "var b=d.buckets;if(!b||b.length===0){" \
    "ctx.fillStyle='#64748b';ctx.font='12px system-ui';ctx.textAlign='center';" \
    "ctx.fillText('No data yet - collecting...',w/2,h/2);return;}" \
    "var pad={l:36,r:22,t:12,b:22};var cw=w-pad.l-pad.r,ch=h-pad.t-pad.b;" \
    "var spanMin=b.length*(d.bucket_minutes||5);" \
    "var title=document.getElementById('chspan');" \
    "if(title)title.textContent=spanMin>=23*60?'(24h)':'(since boot)';" \
    "ctx.strokeStyle='#334155';ctx.lineWidth=1;" \
    "for(var i=0;i<=4;i++){var y=pad.t+ch*i/4;ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(w-pad.r,y);ctx.stroke();}" \
    "ctx.fillStyle='#64748b';ctx.font='10px system-ui';ctx.textAlign='right';" \
    "ctx.fillText('-30',pad.l-4,pad.t+8);ctx.fillText('-50',pad.l-4,pad.t+ch*0.4+4);" \
    "ctx.fillText('-70',pad.l-4,pad.t+ch*0.8+4);ctx.fillText('-90',pad.l-4,h-pad.b);" \
    "ctx.textAlign='center';" \
    "for(var i=0;i<5;i++){var x=pad.l+cw*i/4,age=spanMin*(1-i/4),lab=i===4?'now':(age>=60?Math.round(age/60)+'h':Math.round(age)+'m');ctx.fillText(lab,x,h-6);}" \
    "var dx=cw/(b.length-1||1);" \
    "ctx.beginPath();ctx.strokeStyle='rgba(34,197,94,0.3)';ctx.lineWidth=1;" \
    "for(var i=0;i<b.length;i++){var x=pad.l+i*dx,pct=b[i][1],y=pad.t+ch*(1-pct/100);" \
    "if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);}ctx.lineTo(pad.l+(b.length-1)*dx,pad.t+ch);ctx.lineTo(pad.l,pad.t+ch);ctx.closePath();" \
    "ctx.fillStyle='rgba(34,197,94,0.15)';ctx.fill();" \
    "ctx.beginPath();ctx.strokeStyle='#3b82f6';ctx.lineWidth=2;" \
    "var first=true;for(var i=0;i<b.length;i++){" \
    "var rssi=b[i][0];if(rssi===0)continue;" \
    "var x=pad.l+i*dx,y=pad.t+ch*((rssi+30)/(-90+30));y=Math.max(pad.t,Math.min(pad.t+ch,y));" \
    "if(first){ctx.moveTo(x,y);first=false;}else ctx.lineTo(x,y);}ctx.stroke();" \
    "var info=document.getElementById('wifiinfo');if(info){" \
    "var csec=d.connected_sec,dsec=d.disconnected_sec,total=csec+dsec;" \
    "var upPct=total>0?Math.round(csec*100/total):0;" \
    "info.innerHTML='<span>Uptime: '+upPct+'%</span><span>Connected: '+Math.floor(csec/60)+'m</span><span>Disconnected: '+Math.floor(dsec/60)+'m</span>'+" \
    "(d.time_synced?'<span style=\"color:#22c55e\">NTP synced</span>':'<span style=\"color:#eab308\">NTP pending</span>');}" \
    "}).catch(function(e){console.log('Chart error:',e);});}" \
    "drawWifiChart();setInterval(drawWifiChart,60000);" \
    "function toggleEthMode(){var s=document.getElementById('ethmode');var b=document.getElementById('ethstatic');" \
    "if(s&&b)b.style.display=s.value==='static'?'block':'none';}" \
    "</script>"

#endif // WEB_UI_H
