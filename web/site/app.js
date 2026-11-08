'use strict';
// KartPadNX web builder front end: checks the disc header locally, uploads the image in chunks
// to the build server, follows the build and downloads mkw-launcher.nro.

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
  if (!/\.(iso|wbfs|rvz)$/i.test(f.name)) return 'Pick an .iso, .wbfs or .rvz disc image.';
  const h = await discHeader(f);
  const wii = h.length >= 0x20 && h[0x18] === 0x5d && h[0x19] === 0x1c && h[0x1a] === 0x9e && h[0x1b] === 0xa3;
  if (!wii) return 'This is not a Wii disc image.';
  const id = String.fromCharCode(...h.slice(0, 6));
  if (id !== 'RMCP01') return `This is ${id}, not Mario Kart Wii PAL (RMCP01).`;
  if (h[7] !== 0) return `This is revision ${h[7]}; only revision 0 is supported.`;
  return null;
}

function refreshGo() { $('go').disabled = !(discOk && $('own').checked); }

$('file').addEventListener('change', async () => {
  file = $('file').files[0] || null;
  discOk = false;
  $('file-label').textContent = file ? `${file.name} (${(file.size / 2 ** 30).toFixed(2)} GB)` : 'Choose file';
  $('check').className = 'note';
  $('check').textContent = file ? 'Checking…' : '';
  if (file) {
    const problem = await checkDisc(file).catch((e) => `Could not read the file: ${e.message}`);
    discOk = !problem;
    $('check').className = 'note ' + (discOk ? 'good' : 'bad');
    $('check').textContent = problem || 'Mario Kart Wii PAL, revision 0. Ready.';
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
    setPhase('Uploading your disc…', received / file.size,
      `${(received / 2 ** 30).toFixed(2)} of ${(file.size / 2 ** 30).toFixed(2)} GB. Keep this tab open.`);
  }
  await api(`/api/jobs/${job.id}/start`, { method: 'POST' });
}

async function follow() {
  for (;;) {
    const s = await api(`/api/jobs/${job.id}`);
    $('log').textContent = (s.log || []).join('\n');
    if (s.state === 'uploading') return 'uploading';
    if (s.state === 'queued') setPhase('Waiting for the builder…', null, `Number ${s.queue_position} in line.`);
    if (s.state === 'building') setPhase(s.stage || 'Building…', s.progress, 'You can close this tab and come back; it picks up where it was.');
    if (s.state === 'failed') throw new Error(s.error || 'The build failed.');
    if (s.state === 'done') return s;
    await new Promise((r) => setTimeout(r, 3000));
  }
}

function finished(s) {
  show('done');
  $('rr-note').textContent = s.rr_version ? `Includes Retro Rewind ${s.rr_version}.` : '';
}

async function run(resume) {
  show('progress');
  try {
    if (!resume) {
      setPhase('Starting…', null);
      job = null;
      const created = await api('/api/jobs', { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: file.name, size: file.size, rr: $('rr').checked }) });
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
    finished(s);
  } catch (e) {
    if (e.status === 404) store.set(null);
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
    $('download').textContent = 'Download started (see your browser\'s downloads)';
  } catch (e) {
    $('download').textContent = `Download failed (${e.message}). Try again`;
  }
  setTimeout(() => { $('download').disabled = false; }, 5000);
});

// ---- start ----

(async () => {
  if (!SERVER || SERVER.includes('example.com')) {
    $('server').textContent = 'No builder is set up for this page yet.';
    $('server').className = 'pill bad';
    return;
  }
  try {
    const st = await api('/api/status');
    $('server').textContent = st.busy ? `Builder online, busy (${st.queued} waiting)` : 'Builder online';
    $('server').className = 'pill good';
  } catch {
    $('server').textContent = 'The builder is offline right now. Try again later.';
    $('server').className = 'pill bad';
    return;
  }
  const saved = store.get();
  if (saved) { job = saved; run(true); }
})();
