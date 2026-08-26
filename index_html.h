// ─────────────────────────────────────────────────────────────────────────
// index_html.h — Live position map page, served by the ESP32 at "/".
// Polls GET /beacons once, then GET /data every ~800ms (plain HTTP —
// avoids needing a WebSocket/Async web server library at all).
// ─────────────────────────────────────────────────────────────────────────
#pragma once

const char INDEX_HTML[] = R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BLE Live Position</title>
<style>
  body { margin:0; font-family: -apple-system, Segoe UI, Roboto, sans-serif;
         background:#f7f6f2; color:#333; }
  #wrap { display:flex; flex-wrap:wrap; }
  #mapBox { flex:1; min-width:320px; padding:12px; }
  canvas { background:#eeeee8; border-radius:8px; width:100%; max-width:700px;
           display:block; }
  #panel { width:220px; padding:16px; }
  #panel h3 { margin:0 0 10px; font-size:14px; color:#888; }
  .row { margin-bottom:12px; }
  .row .label { font-size:11px; color:#999; text-transform:uppercase; }
  .row .val { font-size:16px; font-weight:600; font-family:monospace; }
  #status { position:fixed; top:8px; right:12px; font-size:12px; color:#999; }
</style>
</head>
<body>
<div id="status">connecting…</div>
<div id="wrap">
  <div id="mapBox">
    <canvas id="map" width="700" height="500"></canvas>
    </div>
    <div id="panel">
    <h3>LIVE POSITION</h3>
    <div class="row"><div class="label">Position (x, y)</div><div class="val" id="v_pos">—</div></div>
    <div class="row"><div class="label">Last seen</div><div class="val" id="v_seen" style="color:#1d4ed8;">—</div></div>
    <div class="row"><div class="label">GPS</div><div class="val" id="v_gps" style="font-size:12px;">—</div></div>
    <div class="row"><div class="label">Rank-1 beacon</div><div class="val" id="v_b1">—</div></div>
    <div class="row"><div class="label">Confidence</div><div class="val" id="v_conf">—</div></div>
    <div class="row"><div class="label">Motion</div><div class="val" id="v_mot">—</div></div>
    <div class="row"><div class="label">Beacons in window</div><div class="val" id="v_n">—</div></div>
    <div class="row"><div class="label">Moves recorded</div><div class="val" id="v_cnt">0</div></div>
    </div>
    </div>

    <script>
    let beacons = [];
    let trail = [];
    const MAX_TRAIL = 60;
    let moveCount = 0;
    let scaleInfo = null;
    let lastTs = null;    // device ts_ms of the last position we actually applied

    function formatAge(ms) {
        const s = Math.floor(ms / 1000);
        if (s < 5) return 'just now';
        if (s < 60) return `${s}s ago`;
        const m = Math.floor(s / 60);
        if (m < 60) return `${m}m ${s % 60}s ago`;
        const h = Math.floor(m / 60);
        return `${h}h ${m % 60}m ago`;
    }

    const canvas = document.getElementById('map');
    const ctx = canvas.getContext('2d');

    function computeScale() {
        if (!beacons.length) return;
        const pad = 40;
        const xs = beacons.map(b => b.lx), ys = beacons.map(b => b.ly);
        const minX = Math.min(...xs), maxX = Math.max(...xs);
        const minY = Math.min(...ys), maxY = Math.max(...ys);
        const spanX = Math.max(maxX - minX, 1), spanY = Math.max(maxY - minY, 1);
        const sx = (canvas.width - 2*pad) / spanX;
        const sy = (canvas.height - 2*pad) / spanY;
        const s = Math.min(sx, sy);
        scaleInfo = { minX, minY, s, pad };
    }

    function toPx(lx, ly) {
        const { minX, minY, s, pad } = scaleInfo;
        return [ pad + (lx - minX) * s, pad + (ly - minY) * s ];
    }

    function draw(latest) {
        if (!scaleInfo) return;
        ctx.clearRect(0, 0, canvas.width, canvas.height);

        ctx.strokeStyle = '#dddddc'; ctx.lineWidth = 1;
        for (let gx = 0; gx < canvas.width; gx += 25) { ctx.beginPath(); ctx.moveTo(gx,0); ctx.lineTo(gx,canvas.height); ctx.stroke(); }
        for (let gy = 0; gy < canvas.height; gy += 25) { ctx.beginPath(); ctx.moveTo(0,gy); ctx.lineTo(canvas.width,gy); ctx.stroke(); }

        beacons.forEach(b => {
            const [px, py] = toPx(b.lx, b.ly);
            ctx.fillStyle = '#cccccc';
        ctx.beginPath(); ctx.arc(px, py, 6, 0, 2*Math.PI); ctx.fill();
        ctx.strokeStyle = '#bbbbbb'; ctx.stroke();
        ctx.fillStyle = '#aaaaaa'; ctx.font = '10px monospace';
        ctx.fillText(b.name.slice(-4), px - 10, py + 18);
        });

        if (trail.length > 1) {
            ctx.strokeStyle = '#1d4ed8'; ctx.lineWidth = 2;
            ctx.beginPath();
            trail.forEach((p, i) => {
                const [px, py] = toPx(p.x, p.y);
                i === 0 ? ctx.moveTo(px, py) : ctx.lineTo(px, py);
            });
            ctx.stroke();
        }

        if (!latest) return;

        const b1 = beacons.find(b => b.name === latest.b1);
        if (b1) {
            const [bx, by] = toPx(b1.lx, b1.ly);
            const [px, py] = toPx(latest.x, latest.y);
            ctx.strokeStyle = '#fbbf24'; ctx.setLineDash([3,3]);
            ctx.beginPath(); ctx.moveTo(px,py); ctx.lineTo(bx,by); ctx.stroke();
            ctx.setLineDash([]);
            ctx.fillStyle = '#15803d';
            ctx.beginPath(); ctx.arc(bx, by, 8, 0, 2*Math.PI); ctx.fill();
            ctx.strokeStyle = 'white'; ctx.lineWidth = 2; ctx.stroke();
        }

        const [px, py] = toPx(latest.x, latest.y);
        ctx.fillStyle = '#fbbf24';
        ctx.beginPath(); ctx.arc(px, py, 11, 0, 2*Math.PI); ctx.fill();
        ctx.strokeStyle = '#f59e0b'; ctx.lineWidth = 3; ctx.stroke();
    }

    function applyUpdate(msg, isNew) {
        if (isNew) {
            moveCount++;
            trail.push({ x: msg.x, y: msg.y });
            if (trail.length > MAX_TRAIL) trail.shift();
        }
        draw(msg);

        document.getElementById('v_pos').textContent = `(${msg.x.toFixed(1)}, ${msg.y.toFixed(1)})`;
        document.getElementById('v_seen').textContent = formatAge(msg.age_ms);
        document.getElementById('v_gps').textContent = msg.haveGps ? `${msg.lat.toFixed(6)}, ${msg.lon.toFixed(6)}` : '—';
        document.getElementById('v_b1').textContent = `${msg.b1 || '—'} (${msg.b1_rssi ?? '?'} dBm)`;
        const confEl = document.getElementById('v_conf');
        confEl.textContent = msg.conf;
        confEl.style.color = msg.conf === 'high' ? '#15803d' : msg.conf === 'medium' ? '#1d4ed8' : '#b45309';
        const motEl = document.getElementById('v_mot');
        motEl.textContent = msg.motion === 'walking' ? '▶ walking' : '◉ stationary';
        motEl.style.color = msg.motion === 'walking' ? '#1d4ed8' : '#15803d';
        document.getElementById('v_n').textContent = msg.n;
        document.getElementById('v_cnt').textContent = moveCount;
    }

    async function pollData() {
        try {
            const res = await fetch('/data', { cache: 'no-store' });
            if (!res.ok) throw new Error('bad status');
            const msg = await res.json();
            document.getElementById('status').textContent = 'connected';
            if (msg && msg.b1) {
                const isNew = msg.ts_ms !== lastTs;
                lastTs = msg.ts_ms;
                applyUpdate(msg, isNew);
            }
        } catch (e) {
            document.getElementById('status').textContent = 'disconnected — retrying…';
        }
    }

    async function init() {
        try {
            const res = await fetch('/beacons', { cache: 'no-store' });
            const data = await res.json();
            beacons = data.beacons;
            computeScale();
            draw(null);
        } catch (e) {
            document.getElementById('status').textContent = 'failed to load beacons';
            return;
        }
        pollData();
        setInterval(pollData, 800);
    }
    init();
    </script>
    </body>
    </html>
    )HTMLPAGE";
