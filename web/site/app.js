'use strict';
// KartPadNX web builder front end: checks the disc header locally, uploads the image in chunks
// to the build server, follows the build and downloads the SD card zip.

let SERVER = '';   // set at start (findServer)
const $ = (id) => document.getElementById(id);
const store = {
  get() { try { return JSON.parse(localStorage.getItem('kpnx-job') || 'null'); } catch { return null; } },
  set(v) { try { v ? localStorage.setItem('kpnx-job', JSON.stringify(v)) : localStorage.removeItem('kpnx-job'); } catch {} },
};
let job = null;      // {id, token, size, rr}
let file = null;
let discOk = false;

async function api(path, opts = {}) {
  const headers = Object.assign({}, opts.headers, job ? { 'X-Job-Token': job.token } : {});
  const res = await fetch(SERVER + path, Object.assign({}, opts, { headers }));
  const body = res.headers.get('Content-Type') === 'application/json' ? await res.json() : null;
  if (!res.ok) throw Object.assign(new Error((body && body.error) || `HTTP ${res.status}`), { status: res.status, body });
  return body;
}

// ---- disc check (the server repeats it, then checks main.dol and StaticR.rel exactly) ----

async function bytes(f, start, end) { return new Uint8Array(await f.slice(start, end).arrayBuffer()); }

async function discHeader(f) {
  const head = await bytes(f, 0, 0x100);
  const magic = String.fromCharCode(...head.slice(0, 4));
  if (magic === 'WBFS') return bytes(f, 2 ** head[8], 2 ** head[8] + 0x80);
  if (magic === 'RVZ\x01' || magic === 'WIA\x01') return head.slice(0x58, 0xd8);
  return head.slice(0, 0x80);
}

async function checkDisc(f) {
  if (!/\.(iso|wbfs|rvz)$/i.test(f.name)) return 'That isn\'t a disc file. Pick an .iso, .wbfs or .rvz file.';
  const h = await discHeader(f);
  const wii = h.length >= 0x20 && h[0x18] === 0x5d && h[0x19] === 0x1c && h[0x1a] === 0x9e && h[0x1b] === 0xa3;
  if (!wii) return 'That isn\'t a Wii disc file.';
  const id = String.fromCharCode(...h.slice(0, 6));
  if (id === 'RMCE01' || id === 'RMCJ01' || id === 'RMCK01') return 'Wrong disc: this is the US, Japanese or Korean version. Only the European (PAL) one works.';
  if (id !== 'RMCP01') return 'Wrong disc: this isn\'t Mario Kart Wii.';
  if (h[7] !== 0) return 'Wrong disc version: only the original European release works.';
  return null;
}

function refreshGo() { $('go').disabled = !(discOk && $('own').checked); }

$('file').addEventListener('change', async () => {
  file = $('file').files[0] || null;
  discOk = false;
  $('file-label').textContent = file ? file.name : 'Choose file (.iso, .wbfs or .rvz)';
  $('check').className = 'note';
  $('check').textContent = file ? 'Checking…' : '';
  if (file) {
    const problem = await checkDisc(file).catch((e) => `Could not read the file: ${e.message}`);
    discOk = !problem;
    $('check').className = 'note ' + (discOk ? 'good' : 'bad');
    $('check').textContent = problem || 'Looks good!';
  }
  refreshGo();
});
$('own').addEventListener('change', refreshGo);

// ---- upload and build ----

function show(section) {
  for (const s of ['pick', 'progress', 'done']) $(s).hidden = s !== section;
}

function setPhase(text, fraction, detail = '') {
  $('phase').textContent = text;
  if (fraction == null) $('bar').removeAttribute('value'); else $('bar').value = fraction;
  $('detail').textContent = detail;
}

// Several pieces at once: one connection through the tunnel is capped far below a fast line.
const PARALLEL = 6;

async function upload() {
  const chunk = job.chunk;
  const total = Math.ceil(file.size / chunk);
  const st = await api(`/api/jobs/${job.id}`);   // resumes after a reload
  // A server from before parallel pieces has no `missing` list and takes pieces only in order.
  const todo = st.missing || [...Array(total).keys()].slice(Math.floor(st.received / chunk));
  const workers = st.missing ? PARALLEL : 1;
  let done = total - todo.length;
  const show = () => setPhase(`Uploading… ${Math.floor(100 * done / total)}%`, done / total,
    'Keep this page open until the upload is done.');
  show();
  async function worker() {
    for (let i = todo.shift(); i !== undefined; i = todo.shift()) {
      for (let attempt = 1; ; attempt++) {
        try {
          await api(`/api/jobs/${job.id}/upload?offset=${i * chunk}`,
            { method: 'PUT', body: file.slice(i * chunk, Math.min((i + 1) * chunk, file.size)) });
          break;
        } catch (e) {
          if (e.status === 404 || e.status === 409 || attempt > 5) throw e;
          await new Promise((r) => setTimeout(r, 2000 * attempt));
        }
      }
      done++;
      show();
    }
  }
  await Promise.all(Array.from({ length: workers }, worker));
  await api(`/api/jobs/${job.id}/start`, { method: 'POST' });
}

async function follow() {
  for (;;) {
    const s = await api(`/api/jobs/${job.id}`);
    $('log').textContent = (s.log || []).join('\n');
    if (s.state === 'uploading') return 'uploading';
    if (s.state === 'queued') setPhase('Waiting in line…', null, `You're number ${s.queue_position}.`);
    if (s.state === 'building') setPhase('Building…', s.progress, `${s.stage ? s.stage + '. ' : ''}You can close this page and come back later.`);
    if (s.state === 'failed') throw new Error(s.error || 'The build failed.');
    if (s.state === 'done') return s;
    await new Promise((r) => setTimeout(r, 3000));
  }
}

