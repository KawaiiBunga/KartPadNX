'use strict';
// KartPadNX web builder front end: checks the disc header locally, uploads the image in chunks
// to the build server, follows the build and downloads the SD card zip.

const SERVER = (new URLSearchParams(location.search).get('server') || window.KPNX_SERVER || '').replace(/\/+$/, '');
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

async function upload() {
  const chunk = job.chunk;
  let { received } = await api(`/api/jobs/${job.id}`);
  let failures = 0;
  while (received < file.size) {
    const end = Math.min(received + chunk, file.size);
    try {
      ({ received } = await api(`/api/jobs/${job.id}/upload?offset=${received}`, { method: 'PUT', body: file.slice(received, end) }));
      failures = 0;
    } catch (e) {
      if (e.body && typeof e.body.received === 'number') { received = e.body.received; continue; }
      if (++failures > 5) throw e;
      await new Promise((r) => setTimeout(r, 2000 * failures));
    }
    setPhase(`Uploading… ${Math.floor(100 * received / file.size)}%`, received / file.size, 'Keep this page open until the upload is done.');
  }
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
const day = (iso) => new Date(iso).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
const getJson = (url) => fetch(url).then((r) => (r.ok ? r.json() : Promise.reject(new Error(r.status))));

async function showVersions(status) {
  const rr = status && status.rr_latest;
  if (rr && rr.version) {
    $('rr-latest').textContent = rr.date ? `${rr.version} (${day(rr.date)})` : rr.version;
  } else {   // builder offline: Retro Rewind's own feed still says which version is newest
    fetch('https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt').then((r) => r.text())
      .then((t) => { $('rr-latest').textContent = t.trim().split('\n').pop().split(/\s+/)[0]; })
      .catch(() => { $('rr-latest').textContent = 'unknown'; });
  }
  const b = status && status.builder;
  if (b && b.tag) {
    $('builder').textContent = b.date ? `${b.tag} (${day(b.date)})` : b.tag;
  } else {
    getJson(`https://api.github.com/repos/${REPO}/releases?per_page=1`)
      .then(([r]) => { $('builder').textContent = `${r.tag_name} (${day(r.published_at)})`; })
      .catch(() => { $('builder').textContent = 'unknown'; });
  }
  getJson(`https://api.github.com/repos/${REPO}`)
    .then((r) => { $('updated').textContent = day(r.pushed_at); })
    .catch(() => { $('updated').textContent = 'unknown'; });
}

// ---- start ----

(async () => {
  if (!SERVER || SERVER.includes('example.com')) {
    showVersions(null);
    $('server').textContent = 'Not open yet. Check back soon.';
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