function finished() { show('done'); }

async function run(resume) {
  show('progress');
  try {
    if (!resume) {
      setPhase('Starting…', null);
      job = null;
      const created = await api('/api/jobs', { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: file.name, size: file.size, rr: true }) });
      job = { id: created.id, token: created.token, chunk: created.chunk, size: file.size, name: file.name };
      store.set(job);
    }
    let s = await follow();
    if (s === 'uploading') {
      if (!file || file.size !== job.size || file.name !== job.name) {
        show('pick');
        $('check').className = 'note';
        $('check').textContent = `Pick ${job.name} again to finish uploading it.`;
        return;
      }
      await upload();
      s = await follow();
    }
    finished();
  } catch (e) {
    if (e.status === 404) {   // the build expired or the server restarted: start fresh
      store.set(null);
      job = null;
      if (resume) { show('pick'); return; }
    }
    setPhase('Something went wrong', 0, e.message);
  }
}

$('go').addEventListener('click', () => {
  const saved = store.get();
  run(saved && file && saved.size === file.size && saved.name === file.name && (job = saved));
});
$('cancel').addEventListener('click', () => { store.set(null); job = null; show('pick'); });
$('download').addEventListener('click', async () => {
  // A one-time link the browser downloads itself: the zip is several GB, too big for fetch/Blob.
  $('download').disabled = true;
  try {
    const { url } = await api(`/api/jobs/${job.id}/ticket`, { method: 'POST' });
    Object.assign(document.createElement('a'), { href: SERVER + url }).click();
    $('download').textContent = 'Downloading! Check your browser\'s downloads';
  } catch (e) {
    $('download').textContent = `Download failed (${e.message}). Try again`;
  }
  setTimeout(() => { $('download').disabled = false; }, 5000);
});

// ---- versions: what the builder uses, and how fresh it is ----

const REPO = 'KawaiiBunga/KartPadNX';
// Every timestamp is UTC at the source and shown in the visitor's own time zone, with its name.
const when = (iso) => new Date(iso).toLocaleString(undefined, {
  year: 'numeric', month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit', timeZoneName: 'short' });
const withDate = (label, iso) => (iso ? `${label} (${when(iso)})` : label);
const getJson = (url) => fetch(url, { cache: 'no-store' }).then((r) => (r.ok ? r.json() : Promise.reject(new Error(r.status))));

async function showVersions(status) {
  // Newest Retro Rewind: the builder knows it when it's open; rr.json (refreshed hourly by the
  // Pages workflow) when it isn't; Retro Rewind's own feed (version only) as the last resort.
  const rr = status && status.rr_latest;
  if (rr && rr.version) {
    $('rr-latest').textContent = withDate(rr.version, rr.date);
  } else {
    getJson(`rr.json?t=${Date.now()}`)
      .then((j) => { if (!j.version) throw new Error('empty'); $('rr-latest').textContent = withDate(j.version, j.date); })
      .catch(() => fetch('https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt').then((r) => r.text())
        .then((t) => { $('rr-latest').textContent = t.trim().split('\n').pop().split(/\s+/)[0]; }))
      .catch(() => { $('rr-latest').textContent = 'unknown'; });
  }
  const b = status && status.builder;
  if (b && b.tag) {
    $('builder').textContent = withDate(b.tag, b.date);
  } else {
    getJson(`https://api.github.com/repos/${REPO}/releases?per_page=1`)
      .then(([r]) => { $('builder').textContent = withDate(r.tag_name, r.published_at); })
      .catch(() => { $('builder').textContent = 'unknown'; });
  }
  getJson(`https://api.github.com/repos/${REPO}`)
    .then((r) => { $('updated').textContent = when(r.pushed_at); })
    .catch(() => { $('updated').textContent = 'unknown'; });
}

// ---- start ----

// ?server=, then config.js, then the address web/start-builder.ps1 last published.
async function findServer() {
  const given = new URLSearchParams(location.search).get('server') || window.KPNX_SERVER;
  if (given) return given;
  // Through the API: raw.githubusercontent.com caches the file ~5 minutes and ignores query strings,
  // so a restarted builder's new address would show late. The raw CDN is only the fallback (the
  // API allows 60 requests an hour per visitor).
  const sources = [
    [`https://api.github.com/repos/${REPO}/contents/server.txt?ref=builder-url`, { headers: { Accept: 'application/vnd.github.raw' }, cache: 'no-store' }],
    [`https://raw.githubusercontent.com/${REPO}/builder-url/server.txt`, { cache: 'no-store' }],
  ];
  for (const [url, opts] of sources) {
    try {
      const r = await fetch(url, opts);
      if (r.ok) return (await r.text()).trim();
    } catch { /* try the next one */ }
  }
  return '';
}

(async () => {
  SERVER = (await findServer()).replace(/\/+$/, '');
  if (!/^https?:\/\//.test(SERVER)) {
    showVersions(null);
    $('server').textContent = 'Closed right now. Try again later.';
    $('server').className = 'pill bad';
    return;
  }
  try {
    const st = await api('/api/status');
    showVersions(st);
    $('server').textContent = st.busy ? `Open (busy, ${st.queued} waiting)` : 'Open';
    $('server').className = 'pill good';
  } catch {
    showVersions(null);
    $('server').textContent = 'Closed right now. Try again later.';
    $('server').className = 'pill bad';
    return;
  }
  const saved = store.get();
  if (saved) { job = saved; run(true); }
})();
