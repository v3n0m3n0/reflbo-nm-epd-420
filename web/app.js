'use strict';
/* reflbo web configurator (spec §10.3, D18): plain JS, no build step, English only (spec §5.8).
 * Every page reads the device's REST API; nothing is kept in the browser. */

const main = document.getElementById('main');
let auth = null; /* GET /api/auth */

/* ---- helpers ---- */

function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k === 'class') el.className = v;
    else if (k === 'text') el.textContent = v;
    else if (k === 'value') el.value = v;
    else if (k === 'checked' || k === 'selected' || k === 'disabled') el[k] = true;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat(Infinity)) {
    if (kid !== null && kid !== undefined && kid !== false) el.append(kid instanceof Node ? kid : String(kid));
  }
  return el;
}

const card = (title, ...kids) => h('section', { class: 'card' }, title ? h('h2', { text: title }) : null, ...kids);
const facts = (pairs) => h('dl', { class: 'facts' }, pairs.filter(Boolean).map(([k, v]) => [h('dt', { text: k }), h('dd', {}, v)]));
const actions = (...buttons) => h('div', { class: 'actions' }, ...buttons);
const button = (text, onclick, cls) => h('button', { class: 'btn' + (cls ? ' ' + cls : ''), type: 'button', onclick }, text);
const field = (label, input, hint) => [h('label', {}, label), input, hint ? h('p', { class: 'muted small' }, hint) : null];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const bytes = (s) => new TextEncoder().encode(s).length;
/* The device's messages are lowercase phrases: "wrong password" reads "Wrong password." */
const sentence = (m) => (m ? m[0].toUpperCase() + m.slice(1) + (/[.!?…]$/.test(m) ? '' : '.') : m);

class ApiError extends Error {
  constructor(message, status, relogin) {
    super(message);
    this.status = status;
    this.relogin = !!relogin; /* the session ended: start() shows the login page instead */
  }
}

async function api(method, path, body) {
  const init = { method, headers: {} };
  if (body instanceof Blob) {
    init.body = body;
    init.headers['Content-Type'] = 'application/octet-stream';
  } else if (method !== 'GET') {
    /* Spec §10.4: the device refuses changes that don't say they are JSON, as a cross-site form
     * can't; that includes requests without a body. */
    init.headers['Content-Type'] = 'application/json';
    if (body !== undefined) init.body = JSON.stringify(body);
  }
  let r;
  try {
    r = await fetch(path, init);
  } catch (e) {
    throw new ApiError("The device doesn't answer. Is this phone still on its network?", 0);
  }
  const json = (r.headers.get('Content-Type') || '').includes('application/json');
  const data = json ? await r.json() : await r.blob();
  if (r.status === 401 && !path.startsWith('/api/auth')) {
    start();
    throw new ApiError(sentence(data.error), 401, true);
  }
  if (!r.ok) throw new ApiError(sentence((json && data.error) || r.statusText), r.status);
  return data;
}

let toastTimer;
function toast(text) {
  const t = document.getElementById('toast');
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), 3000);
}

/* Runs `fn` with the element's buttons disabled; an error goes into `note`. */
async function busy(el, note, fn) {
  const buttons = [...el.querySelectorAll('button')];
  buttons.forEach((b) => (b.disabled = true));
  if (note) note.textContent = '';
  try {
    await fn();
  } catch (e) {
    if (note && !e.relogin) {
      note.className = 'bad';
      note.textContent = e.message;
    }
  } finally {
    buttons.forEach((b) => (b.disabled = false));
  }
}

function duration(s) {
  const d = Math.floor(s / 86400), hr = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d} d ${hr} h` : hr ? `${hr} h ${m} min` : `${m} min`;
}

function phoneZone() {
  const tz = Intl.DateTimeFormat().resolvedOptions().timeZone;
  return tz && ZONES[tz] ? tz : null;
}

/* POST /api/time: the phone's clock, and its zone if asked. */
async function setClock(withZone) {
  const body = { epoch: Math.round(Date.now() / 1000) };
  const tz = withZone ? phoneZone() : null;
  if (tz) Object.assign(body, { tz_iana: tz, tz_posix: ZONES[tz] });
  await api('POST', '/api/time', body);
}

function download(blob, name) {
  const a = h('a', { href: URL.createObjectURL(blob), download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

/* ---- start, password and login (D18) ---- */

async function start() {
  try {
    auth = await api('GET', '/api/auth');
  } catch (e) {
    main.replaceChildren(card('No answer', h('p', { text: e.message }), actions(button('Try again', start))));
    return;
  }
  document.getElementById('device').textContent = auth.host;
  const inside = auth.password_set && auth.logged_in;
  document.getElementById('nav').hidden = !inside;
  document.getElementById('done').hidden = !inside;
  if (!inside) return auth.password_set ? loginPage() : setupPage();
  window.onhashchange = route;
  route();
}

function passwordForm(title, text, submit, withOld) {
  const old = h('input', { type: 'password', autocomplete: 'current-password', required: true });
  const p1 = h('input', { type: 'password', autocomplete: 'new-password', minlength: 8, maxlength: 64, required: true });
  const p2 = h('input', { type: 'password', autocomplete: 'new-password', required: true });
  const note = h('p');
  const form = h('form', {
    onsubmit: (ev) => {
      ev.preventDefault();
      if (p1.value !== p2.value) {
        note.className = 'bad';
        note.textContent = 'The two passwords differ.';
        return;
      }
      busy(form, note, () => submit(p1.value, old.value, note, form));
    },
  },
  text ? h('p', { text }) : null,
  withOld ? field('Current password', old) : null,
  field('New password', p1, '8 to 64 characters.'),
  field('Repeat it', p2),
  note,
  actions(h('button', { class: 'btn primary', type: 'submit' }, title)));
  return form;
}

function setupPage() {
  if (!auth.on_ap) {
    main.replaceChildren(h('h1', { text: 'Welcome' }), card('Choose a password first',
      h('p', {}, 'Only a phone on the device\'s own Wi-Fi can choose the password for this page. Join ',
        h('b', { text: auth.ap_ssid }), ' (its password is on the device\'s screen), then open ',
        h('b', { text: 'http://192.168.4.1' }), '.')));
    return;
  }
  main.replaceChildren(h('h1', { text: 'Welcome' }), card('Choose a password for this page',
    passwordForm('Set password', 'Anyone who joins the device\'s Wi-Fi could open this page; the password keeps the ' +
      'settings to you. If you forget it: Menu ▸ Wi-Fi ▸ Reset web password on the device.',
      async (password) => {
        await api('POST', '/api/auth/setup', { password });
        toast('Password set');
        start();
      })));
}

function loginPage() {
  const pass = h('input', { type: 'password', autocomplete: 'current-password', required: true });
  const note = h('p');
  const form = h('form', {
    onsubmit: (ev) => {
      ev.preventDefault();
      busy(form, note, async () => {
        await api('POST', '/api/auth/login', { password: pass.value });
        start();
      });
    },
  }, field('Password', pass), note, actions(h('button', { class: 'btn primary', type: 'submit' }, 'Log in')));
  main.replaceChildren(h('h1', { text: 'Log in' }), card(null, form,
    h('p', { class: 'muted small', text: 'Forgot it? On the device: Menu ▸ Wi-Fi ▸ Reset web password.' })));
  pass.focus();
}

document.getElementById('done').onclick = async () => {
  let lan = false; /* sync mode Always on keeps the device on the network, and this page with it (spec §9.3) */
  try {
    const s = await api('GET', '/api/status');
    lan = !!s.sync && s.sync.mode === 'always' && s.wifi.state === 'station' && !s.wifi.ap_on;
  } catch (e) { /* the page decides below */ }
  try {
    await api('POST', '/api/done');
  } catch (e) {
    toast(e.message);
    return;
  }
  if (lan) {
    toast('Done. Sync mode Always on keeps the device on your network, so this page still works.');
    return;
  }
  document.getElementById('nav').hidden = true;
  document.getElementById('done').hidden = true;
  main.replaceChildren(card('Wi-Fi is off', h('p', { text: 'You can close this page. To come back, hold BOOT on the ' +
    'device for 3 seconds.' })));
};

/* ---- pages ---- */

const pages = { status: statusPage, wifi: wifiPage, place: placePage, sync: syncPage, radar: radarPage,
                solar: solarPage, pc: pcPage, lora: loraPage, device: devicePage, presets: presetsPage, firmware: firmwarePage,
                backup: backupPage };

function route() {
  const name = location.hash.slice(1) || 'status';
  if (name !== 'lora') {
    if (loraPollTimer !== null) {
      clearTimeout(loraPollTimer);
      loraPollTimer = null;
    }
    document.title = 'reflbo';
  }
  const page = pages[name] || statusPage;
  for (const a of document.querySelectorAll('nav a')) {
    const active = a.getAttribute('href') === '#' + name;
    a.classList.toggle('on', active);
    if (active) a.setAttribute('aria-current', 'page');
    else if (typeof a.removeAttribute === 'function') a.removeAttribute('aria-current');
    else if (a.attrs) delete a.attrs['aria-current'];
  }
  main.replaceChildren(h('p', { class: 'muted', text: 'Loading…' }));
  window.scrollTo(0, 0);
  page().catch((e) => {
    if (!e.relogin) main.replaceChildren(card('Something went wrong', h('p', { class: 'bad', text: e.message }),
      actions(button('Try again', route))));
  });
}

function wifiText(w) {
  switch (w.state) {
  case 'station': return `On ${w.ssid}${w.ip ? ' as ' + w.ip : ''}${w.ap_on ? `, and its own network ${w.ap_ssid}` : ''}`;
  case 'joining': return 'Connecting to a saved network…';
  case 'ap': return `Own network ${w.ap_ssid} only`;
  default: return 'Off';
  }
}

function loraState(l) {
  if (!l || !l.enabled) return 'Off';
  if (!l.initialized) return 'Module not detected';
  const mhz = ((l.freq_khz || 868000) / 1000).toFixed(3);
  return `Listening on ${mhz} MHz · SF${l.sf || 7} · ${l.bw_khz || 250} kHz · CR 4/${l.cr || 5}`;
}

async function statusPage() {
  let s = await api('GET', '/api/status');
  let clockNote = null;
  if (!s.time.valid) { /* no backup cell (D9): the phone's clock is better than none */
    await setClock(false);
    s = await api('GET', '/api/status');
    clockNote = h('p', { class: 'good small', text: 'The device had lost the time; it now has this phone\'s.' });
  }
  const off = Math.round(s.time.epoch - Date.now() / 1000);
  const b = s.battery, env = s.sensors;
  const shot = h('img', { class: 'screen', alt: 'What the device shows', src: '/api/screenshot.bmp?t=' + Date.now() });
  main.replaceChildren(
    h('h1', { text: 'Status' }),
    card('Screen', shot, actions(button('Refresh', () => statusPage().catch(() => {})))),
    card('Device', facts([
      ['Name', `${s.device.id}.local`],
      ['Firmware', s.device.firmware],
      ['Running for', duration(s.device.uptime_s)],
      ['Free memory', `${(s.device.free_heap / 1048576).toFixed(1)} MB`],
      ['Preset', `${s.preset.name}${s.preset.cycle ? ', cycling' : ''}`],
      s.lora ? ['LoRa', loraState(s.lora)] : null,
    ])),
    card('Time', facts([
      ['On the device', s.time.local.replace('T', ' ')],
      ['Time zone', s.time.tz_iana],
      ['Clock', Math.abs(off) <= 2 ? 'matches this phone' : `${Math.abs(off)} s ${off > 0 ? 'ahead of' : 'behind'} this phone`],
    ]), clockNote, actions(button('Set the clock from this phone', async (ev) => {
      await busy(ev.target.parentNode, null, async () => {
        await setClock(false);
        toast('Clock set');
        await statusPage();
      });
    }))),
    card('Battery and sensors', facts([
      ['Battery', b.percent !== undefined ? `${b.percent} % · ${(b.mv / 1000).toFixed(2)} V · ${(b.state === 'unknown' && b.usb) ? 'USB' : (b.usb ? `${b.state} (USB)` : b.state)}` : (b.usb ? 'no reading yet (USB)' : 'no reading yet')],
      b.days_left !== null ? ['Lasts', `about ${b.days_left.toFixed(1)} days`] : null,
      ['Temperature', env.temp_c !== null ? `${env.temp_c.toFixed(1)} °C` : '—'],
      ['Humidity', env.hum_pct !== null ? `${env.hum_pct.toFixed(0)} %` : '—'],
      env.age_s !== undefined ? ['Measured', `${duration(env.age_s)} ago`] : null,
    ])),
    card('Wi-Fi', facts([['Now', wifiText(s.wifi)], s.wifi.ap_on ? ['On its network', `${s.wifi.ap_clients} device(s)`] : null])),
    card('Sync', facts(syncFacts(s.sync)), actions(h('a', { class: 'btn', href: '#sync' }, 'Sync settings'))),
    card('This page', passwordForm('Change password', null, async (password, old, note, form) => {
      await api('POST', '/api/auth/password', { old, password });
      form.reset();
      note.className = 'good';
      note.textContent = 'Password changed.';
    }, true), actions(button('Log out', async () => {
      await api('POST', '/api/auth/logout');
      start();
    }), button('Restart the device', async () => {
      if (!confirm('Restart the device? Wi-Fi comes back on by itself.')) return;
      await api('POST', '/api/reboot');
      waitForRestart('Restarting…');
    }))),
  );
}

/* ---- Sync (spec §9.3, D25) ---- */

const SYNC_STEPS = [['wifi', 'Wi-Fi'], ['time', 'Time'], ['weather', 'Weather'], ['air', 'Air quality'],
                    ['radar', 'Radar'], ['solar', 'Solar forecast'], ['energy', 'House energy'], ['pc', 'PC']];
/* The data steps a sync can leave out (D35): the time always runs, as the clock and its trim need it. */
const STEP_SWITCHES = SYNC_STEPS.slice(2);
const SYNC_INTERVALS = [15, 30, 60, 120, 180, 360, 720, 1440];
const intervalLabel = (m) => (m < 60 ? `${m} min` : `${m / 60} h`);

function when(epoch) {
  if (!epoch) return '—';
  const d = new Date(epoch * 1000), now = new Date();
  const hm = d.toTimeString().slice(0, 5);
  const day = d.toDateString() === now.toDateString() ? 'today'
    : d.toDateString() === new Date(now.getTime() + 86400000).toDateString() ? 'tomorrow' : d.toISOString().slice(0, 10);
  return `${hm} ${day}`;
}

function syncFacts(sync) {
  if (!sync) return [];
  const last = sync.last;
  return [
    ['Last sync', sync.running ? `running: ${(SYNC_STEPS.find(([k]) => k === sync.step) || [0, '…'])[1]}`
      : !last ? 'not since the device started'
        : last.failed ? `${when(last.at)}: ${(SYNC_STEPS.find(([k]) => k === last.failed) || [0, last.failed])[1]} failed (${last.detail})`
          : `${when(last.at)}, all well`],
    ['Next', sync.next ? `${when(sync.next)}${sync.next_retry ? ', a retry' : ''}` : sync.mode === 'manual' ? 'when you ask' : '—'],
    sync.weather_at ? ['Weather from', when(sync.weather_at)] : null,
  ];
}

async function syncPage() {
  const [s, st] = await Promise.all([api('GET', '/api/settings'), api('GET', '/api/status')]);
  const sync = s.sync || {}, quiet = sync.quiet || {};
  const steps = h('dl', { class: 'facts' });
  const showSteps = (status) => {
    const last = status.sync.last;
    const detail = (k) => (last.details || {})[k] || (last.failed === k ? last.detail : '');
    steps.replaceChildren(...SYNC_STEPS.flatMap(([k, name]) => [h('dt', { text: name }),
      h('dd', { class: !last ? '' : last.steps[k] === 'ok' ? 'good' : last.steps[k] === 'failed' ? 'bad' : 'muted',
                text: !last ? '—' : last.steps[k] && last.steps[k] !== 'ok' && detail(k) ? `${last.steps[k]}: ${detail(k)}`
                  : last.steps[k] || '—' })]));
  };
  const summary = h('div');
  const rtc = h('p', { class: 'muted small' });
  const showStatus = (status) => {
    summary.replaceChildren(facts(syncFacts(status.sync)));
    showSteps(status);
    const r = status.time.rtc || {};
    rtc.textContent = `The clock chip's trim: ${r.trim_steps ?? 0} steps` +
      (r.drift_s_per_day !== undefined ? `; it drifted ${r.drift_s_per_day > 0 ? '+' : ''}${r.drift_s_per_day} s a day before the last sync.` : '.');
  };
  showStatus(st);
  const nowNote = h('p');
  const nowCard = card('Now', summary, steps, rtc, nowNote, actions(button('Sync now', () => busy(nowCard, nowNote, async () => {
    await api('POST', '/api/sync');
    nowNote.className = 'muted';
    nowNote.textContent = 'Syncing…';
    for (let i = 0; i < 60; i++) { /* a sync takes some seconds, 45 s at most (spec §9.3) */
      await sleep(1500);
      const now = await api('GET', '/api/status');
      showStatus(now);
      if (!now.sync.running) { /* the house's energy failing alone fails no sync (D36) */
        const last = now.sync.last, done = last && (last.ok ?? !last.failed);
        nowNote.className = done ? 'good' : 'bad';
        nowNote.textContent = !done ? 'The sync failed; see above.'
          : last.failed ? 'Synced; the house\'s energy failed, see above.' : 'Synced.';
        return;
      }
    }
  }), 'primary')));

  const modes = [['times', 'At set times'], ['interval', 'Every interval'], ['always', 'Always on'], ['manual', 'Only when asked']];
  let mode = modes.some(([m]) => m === sync.mode) ? sync.mode : 'times';
  const radios = modes.map(([m, label]) => h('input', { type: 'radio', name: 'mode', value: m, checked: m === mode,
                                                       onchange: () => { mode = m; showMode(); } }));
  let times = Array.isArray(sync.times) && sync.times.length ? [...sync.times] : ['05:30'];
  const timeList = h('div');
  const showTimes = () => timeList.replaceChildren(...times.map((v, i) => h('div', { class: 'row' },
    h('input', { type: 'time', value: v, onchange: (ev) => { times[i] = ev.target.value; } }),
    times.length > 1 ? button('Remove', () => { times.splice(i, 1); showTimes(); }) : null)),
  times.length < 8 ? button('Add a time', () => { times.push('12:00'); showTimes(); }) : null);
  showTimes();
  const interval = h('select', {}, SYNC_INTERVALS.map((m) => h('option', { value: String(m), selected: m === (sync.interval_min ?? 60) },
    intervalLabel(m))));
  interval.value = String(SYNC_INTERVALS.includes(sync.interval_min) ? sync.interval_min : 60);
  const timesBox = h('div', {}, h('label', {}, 'Times'), timeList);
  const intervalBox = h('div', {}, field('Every', interval));
  const alwaysNote = h('p', { class: 'muted small', text: 'Wi-Fi stays on and the device stays awake: meant for USB ' +
    'power, as it drains the battery in days. This page stays reachable on your network.' });
  const manualNote = h('p', { class: 'muted small', text: 'No sync starts by itself; the weather shows its age.' });
  const showMode = () => {
    timesBox.hidden = mode !== 'times';
    intervalBox.hidden = mode !== 'interval';
    alwaysNote.hidden = mode !== 'always';
    manualNote.hidden = mode !== 'manual';
  };
  showMode();
  const shortcut = (label, m, set) => button(label, () => {
    mode = m;
    radios.forEach((r) => (r.checked = r.value === m));
    set();
    showTimes();
    showMode();
  });
  const quietOn = h('input', { type: 'checkbox', checked: !!quiet.enabled });
  const from = h('input', { type: 'time', value: quiet.from || '23:00' });
  const to = h('input', { type: 'time', value: quiet.to || '06:00' });
  const minutes = (v) => Number(v.slice(0, 2)) * 60 + Number(v.slice(3, 5));
  const inQuiet = (v) => {
    const a = minutes(from.value), b = minutes(to.value), m = minutes(v);
    return a < b ? m >= a && m < b : a > b ? m >= a || m < b : false;
  };
  const note = h('p');
  const schedCard = card('Schedule',
    h('div', { class: 'actions' }, shortcut('Battery saver', 'times', () => { times = ['05:30']; }),
      shortcut('Balanced', 'interval', () => { interval.value = '60'; }), shortcut('Always connected', 'always', () => {})),
    ...modes.map(([m, label], i) => h('label', { class: 'check' }, radios[i], label)),
    timesBox, intervalBox, alwaysNote, manualNote,
    h('h3', { text: 'Quiet hours' }),
    h('p', { class: 'muted small', text: 'No sync starts by itself in these hours; one runs as they end. In "Always on", ' +
      'Wi-Fi goes off for them.' }),
    h('label', { class: 'check' }, quietOn, 'Quiet hours'),
    h('div', { class: 'row' }, h('div', {}, field('From', from)), h('div', {}, field('To', to))),
    note,
    actions(button('Save', () => busy(schedCard, note, async () => {
      const valid = (v) => /^([01]\d|2[0-3]):[0-5]\d$/.test(v);
      const list = [...new Set(times.filter(valid))].sort();
      if (mode === 'times' && !list.length) throw new ApiError('Add a time.');
      if (quietOn.checked && (!valid(from.value) || !valid(to.value) || from.value === to.value)) {
        throw new ApiError('Quiet hours need a start and an end that differ.');
      }
      await api('PATCH', '/api/settings', { sync: { mode, times: list.length ? list : ['05:30'],
        interval_min: Number(interval.value), quiet: { enabled: quietOn.checked, from: from.value, to: to.value } } });
      times = list.length ? list : ['05:30'];
      showTimes();
      const moved = mode === 'times' && quietOn.checked ? list.filter(inQuiet) : [];
      note.className = moved.length ? 'muted' : 'good';
      note.textContent = moved.length ? `Saved. ${moved.join(', ')} falls in the quiet hours: it runs at ${to.value}.` : 'Saved.';
      toast('Saved');
    }), 'primary')));
  const on = new Set(Array.isArray(sync.steps) ? sync.steps : STEP_SWITCHES.map(([k]) => k));
  const switches = STEP_SWITCHES.map(([k, name]) => h('label', { class: 'check' },
    h('input', { type: 'checkbox', checked: on.has(k) }), name));
  const stepsNote = h('p');
  const stepsCard = card('Steps',
    h('p', { class: 'muted small', text: 'A step that is off makes no requests, and its data age out as usual. The time ' +
      'always runs: the clock and its trim need it.' }),
    ...switches, stepsNote,
    actions(button('Save steps', () => busy(stepsCard, stepsNote, async () => {
      const list = STEP_SWITCHES.filter((_, i) => switches[i].children[0].checked).map(([k]) => k);
      await api('PATCH', '/api/settings', { sync: { steps: list } });
      stepsNote.className = 'good';
      stepsNote.textContent = 'Saved.';
      toast('Saved');
    }), 'primary')));
  main.replaceChildren(h('h1', { text: 'Sync' }), nowCard, schedCard, stepsCard);
}

/* ---- Wi-Fi (spec §10.1, §10.2) ---- */

function bars(rssi) {
  const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  return '▮'.repeat(n) + '▯'.repeat(4 - n);
}

const TEST_TEXT = {
  'wrong password': 'The password is wrong.',
  'not found': "The device can't see this network. It uses 2.4 GHz only: check the name, and that the router is in reach.",
  failed: "It couldn't connect.",
  invalid: 'The name or password is not valid.',
};

/* The test runs on the device while this phone may drop off its network for a moment (spec §10.2). */
async function tryNetwork(ssid, password, note) {
  await api('POST', '/api/wifi/networks', { ssid, password });
  note.className = 'muted';
  note.textContent = `Trying ${ssid}…`;
  for (const until = Date.now() + 60000; Date.now() < until;) {
    await sleep(1000);
    let n;
    try {
      n = await api('GET', '/api/wifi/networks');
    } catch (e) {
      if (e.relogin) throw e;
      note.textContent = `Trying ${ssid}… This phone lost the device's network for a moment; waiting for it.`;
      continue;
    }
    if (n.test.ssid !== ssid || n.test.result === 'testing') continue;
    if (n.test.result === 'connected') return n;
    throw new ApiError(TEST_TEXT[n.test.result] || n.test.result, 409);
  }
  throw new ApiError('The device gave no answer in time.', 0);
}

async function wifiPage() {
  const n = await api('GET', '/api/wifi/networks');
  const ssid = h('input', { type: 'text', maxlength: 32, autocapitalize: 'off', autocomplete: 'off', required: true });
  const pass = h('input', { type: 'password', maxlength: 64, autocomplete: 'off' });
  const note = h('p');
  const found = h('ul', { class: 'list' });
  const scanNote = h('p', { class: 'muted small' });
  const scan = () => busy(found.parentNode, scanNote, async () => {
    scanNote.textContent = 'Looking…';
    const r = await api('GET', '/api/wifi/scan');
    scanNote.textContent = r.networks.length ? 'Tap one to use its name.' : 'No networks in sight.';
    found.replaceChildren(...r.networks.map((ap) => h('li', {},
      h('button', { class: 'link', type: 'button', onclick: () => { ssid.value = ap.ssid; pass.focus(); } }, ap.ssid),
      h('span', { class: 'muted small', text: ap.open ? 'open' : '' }),
      h('span', { class: 'bars', title: `${ap.rssi} dBm`, text: bars(ap.rssi) }))));
  });
  const valid = () => {
    if (bytes(ssid.value) < 1 || bytes(ssid.value) > 32) return 'The name needs 1 to 32 characters.';
    if (pass.value && (pass.value.length < 8 || pass.value.length > 63) && !/^[0-9a-fA-F]{64}$/.test(pass.value)) {
      return 'A Wi-Fi password has 8 to 63 characters.';
    }
    return null;
  };
  const form = h('form', {
    onsubmit: (ev) => {
      ev.preventDefault();
      const bad = valid();
      if (bad) {
        note.className = 'bad';
        note.textContent = bad;
        return;
      }
      busy(form, note, async () => {
        const r = await tryNetwork(ssid.value, pass.value, note);
        note.className = 'good';
        note.textContent = `Connected: the device is on ${r.ssid} as ${r.ip}. On that network, open ` +
          `http://${auth.host}.local or http://${r.ip}.`;
        pass.value = '';
        refreshLists(await api('GET', '/api/wifi/networks'));
      });
    },
  },
  field('Network name', ssid), field('Password', pass, 'Leave it empty for an open network.'), note,
  actions(h('button', { class: 'btn primary', type: 'submit' }, 'Connect'),
    button('Save without trying', () => {
      const bad = valid();
      if (bad) {
        note.className = 'bad';
        note.textContent = bad;
        return;
      }
      busy(form, note, async () => {
        await api('POST', '/api/wifi/networks', { ssid: ssid.value, password: pass.value, test: false });
        note.className = 'good';
        note.textContent = `Saved: the device tries ${ssid.value} the next time Wi-Fi comes on.`;
        pass.value = '';
        refreshLists(await api('GET', '/api/wifi/networks'));
      });
    })));
  const savedRows = (list) => list.saved.length ? list.saved.map((name) => h('li', {},
    h('span', { class: 'grow', text: name }),
    button('Forget', async (ev) => {
      if (!confirm(`Forget ${name}?`)) return;
      await busy(ev.target.parentNode, null, async () => {
        await api('DELETE', '/api/wifi/networks?ssid=' + encodeURIComponent(name));
        refreshLists(await api('GET', '/api/wifi/networks'));
      });
    }))) : [h('li', { class: 'muted', text: 'None yet.' })];
  const saved = h('ul', { class: 'list' }, ...savedRows(n));
  const now = h('div', {}, facts([['Wi-Fi', wifiText(Object.assign({ ap_ssid: auth.ap_ssid }, n))]]));
  const refreshLists = (list) => {
    saved.replaceChildren(...savedRows(list));
    now.replaceChildren(facts([['Wi-Fi', wifiText(Object.assign({ ap_ssid: auth.ap_ssid }, list))]]));
  };
  const lanNote = !auth.on_ap && n.state === 'station'
    ? h('p', { class: 'muted small', text: `This phone reaches the device over ${n.ssid}. Trying another network may ` +
      'cut this page off: "Save without trying" is safer from here.' }) : null;
  main.replaceChildren(
    h('h1', { text: 'Wi-Fi' }),
    card('Now', now),
    card('Saved networks', saved, h('p', { class: 'muted small', text: 'Tried in this order, at most 5.' })),
    card('Add a network', h('div', {}, found, scanNote, actions(button('Look for networks', scan))), form, lanNote),
  );
  scan();
}

/* ---- Location and time ---- */

/* A place by its name, through the device, which is online (spec §10.3): `onPick` gets the one tapped. */
function placeSearch(note, onPick) {
  const query = h('input', { type: 'search', placeholder: 'A town or city' });
  const found = h('div');
  const search = () => busy(box, note, async () => {
    if (!query.value.trim()) throw new ApiError('Type a place to look for.');
    const r = await api('GET', '/api/geocode?q=' + encodeURIComponent(query.value.trim()));
    found.replaceChildren(...(r.places.length ? r.places.map((pl) => button(
      `${pl.name}${pl.region ? ', ' + pl.region : ''}${pl.country ? ', ' + pl.country : ''}`, () => {
        onPick(pl);
        found.replaceChildren(h('p', { class: 'muted small', text: `${pl.name}: ${pl.lat}, ${pl.lon}. Save it below.` }));
      })) : [h('p', { class: 'muted small', text: 'Nothing found by that name.' })]));
  });
  const box = h('div', {}, h('div', { class: 'row' }, h('div', {}, field('Find a place', query)),
    actions(button('Search', search))), found);
  return box;
}

async function placePage() {
  const s = await api('GET', '/api/settings');
  const loc = s.location || {}, time = s.time || {};
  const name = h('input', { type: 'text', maxlength: 31, value: loc.name || '' });
  const lat = h('input', { type: 'number', step: 'any', min: -90, max: 90, value: loc.lat ?? '' });
  const lon = h('input', { type: 'number', step: 'any', min: -180, max: 180, value: loc.lon ?? '' });
  const locNote = h('p');
  const locCard = card('Location', h('p', { class: 'muted small', text: 'For sunrise, sunset and the weather. ' +
    'Decimal degrees: north and east are positive.' }),
  placeSearch(locNote, (pl) => {
    name.value = pl.name.slice(0, 31);
    lat.value = String(pl.lat);
    lon.value = String(pl.lon);
  }),
  field('Name', name), h('div', { class: 'row' }, h('div', {}, field('Latitude', lat)), h('div', {}, field('Longitude', lon))),
  locNote, actions(button('Save location', () => busy(locCard, locNote, async () => {
    const la = Number(lat.value), lo = Number(lon.value);
    if (!name.value.trim() || bytes(name.value) > 31) throw new ApiError('The name needs 1 to 31 bytes.');
    if (lat.value === '' || !(la >= -90 && la <= 90)) throw new ApiError('Latitude: -90 to 90.');
    if (lon.value === '' || !(lo >= -180 && lo <= 180)) throw new ApiError('Longitude: -180 to 180.');
    await api('PATCH', '/api/settings', { location: { name: name.value.trim(), lat: la, lon: lo } });
    toast('Location saved');
  }), 'primary')));

  const filter = h('input', { type: 'search', placeholder: 'Filter: Prague, New_York, UTC…' });
  const zone = h('select', { size: 8 });
  const fillZones = () => {
    const q = filter.value.trim().toLowerCase().replace(/ /g, '_');
    const names = Object.keys(ZONES).filter((z) => !q || z.toLowerCase().includes(q));
    if (time.tz_iana && !names.includes(time.tz_iana) && !q) names.unshift(time.tz_iana);
    zone.replaceChildren(...names.map((z) => h('option', { value: z, selected: z === time.tz_iana }, z.replace(/_/g, ' '))));
  };
  filter.oninput = fillZones;
  fillZones();
  const h24 = h('input', { type: 'checkbox', checked: time.clock_24h !== false });
  const tzNote = h('p');
  const mine = phoneZone();
  const tzCard = card('Time zone', field('Zone', filter), zone,
    mine && mine !== time.tz_iana ? h('p', { class: 'muted small' }, `This phone: ${mine.replace(/_/g, ' ')}. `,
      h('a', { href: '#', onclick: (ev) => { ev.preventDefault(); filter.value = ''; time.tz_iana = mine; fillZones(); } },
        'Pick it')) : null,
    h('label', { class: 'check' }, h24, '24-hour clock'), tzNote,
    actions(button('Save time zone', () => busy(tzCard, tzNote, async () => {
      const z = zone.value || time.tz_iana;
      if (!ZONES[z] && z !== time.tz_iana) throw new ApiError('Pick a zone from the list.');
      const t = { clock_24h: h24.checked };
      if (ZONES[z]) Object.assign(t, { tz_iana: z, tz_posix: ZONES[z] });
      await api('PATCH', '/api/settings', { time: t });
      time.tz_iana = z;
      toast('Time zone saved');
    }), 'primary')));

  const clockNote = h('p');
  const clockCard = card('Clock', h('p', { class: 'muted small', text: 'Sets the device from this phone\'s clock. ' +
    'Without a backup cell the device forgets the time when it is switched off.' }), clockNote,
  actions(button('Set the clock from this phone', () => busy(clockCard, clockNote, async () => {
    await setClock(false);
    toast('Clock set');
  }))));
  main.replaceChildren(h('h1', { text: 'Location and time' }), locCard, tzCard, clockCard);
}

/* ---- Radar (spec §10.3, §11.1-§11.3, D22, D23, D28) ---- */

const ZOOMS = Array.from({ length: 21 }, (_, i) => 4 + i / 4); /* 4 to 9 in quarters (spec §14.3) */
const RANGES = [10, 15, 20, 25, 30, 40, 50, 60, 75, 100];
/* The ground the Radar layout's 400 px span at a zoom and latitude, in web Mercator (spec §11.1). */
const acrossKm = (zoom, lat) => Math.round(400 * 156543.03 * Math.cos((lat || 0) * Math.PI / 180) / 2 ** zoom / 1000);

/* A radar's centre (spec §11.1): the location, which it then follows, or a place of its own. */
function centreEditor(group, settings, own, note) {
  const loc = settings.location || {};
  const ownSet = typeof (own || {}).lat === 'number' && typeof own.lon === 'number';
  const lat = h('input', { type: 'number', step: 'any', min: -85, max: 85, value: ownSet ? own.lat : loc.lat ?? '' });
  const lon = h('input', { type: 'number', step: 'any', min: -180, max: 180, value: ownSet ? own.lon : loc.lon ?? '' });
  const where = h('div', {}, placeSearch(note, (pl) => { lat.value = String(pl.lat); lon.value = String(pl.lon); }),
    h('div', { class: 'row' }, h('div', {}, field('Latitude', lat)), h('div', {}, field('Longitude', lon))));
  where.hidden = !ownSet;
  const atHome = h('input', { type: 'radio', name: group, checked: !ownSet, onchange: () => { where.hidden = true; } });
  const elsewhere = h('input', { type: 'radio', name: group, checked: ownSet, onchange: () => { where.hidden = false; } });
  return {
    els: [h('label', {}, 'Centre'), h('label', { class: 'check' }, atHome, `The location${loc.name ? ` (${loc.name})` : ''}`),
          h('label', { class: 'check' }, elsewhere, 'A place of its own'), where],
    /* the patch's coordinates: null follows the location again, as a merge patch removes them */
    value() {
      if (atHome.checked) return { lat: null, lon: null };
      const la = Number(lat.value), lo = Number(lon.value);
      if (lat.value === '' || !(la >= -85 && la <= 85)) throw new ApiError('Latitude: -85 to 85.');
      if (lon.value === '' || !(lo >= -180 && lo <= 180)) throw new ApiError('Longitude: -180 to 180.');
      return { lat: la, lon: lo };
    },
  };
}

/* The device draws a radar's layout as saved (POST /api/preview.bmp): one preset on it. */
async function radarPreview(img, layout) {
  const doc = { schema: 1, active: 'preview', presets: [{ id: 'preview', name: 'Preview', layout,
                                                          options: { status_clock: true } }] };
  const bmp = await api('POST', '/api/preview.bmp?preset=preview', doc);
  if (img.src && img.src.startsWith('blob:')) URL.revokeObjectURL(img.src);
  img.src = URL.createObjectURL(bmp);
}

const link = (href, text) => h('a', { href, target: '_blank', rel: 'noopener' }, text);

async function radarPage() {
  const [s, st] = await Promise.all([api('GET', '/api/settings'), api('GET', '/api/status')]);
  const radar = s.radar || {}, wx = radar.weather || {}, fl = radar.flights || {}, now = st.radar || {};
  const always = (s.sync || {}).mode === 'always';
  const lat = typeof wx.lat === 'number' ? wx.lat : (s.location || {}).lat;

  const wxNote = h('p');
  const wxCentre = centreEditor('wx-centre', s, wx, wxNote);
  const zoom = h('select', { name: 'zoom' }, ZOOMS.map((z) => h('option', { value: String(z), selected: z === (wx.zoom ?? 6.5) },
    `${z}: about ${acrossKm(z, lat)} km across`)));
  zoom.value = String(ZOOMS.includes(wx.zoom) ? wx.zoom : 6.5);
  const WX_SOURCES = [
    ['auto', 'Automatic (DWD 1×1 km radar, else RainViewer)'],
    ['rainviewer', 'RainViewer (10 min live radar)'],
    ['dwd', 'DWD OpenData WMS (Radar_rv / ICON-EU)'],
  ];
  const sourceSel = h('select', { name: 'source' }, WX_SOURCES.map(([v, label]) =>
    h('option', { value: v, selected: v === (wx.source || 'auto') }, label)));
  sourceSel.value = wx.source || 'auto';
  const wxImg = h('img', { class: 'screen', alt: 'The Radar layout' });
  const w = now.weather || {};
  const wxCard = card('Weather radar', wxImg, facts([
    ['Source', w.source === 'rainviewer' ? 'RainViewer (10 min live radar)' : 'DWD'],
    ['Newest frame', w.frame_at ? when(w.frame_at) : 'none yet'],
    ['Frames kept', `${w.frames ?? 0}${always ? ', the last hour for the loop' : ''}`],
    w.error ? ['Last fetch', `failed: ${w.error}`] : null,
  ]),
  h('p', { class: 'muted small', text: 'A frame comes with each sync, and every 5 minutes in sync mode Always on ' +
    '(DWD Radar_rv: 5 min, ICON-EU: 60 min; RainViewer: 10 min). Auto uses DWD 1×1 km Radar_rv where covered, ' +
    'RainViewer live 10 min radar elsewhere with DWD ICON-EU fallback. Rain for a new centre, zoom, or source fetches a fresh frame. ' +
    'In sync mode Always on, BOOT on the Radar layout plays the last hour; otherwise BOOT there syncs for a fresh frame.' }),
  wxCentre.els, field('Zoom', zoom), field('Source', sourceSel), wxNote,
  actions(button('Save weather radar', () => busy(wxCard, wxNote, async () => {
    const weatherPatch = { ...wxCentre.value(), zoom: Number(zoom.value) };
    if (sourceSel.value !== 'auto') {
      weatherPatch.source = sourceSel.value;
    } else if (wx.source) {
      weatherPatch.source = null;
    }
    await api('PATCH', '/api/settings', { radar: { weather: weatherPatch } });
    toast('Weather radar saved');
    await radarPreview(wxImg, 'radar');
  }), 'primary')),
  h('p', { class: 'muted small' }, 'Rain: ', link('https://opendata.dwd.de', 'DWD OpenData WMS'),
    ' (Radar_rv 1×1 km / ICON-EU); ', link('https://www.rainviewer.com', 'RainViewer'), ' (10 min live radar).'));

  const flNote = h('p');
  const flCentre = centreEditor('fl-centre', s, fl, flNote);
  const ranges = RANGES.includes(fl.range_km) || fl.range_km === undefined ? RANGES : [...RANGES, fl.range_km].sort((a, b) => a - b);
  const range = h('select', { name: 'range' }, ranges.map((km) => h('option', { value: String(km), selected: km === (fl.range_km ?? 50) },
    `${km} km`)));
  range.value = String(fl.range_km ?? 50);
  const minAlt = h('input', { type: 'number', name: 'min_alt_ft', min: 0, max: 60000, step: 100, value: fl.min_alt_ft ?? 0 });
  const ground = h('input', { type: 'checkbox', name: 'ground', checked: !!fl.ground });
  const max = h('input', { type: 'number', name: 'max', min: 1, max: 100, step: 1, value: fl.max ?? 100 });
  const f = now.flights || {};
  const flImg = h('img', { class: 'screen', alt: 'The Flights layout' });
  const flCard = card('Flight radar',
    always ? null : h('p', { class: 'bad small' }, 'It runs only in sync mode Always on, set on the ',
      h('a', { href: '#sync' }, 'Sync'), ' page.'),
    flImg, facts([['Now', f.on ? `${f.aircraft} aircraft${f.updated ? ' at ' + when(f.updated) : ''}`
      : 'resting: it asks while the Flights view is on the screen'],
    f.failed ? ['Last poll', `failed: ${f.error || 'no reason given'}`] : null,
    f.routes_paused_until ? ['Routes', `paused by adsb.lol until ${when(f.routes_paused_until)}`] : null]),
    h('p', { class: 'muted small', text: 'While the Flights view shows, it asks adsb.fi every 5 to 15 s, more often for ' +
      'a smaller range, and the nearest aircraft\'s route comes from adsb.lol.' }),
    flCentre.els, field('Range', range, 'From the centre to the map\'s top edge.'),
    field('Lowest altitude (ft)', minAlt), h('label', { class: 'check' }, ground, 'Aircraft on the ground too'),
    field('Aircraft at most', max), flNote,
    actions(button('Save flight radar', () => busy(flCard, flNote, async () => {
      const lo = Number(minAlt.value), mx = Number(max.value);
      if (minAlt.value === '' || !(lo >= 0 && lo <= 60000)) throw new ApiError('Lowest altitude: 0 to 60000 ft.');
      if (max.value === '' || !Number.isInteger(mx) || mx < 1 || mx > 100) throw new ApiError('Aircraft at most: 1 to 100.');
      await api('PATCH', '/api/settings', { radar: { flights: { ...flCentre.value(), range_km: Number(range.value),
        min_alt_ft: Math.round(lo), ground: ground.checked, max: mx } } });
      toast('Flight radar saved');
      await radarPreview(flImg, 'flights');
    }), 'primary')),
    h('p', { class: 'muted small' }, 'Aircraft: ', link('https://adsb.fi', 'adsb.fi'), '; routes: ',
      link('https://adsb.lol', 'adsb.lol'), '.'));
  const mapCredit = h('p', { class: 'muted small' }, 'Map: borders and towns from ',
    link('https://www.naturalearthdata.com', 'Natural Earth'), ', airports from ',
    link('https://ourairports.com', 'OurAirports'), '; more towns in central Europe from ',
    link('https://www.geonames.org', 'GeoNames'), ', CC BY 4.0.');
  main.replaceChildren(h('h1', { text: 'Radar' }), wxCard, flCard, mapCredit);
  await Promise.all([radarPreview(wxImg, 'radar'), radarPreview(flImg, 'flights')]).catch(() => {});
}

/* ---- Device: the settings the menu also has (spec §5.7, D19) ---- */

/* ---- Solar (spec §11.5, §11.6, D35, D36, D37) ---- */

const SOLAR_SOURCES = [['off', 'Off'], ['open-meteo', 'Open-Meteo, through the device\'s own model'],
                       ['forecast-solar', 'Forecast.Solar'], ['solcast', 'Solcast']];
const ENERGY_SOURCES = [['off', 'Off'], ['solax-dev', 'SolaX Cloud, Developer API'],
                        ['solax', 'SolaX Cloud, Token ID']];
const SOLAX_REGIONS = [['eu', 'Europe'], ['cn', 'China'], ['in', 'India']];
const BATTERY_MODES = [['auto', 'Automatic: a hybrid inverter, or a charge above 0 %'], ['on', 'Shown'],
                       ['off', 'Hidden']];

function choose(options, value) {
  const el = h('select', {}, options.map(([v, t]) => h('option', { value: v, selected: v === value }, t)));
  el.value = value;
  return el;
}

/* A number input bound to obj[key]. */
function numberInput(obj, key, min, max, step) {
  return h('input', { type: 'number', min: String(min), max: String(max), step: String(step), value: String(obj[key]),
                      oninput: (ev) => { obj[key] = Number(ev.target.value); } });
}

/* A write-only key (spec §10.3): empty, it says whether one is set; what you type is sent; Clear sends null. */
function secretInput(label, isSet, hint) {
  /* new-password: a password manager never fills the device's web password into a key, which the requests send on */
  const input = h('input', { type: 'password', autocomplete: 'new-password', 'data-1p-ignore': '', 'data-lpignore': 'true',
                             placeholder: isSet ? 'set: type to replace it' : '' });
  const note = h('p', { class: 'muted small', text: isSet ? 'A key is set.' : 'None is set.' });
  const s = { input, note, isSet, cleared: false };
  s.value = () => (input.value ? input.value : s.cleared ? null : undefined);
  s.saved = (nowSet) => {
    s.isSet = !!nowSet;
    s.cleared = false;
    input.value = '';
    input.placeholder = s.isSet ? 'set: type to replace it' : '';
    note.textContent = s.isSet ? 'A key is set.' : 'None is set.';
  };
  s.el = [field(label, input, hint), note,
          isSet ? actions(button('Clear the key', () => { s.cleared = true; input.value = ''; note.textContent = 'It goes when you save.'; }))
                : null];
  return s;
}

async function solarPage() {
  const [s, st] = await Promise.all([api('GET', '/api/settings'), api('GET', '/api/status')]);
  const solar = s.solar || {}, energy = s.energy || {}, keys = solar.keys || {}, ekeys = energy.keys || {};

  const source = choose(SOLAR_SOURCES, solar.source || 'off');
  const planes = (Array.isArray(solar.planes) && solar.planes.length ? solar.planes : [{ kwp: 5, tilt: 35, azimuth: 0 }])
    .slice(0, 2).map((q) => ({ kwp: q.kwp, tilt: q.tilt, azimuth: q.azimuth }));
  const planeBox = h('div');
  const showPlanes = () => planeBox.replaceChildren(...[...planes.map((q, i) => h('div', {},
    h('h3', { text: planes.length > 1 ? `Plane ${i + 1}` : 'The roof' }),
    h('div', { class: 'row' }, h('div', {}, field('kWp', numberInput(q, 'kwp', 0.1, 100, 0.01))),
      h('div', {}, field('Tilt (°)', numberInput(q, 'tilt', 0, 90, 1))),
      h('div', {}, field('Azimuth (°)', numberInput(q, 'azimuth', -180, 180, 1)))),
    planes.length > 1 ? actions(button('Remove this plane', () => { planes.splice(i, 1); showPlanes(); })) : null)),
  h('p', { class: 'muted small', text: 'Azimuth 0 is south, -90 east, 90 west.' }),
  planes.length < 2 ? actions(button('Add a second plane', () => { planes.push({ kwp: 2, tilt: 35, azimuth: -90 }); showPlanes(); }))
    : null].filter(Boolean));
  showPlanes();
  const model = { losses: solar.losses_pct ?? 14, inverter: solar.inverter_kw ?? 0 };
  const modelBox = h('div', { class: 'row' },
    h('div', {}, field('Losses (%)', numberInput(model, 'losses', 0, 50, 1))),
    h('div', {}, field('Inverter limit (kW)', numberInput(model, 'inverter', 0, 100, 0.01), '0 for none.')));
  const fsKey = secretInput('Forecast.Solar key (optional)', !!keys.fs_key,
    'Without one: one plane, hourly values, today and tomorrow. A second plane needs one.');
  const scKey = secretInput('Solcast API key', !!keys.solcast_key);
  const sites = [secretInput('First site id', (keys.solcast_sites || 0) >= 1),
                 secretInput('Second site id (optional)', (keys.solcast_sites || 0) >= 2)];
  const fsBox = h('div', {}, fsKey.el);
  const scBox = h('div', {}, h('p', { class: 'muted small', text: 'Solcast knows the roof: its tilt, azimuth and kWp ' +
    'are set on solcast.com. A hobbyist account has 10 calls a day: the device asks at most every 3 h with one site, ' +
    'every 6 h with two, and keeps the forecast it has in between.' }), scKey.el, sites[0].el, sites[1].el);
  const show = () => {
    const v = source.value;
    planeBox.hidden = v !== 'open-meteo' && v !== 'forecast-solar';
    modelBox.hidden = v !== 'open-meteo';
    fsBox.hidden = v !== 'forecast-solar';
    scBox.hidden = v !== 'solcast';
  };
  source.addEventListener('change', show);
  show();

  const esource = choose(ENERGY_SOURCES, energy.source || 'off');
  const region = choose(SOLAX_REGIONS, energy.region || 'eu');
  const clientId = secretInput('Client ID', !!ekeys.solax_client_id);
  const secret = secretInput('Client Secret', !!ekeys.solax_client_secret);
  const devBox = h('div', {}, h('p', { class: 'muted small', text: 'From developer.solaxcloud.com: the Client ID and ' +
    'Client Secret of the application you created there. The device logs in with them and finds the plant and its ' +
    'inverter, battery and meter itself; today\'s totals are SolaX\'s own.' }), clientId.el, secret.el,
  field('Region', region, 'Where the SolaX account is.'));
  const token = secretInput('Token ID', !!ekeys.solax_token, 'From the API page of solaxcloud.com, for accounts ' +
    'that have one.');
  const sn = secretInput('Registration number', !!ekeys.solax_sn, 'The dongle\'s, on its label.');
  const battery = choose(BATTERY_MODES, energy.battery || 'auto');
  const solaxBox = h('div', {}, token.el, sn.el);
  const eshow = () => { solaxBox.hidden = esource.value !== 'solax'; devBox.hidden = esource.value !== 'solax-dev'; };
  esource.addEventListener('change', eshow);
  eshow();

  const note = h('p');
  const save = card(null, note, actions(button('Save', () => busy(save, note, async () => {
    const touched = sites.some((x) => x.value() !== undefined);
    if (touched && sites.some((x) => x.isSet && x.value() === undefined)) {
      throw new ApiError('Type both site ids, or clear the one you don\'t want.');
    }
    const out = { solar: { source: source.value, planes: planes.map((q) => ({ kwp: Number(q.kwp), tilt: Number(q.tilt),
                                                                             azimuth: Number(q.azimuth) })),
                           losses_pct: Number(model.losses), inverter_kw: Number(model.inverter) },
                  energy: { source: esource.value, region: region.value, battery: battery.value } };
    const put = (obj, key, v) => { if (v !== undefined) obj[key] = v; };
    put(out.solar, 'fs_key', fsKey.value());
    put(out.solar, 'solcast_key', scKey.value());
    if (touched) out.solar.solcast_sites = sites.map((x) => x.value() || '').filter(Boolean);
    put(out.energy, 'solax_token', token.value());
    put(out.energy, 'solax_sn', sn.value());
    put(out.energy, 'solax_client_id', clientId.value());
    put(out.energy, 'solax_client_secret', secret.value());
    await api('PATCH', '/api/settings', out);
    note.className = 'good';
    note.textContent = 'Saved.';
    toast('Saved');
  }), 'primary')));

  const facts2 = h('div');
  const showNow = (status) => {
    const f = status.solar || {}, e = status.energy || {};
    facts2.replaceChildren(facts([
      ['Forecast', f.fetched_at ? `from ${when(f.fetched_at)}` : 'none yet'],
      f.kept ? ['Its last step', f.kept !== 'kept' ? `kept the forecast (${f.kept})` : 'kept the forecast: Solcast ' +
        `is asked again from ${f.next_at ? when(f.next_at) : 'its next sync'}, within its 10 calls a day`] : null,
      f.error ? ['Its last call', `failed: ${f.error}`] : null,
      ['House reading', e.reading_at ? `from ${when(e.reading_at)}` : 'none yet'],
      e.source === 'solax-dev' ? ['SolaX plant', e.plant || 'not found yet'] : null,
      e.error ? ['Its last step', `failed: ${e.error}`] : null,
    ]));
  };
  showNow(st);
  const checkNote = h('p');
  const nowCard = card('Now', facts2, checkNote, actions(button('Check now', () => busy(nowCard, checkNote, async () => {
    const before = await api('GET', '/api/status');
    const tried = (x) => `${(x.solar || {}).tried_at}/${(x.energy || {}).tried_at}`;
    await api('POST', '/api/solar/check');
    checkNote.className = 'muted';
    checkNote.textContent = 'Checking…';
    for (let i = 0; i < 40; i++) { /* the two steps take some seconds, 45 s at most (spec §9.3) */
      await sleep(1500);
      const now = await api('GET', '/api/status');
      if (tried(now) !== tried(before)) {
        showNow(now);
        checkNote.className = 'good';
        checkNote.textContent = 'Checked.';
        return;
      }
    }
    checkNote.textContent = 'No result yet; see Sync.';
  }))));

  main.replaceChildren(h('h1', { text: 'Solar' }),
    card('PV forecast', field('Source', source), planeBox, modelBox, fsBox, scBox),
    card('The house\'s energy', field('Source', esource), devBox, solaxBox, field('Home battery', battery)),
    save, nowCard,
    card('Credits', h('p', { class: 'muted small', text: 'Forecasts: Open-Meteo (CC BY 4.0), Forecast.Solar (CC BY-SA ' +
      '4.0), Solcast (for personal use only, as its terms say). The house\'s readings: SolaX Cloud.' })));
}

/* ---- PC (spec §10.3, §12a, M7, D44) ---- */

const HWINFO_STATES = { ok: 'running', 'not running': 'not running', 'shm off': 'running, its Shared Memory Support off',
                        stale: 'not updating', unknown: '—' };
/* Fixed physical e-paper frame time (~1 s): faster intervals don't show intermediate values. */
const PANEL_FRAME_MS = 1000;
const msLabel = (ms) => (ms < 1000 ? `${ms} ms` : `${+(ms / 1000).toFixed(2)} s`);

async function pcPage() {
  const [s, doc, pc] = await Promise.all([api('GET', '/api/settings'), api('GET', '/api/presets'), api('GET', '/api/pc')]);
  const set = s.pc || {}, keys = set.keys || {};
  const enabled = h('input', { type: 'checkbox', checked: !!set.enabled });
  const host = h('input', { type: 'text', value: set.host || '', placeholder: '192.168.1.20 or desktop-pc.local',
                            autocapitalize: 'off', spellcheck: 'false' });
  const conn = { port: set.port ?? 47123, interval: set.interval_ms ?? 1000 };
  const port = numberInput(conn, 'port', 1, 65535, 1);
  const interval = numberInput(conn, 'interval', 250, 10000, 50);
  const warning = h('p', { class: 'bad small' });
  const warn = () => {
    const ms = Number(conn.interval);
    warning.hidden = !(ms < PANEL_FRAME_MS);
    warning.textContent = "Shorter than the panel's frame time (1 s): the values in between don't show.";
  };
  interval.addEventListener('input', warn);
  warn();
  const token = secretInput('Token', !!keys.token, 'From the PC app\'s page: ABCDE-FGHIJ-…, with or without the dashes.');
  const always = h('input', { type: 'checkbox', checked: set.auto_always !== false });
  const preset = choose([['', 'None'], ...doc.presets.map((p) => [p.id, p.name])], set.preset ?? 'pc');
  const note = h('p');
  const save = card(null, note, actions(button('Save', () => busy(save, note, async () => {
    const out = { pc: { enabled: enabled.checked, host: host.value.trim(), port: Number(conn.port),
                        interval_ms: Number(conn.interval), auto_always: always.checked, preset: preset.value } };
    const t = token.value();
    if (t !== undefined) out.pc.token = t;
    await api('PATCH', '/api/settings', out);
    note.className = 'good';
    note.textContent = 'Saved.';
    toast('Saved');
  }), 'primary')));

  const nowBox = h('div');
  const showNow = (x) => {
    const age = x.age_ms ? msLabel(x.age_ms) + ' ago' : '—';
    nowBox.replaceChildren(facts([
      ['Session', x.demo ? 'the sample PC (pc demo)' : x.session ? (x.connected ? 'running' : 'running, reconnecting')
        : x.since ? `none since ${when(x.since)}` : 'none yet'],
      x.name ? ['PC', x.name] : null,
      x.session ? ['HWiNFO', HWINFO_STATES[x.hwinfo] || x.hwinfo] : null,
      x.session ? ['Last message', age] : null,
      x.session && x.set_always ? ['Sync mode', 'Always on, until the session ends'] : null,
      x.trouble ? ['Last trouble', x.trouble] : null,
      x.step && x.step.at ? ['Last sync\'s PC step', `${x.step.result}${x.step.detail ? `: ${x.step.detail}` : ''}`] : null,
      x.test && x.test.at ? ['Last test', `${when(x.test.at)}: ${x.test.result === 'ok' ? 'found' : 'not found'}` +
        `${x.test.detail ? ` (${x.test.detail})` : ''}`] : null,
    ]), ...(x.catalog && x.catalog.length ? [h('h3', { text: `Readings (${x.catalog.length})` }),
      h('ul', { class: 'plain small' }, x.catalog.map((r) => h('li', {}, h('b', { text: r.label || r.id }),
        ` pc.${r.id}${r.unit ? ` · ${r.unit}` : ''}`)))]
      : [h('p', { class: 'muted small', text: 'No readings yet: they come with the first session, and stay for the ' +
        'Presets page while the PC is off.' })]));
  };
  showNow(pc);
  const testNote = h('p');
  const nowCard = card('Now', nowBox, testNote, actions(button('Test', () => busy(nowCard, testNote, async () => {
    const before = (pc.test || {}).at || 0;
    await api('POST', '/api/pc/test');
    testNote.className = 'muted';
    testNote.textContent = 'Asking the PC…';
    for (let i = 0; i < 20; i++) { /* joining Wi-Fi first can take some seconds */
      await sleep(1000);
      const now = await api('GET', '/api/pc');
      if ((now.test || {}).at && now.test.at !== before) {
        showNow(now);
        const ok = now.test.result === 'ok';
        testNote.className = ok ? 'good' : 'bad';
        testNote.textContent = ok ? `Found ${now.name || 'the PC'}.` : sentence(`Not found: ${now.test.detail || 'no answer'}`);
        return;
      }
    }
    testNote.textContent = 'No answer yet.';
  }))));

  main.replaceChildren(h('h1', { text: 'PC' }),
    card('PC stats', h('label', { class: 'check' }, enabled, 'On'),
      h('p', { class: 'muted small', text: 'The PC runs reflbo-stats, which reads HWiNFO. A sync looks for it; while it ' +
        'answers, its readings come live.' }),
      field('Address', host), field('Port', port), token.el,
      field('A reading every (ms)', interval, '250 ms to 10 s.'), warning),
    card('When the PC is on', h('label', { class: 'check' }, always, 'Switch to Always on'),
      h('p', { class: 'muted small', text: 'Without it, a session starts only in sync mode Always on.' }),
      field('Show preset', preset, 'When the PC goes quiet, what you haven\'t changed comes back.')),
    save, nowCard);
}

/* ---- LoRa & MeshCore (spec §10.3) ---- */

const LORA_SF = [
  ['5', 'SF5'], ['6', 'SF6'], ['7', 'SF7'], ['8', 'SF8'],
  ['9', 'SF9'], ['10', 'SF10'], ['11', 'SF11'], ['12', 'SF12'],
];
const LORA_BW = [
  ['7.8', '7.8 kHz'], ['10.4', '10.4 kHz'], ['15.6', '15.6 kHz'], ['20.8', '20.8 kHz'],
  ['31.25', '31.25 kHz'], ['41.7', '41.7 kHz'], ['62.5', '62.5 kHz'],
  ['125', '125 kHz'], ['250', '250 kHz'], ['500', '500 kHz'],
];
const LORA_CR = [
  ['5', '4/5'], ['6', '4/6'], ['7', '4/7'], ['8', '4/8'],
];
const LORA_PATH_HASH = [
  ['0', '1 byte'], ['1', '2 bytes'], ['2', '3 bytes'],
];

function nodeTypeName(t) {
  switch (Number(t)) {
    case 1: return 'Chat';
    case 2: return 'Repeater';
    case 3: return 'Room';
    case 4: return 'Sensor';
    default: return 'Unknown';
  }
}

let loraActiveTarget = 'all';
let loraDmTargets = [];
let loraSeenMaxId = 0;
let loraUnreadByKey = {};
let loraSoundEnabled = false;
let loraForceFlood = false;
let loraPollTimer = null;
let loraActiveTab = 'all';

function loraAirtimeMs(bwHz, sf, cr, preambleLen, payloadBytes) {
  if (!bwHz || sf < 5 || sf > 12) return 0;
  const symbolLengthUs = Math.floor((1000000 * Math.pow(2, sf)) / bwHz);
  const sfCoeff1X4 = (sf === 5 || sf === 6) ? 25 : 17;
  const sfCoeff2 = (sf === 5 || sf === 6) ? 0 : 8;
  const ldro = symbolLengthUs >= 16384;
  const sfDivisor = ldro ? (4 * (sf - 2)) : (4 * sf);
  let bitCount = 8 * payloadBytes + 16 - 4 * sf + sfCoeff2 + 20;
  if (bitCount < 0) bitCount = 0;
  const nPrecoded = Math.floor((bitCount + sfDivisor - 1) / sfDivisor);
  const crMult = cr <= 4 ? (cr + 4) : cr;
  const nSymbolX4 = (preambleLen + 8) * 4 + sfCoeff1X4 + nPrecoded * crMult * 4;
  const toaUs = Math.floor((symbolLengthUs * nSymbolX4) / 4);
  return Math.floor((toaUs + 999) / 1000);
}

function loraPlayBeep() {
  try {
    const AC = typeof globalThis !== 'undefined' && (globalThis.AudioContext || globalThis.webkitAudioContext);
    if (!AC) return;
    const ctx = new AC();
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = 'sine';
    osc.frequency.setValueAtTime(880, ctx.currentTime);
    osc.frequency.exponentialRampToValueAtTime(1320, ctx.currentTime + 0.08);
    gain.gain.setValueAtTime(0.06, ctx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + 0.16);
    osc.connect(gain);
    gain.connect(ctx.destination);
    osc.onended = () => {
      try { if (typeof ctx.close === 'function') ctx.close(); } catch (_) {}
    };
    osc.start();
    osc.stop(ctx.currentTime + 0.17);
  } catch (_) {}
}

function loraCopyText(text, okMsg) {
  if (typeof navigator !== 'undefined' && navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
    try {
      navigator.clipboard.writeText(text);
      toast(okMsg || 'Copied');
      return;
    } catch (_) {}
  }
  toast('Clipboard unavailable — copy manually');
}

function loraBuildUri(pubKey, name, type, loc) {
  if (!pubKey) return '';
  let uri = `meshcore://${pubKey}?name=${encodeURIComponent(name || 'Node')}&type=${type || 1}`;
  if (loc && Number.isFinite(loc.lat) && Number.isFinite(loc.lon) && (loc.lat !== 0 || loc.lon !== 0)) {
    uri += `&lat=${loc.lat}&lon=${loc.lon}`;
  }
  return uri;
}

function loraParseUri(uri) {
  const s = (uri || '').trim();
  const m = s.match(/^meshcore:\/\/([0-9a-fA-F]{64})(?:\?(.*))?$/);
  if (!m) return null;
  const pub_key = m[1].toLowerCase();
  const out = { pub_key, name: '', node_type: 1, lat: null, lon: null };
  if (m[2]) {
    const params = new URLSearchParams(m[2]);
    if (params.has('name')) out.name = params.get('name') || '';
    if (params.has('type')) {
      const t = Number(params.get('type'));
      if (t >= 1 && t <= 4) out.node_type = t;
    }
    if (params.has('lat') && params.has('lon')) {
      const la = Number(params.get('lat'));
      const lo = Number(params.get('lon'));
      if (Number.isFinite(la) && Number.isFinite(lo)) {
        out.lat = la;
        out.lon = lo;
      }
    }
  }
  return out;
}

function loraHaversineKm(lat1, lon1, lat2, lon2) {
  const R = 6371;
  const toRad = (d) => (d * Math.PI) / 180;
  const dLat = toRad(lat2 - lat1);
  const dLon = toRad(lon2 - lon1);
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function loraBearingDeg(lat1, lon1, lat2, lon2) {
  const toRad = (d) => (d * Math.PI) / 180;
  const y = Math.sin(toRad(lon2 - lon1)) * Math.cos(toRad(lat2));
  const x = Math.cos(toRad(lat1)) * Math.sin(toRad(lat2)) -
    Math.sin(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.cos(toRad(lon2 - lon1));
  return ((Math.atan2(y, x) * 180) / Math.PI + 360) % 360;
}

function loraCompass(deg) {
  const dirs = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
  return dirs[Math.round(deg / 45) % 8];
}

function loraFormatTime(ts) {
  if (!ts || ts <= 0) return '';
  const d = new Date(ts * 1000);
  return [d.getHours(), d.getMinutes(), d.getSeconds()].map((n) => String(n).padStart(2, '0')).join(':');
}

function loraMsgKey(m) {
  if (m.direct && m.peer_pub) return 'dm:' + String(m.peer_pub).toLowerCase();
  return 'ch:' + (m.channel || 0);
}

async function loraPage() {
  if (loraPollTimer !== null) {
    clearTimeout(loraPollTimer);
    loraPollTimer = null;
  }
  const [s, initialSt] = await Promise.all([api('GET', '/api/settings'), api('GET', '/api/status')]);
  let st = initialSt;
  const lora = s.lora || {};

  /* Seed seen message ID so initial load does not trigger unread/sound */
  const initMsgs = (st && st.lora && st.lora.messages) || [];
  for (const m of initMsgs) {
    if (m.id && m.id > loraSeenMaxId) loraSeenMaxId = m.id;
  }

  /* Card 1: Now (Live status, telemetry & quick controls) */
  const summary = h('div', { id: 'lora-live-status' });
  summary.id = 'lora-live-status';
  summary.setAttribute('id', 'lora-live-status');
  const nowNote = h('p');
  const showNow = (status) => {
    const l = (status && status.lora) || {};
    const ageText = !l.last_packet_at
      ? 'none yet'
      : (l.age_s !== undefined && l.age_s !== null && l.age_s >= 0)
        ? `${l.age_s < 60 ? Math.round(l.age_s) + ' s' : duration(l.age_s)} ago (${when(l.last_packet_at)})`
        : when(l.last_packet_at);
    const hasSignal = l.last_rssi_dbm !== undefined && l.last_snr_db !== undefined && (l.last_packet_at || l.packet_count);
    const rssiSnr = hasSignal
      ? `${l.last_rssi_dbm} dBm / ${l.last_snr_db} dB`
      : '—';
    const txAirS = ((l.tx_air_ms || 0) / 1000).toFixed(1);
    const rxAirS = ((l.rx_air_ms || 0) / 1000).toFixed(1);

    const pubKeyKid = l.pub_key ? h('span', {},
      `${l.pub_key.slice(0, 16)}… `,
      button('Copy key', () => loraCopyText(l.pub_key, 'Key copied'), 'small')
    ) : '—';

    const bleState = !l.enabled
      ? 'Off'
      : l.ble_connected
        ? 'Connected'
        : 'Advertising';
    const blePinStr = l.ble_pin ? String(l.ble_pin).padStart(6, '0') : '—';
    const bleBadge = h('span', { class: 'ble-status' },
      h('span', { class: l.ble_connected ? 'good' : 'muted' }, bleState),
      ` (MeshCore-${l.node_name || 'NM-EPD-420'}) · PIN: `,
      h('code', { class: 'badge' }, blePinStr)
    );

    summary.replaceChildren(facts([
      ['State', loraState(l)],
      l.legacy_identity ? ['Identity', h('span', { class: 'bad' }, 'Legacy MAC-derived identity (reset recommended)')] : null,
      ['Companion BLE', bleBadge],
      ['Node', l.node_name || '—'],
      ['Public key', pubKeyKid],
      ['Noise floor', (l.noise_floor_dbm === undefined || l.noise_floor_dbm === null || l.noise_floor_dbm === 0) ? '—' : `${l.noise_floor_dbm} dBm`],
      ['RSSI / SNR', rssiSnr],
      ['Last packet', ageText],
      ['Packets', `${l.packet_count ?? 0} (flood ${l.recv_flood ?? 0}, direct ${l.recv_direct ?? 0}, err ${l.recv_errors ?? 0})`],
      ['Packets TX', `${l.packets_sent ?? 0} (flood ${l.sent_flood ?? 0}, direct ${l.sent_direct ?? 0})`],
      ['Airtime', `TX ${txAirS} s · RX ${rxAirS} s`],
    ]));
  };
  showNow(st);

  const toggleBtn = button(
    st.lora?.enabled ? 'Turn LoRa Off' : 'Turn LoRa On',
    () => busy(nowCard, nowNote, async () => {
      const nextEnabled = !st.lora?.enabled;
      await api('PATCH', '/api/settings', { schema: 1, lora: { enabled: nextEnabled } });
      if (!st.lora) st.lora = {};
      st.lora.enabled = nextEnabled;
      toggleBtn.replaceChildren(nextEnabled ? 'Turn LoRa Off' : 'Turn LoRa On');
      toast(nextEnabled ? 'LoRa enabled' : 'LoRa disabled');
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showNow(fresh);
      showChat(fresh);
      showNodes(fresh);
    })
  );
  toggleBtn.id = 'lora-toggle-btn';
  toggleBtn.setAttribute('id', 'lora-toggle-btn');

  const copyUriBtn = button('Copy meshcore:// URI', () => {
    const l = (st && st.lora) || {};
    if (!l.pub_key) {
      toast('Public key not available yet');
      return;
    }
    const uri = loraBuildUri(l.pub_key, s.lora?.node_name || l.node_name || 'Node', 1, (st && st.location) || s.location);
    loraCopyText(uri, 'meshcore:// URI copied');
  });
  copyUriBtn.id = 'lora-btn-copy-uri';
  copyUriBtn.setAttribute('id', 'lora-btn-copy-uri');

  const rekeyBtn = button('Reset identity', () => {
    if (!confirm('Generate a new random Ed25519 identity and default BLE PIN? Existing MeshCore contacts will need to re-discover this node.')) return;
    busy(nowCard, nowNote, async () => {
      await api('POST', '/api/lora/rekey', {});
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showNow(fresh);
      toast('Identity reset');
    });
  }, 'danger');
  rekeyBtn.id = 'btn-lora-rekey';
  rekeyBtn.setAttribute('id', 'btn-lora-rekey');

  const nowCard = card('Now', summary, nowNote, actions(
    toggleBtn,
    button('Refresh', () => busy(nowCard, nowNote, async () => {
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showNow(fresh);
      showChat(fresh);
      showNodes(fresh);
    })),
    button('Send Advert (Flood)', () => busy(nowCard, nowNote, async () => {
      await api('POST', '/api/lora/advert', { flood: true });
      toast('Advert sent (Flood)');
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showNow(fresh);
    })),
    button('Send Advert (Zero-Hop)', () => busy(nowCard, nowNote, async () => {
      await api('POST', '/api/lora/advert', { flood: false });
      toast('Advert sent (Zero-Hop)');
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showNow(fresh);
    })),
    button('Discover Repeaters', () => busy(nowCard, nowNote, async () => {
      await api('POST', '/api/lora/discover', {});
      toast('Discovery probe sent');
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showNow(fresh);
      showNodes(fresh);
      setTimeout(async () => {
        if (location.hash !== '#lora') return;
        try {
          const f2 = await api('GET', '/api/status');
          st = f2;
          showNow(f2);
          showNodes(f2);
        } catch (_) {}
      }, 1500);
    })),
    button('Reset counter', () => busy(nowCard, nowNote, async () => {
      await api('POST', '/api/lora/reset', {});
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showNow(fresh);
      toast('Counter reset');
    })),
    copyUriBtn,
    rekeyBtn,
  ));

  /* Card 2: Group channel chat & MeshCore messenger */
  const chInfo = h('p', { class: 'muted small' });
  const convTabs = h('div', { id: 'lora-conv-tabs', class: 'conv-tabs' });
  convTabs.id = 'lora-conv-tabs';
  convTabs.setAttribute('id', 'lora-conv-tabs');

  const forceFloodCb = h('input', { type: 'checkbox', id: 'lora-force-flood' });
  forceFloodCb.id = 'lora-force-flood';
  forceFloodCb.setAttribute('id', 'lora-force-flood');
  forceFloodCb.checked = loraForceFlood;
  forceFloodCb.addEventListener('change', () => {
    loraForceFlood = forceFloodCb.checked;
  });
  const forceFloodLabel = h('label', { class: 'check' }, forceFloodCb, 'Force Flood');

  const soundCb = h('input', { type: 'checkbox', id: 'lora-sound-toggle' });
  soundCb.id = 'lora-sound-toggle';
  soundCb.setAttribute('id', 'lora-sound-toggle');
  soundCb.checked = loraSoundEnabled;
  soundCb.addEventListener('change', () => {
    loraSoundEnabled = soundCb.checked;
  });
  const soundLabel = h('label', { class: 'check' }, soundCb, 'Sound');

  const filterMessages = (msgs) => {
    if (loraActiveTarget === 'all') return msgs;
    if (loraActiveTarget.startsWith('ch:')) {
      const chIdx = Number(loraActiveTarget.slice(3)) || 0;
      return msgs.filter((m) => !m.direct && (m.channel || 0) === chIdx);
    }
    if (loraActiveTarget.startsWith('dm:')) {
      const key = loraActiveTarget.slice(3).toLowerCase();
      return msgs.filter((m) => m.direct && (m.peer_pub || '').toLowerCase() === key);
    }
    return msgs;
  };

  const loraChannelName = (channels, chIdx, primaryName) => {
    const idx = Number(chIdx);
    const slot = (channels || []).find((c) => c && c.index === idx);
    if (slot && slot.name) return slot.name;
    if (idx === 0) return primaryName || 'Public';
    return `#${idx}`;
  };

  const exportBtn = button('Export .txt', () => {
    const l = (st && st.lora) || {};
    const channels = l.channels || [];
    const chName = l.channel_name || 'Public';
    const msgs = filterMessages(l.messages || []);
    const lines = msgs.map((m) => {
      const ts = loraFormatTime(m.ts) || (m.age_s !== undefined ? `${Math.round(m.age_s)}s ago` : '');
      const dir = m.outgoing ? 'TX' : 'RX';
      let scope = '';
      if (m.direct) {
        scope = '[DM] ';
      } else if (m.channel !== undefined) {
        const chLabel = loraChannelName(channels, m.channel, chName);
        scope = `[${chLabel}] `;
      }
      const who = m.sender ? `${m.sender}: ` : '';
      return `[${ts}] [${dir}] ${scope}${who}${m.body || m.text || ''}`;
    });
    download(new Blob([lines.join('\n')], { type: 'text/plain;charset=utf-8' }), 'meshcore-chat.txt');
  }, 'small');

  const clearHistBtn = button('Clear history', () => {
    if (!confirm('Clear all LoRa chat history?')) return;
    busy(chatCard, chatNote, async () => {
      await api('POST', '/api/lora/messages/clear', {});
      loraUnreadByKey = {};
      document.title = 'reflbo';
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showChat(fresh);
      toast('History cleared');
    });
  }, 'small danger');

  const chatToolbar = h('div', { class: 'chat-toolbar' }, forceFloodLabel, soundLabel, exportBtn, clearHistBtn);
  const chatBox = h('div', { id: 'lora-chat-box', class: 'chat-box' });
  chatBox.id = 'lora-chat-box';
  chatBox.setAttribute('id', 'lora-chat-box');
  const chatNote = h('p');
  const chatInput = h('input', { type: 'text', id: 'lora-chat-input', placeholder: 'Message on channel…' });
  chatInput.setAttribute('id', 'lora-chat-input');

  const byteCounterEl = h('span', { text: '0 / 120 B' });
  const airtimeEl = h('span', { text: 'Est. airtime: ~— ms' });
  const composerMeta = h('div', { class: 'composer-meta muted small' }, byteCounterEl, airtimeEl);

  const updateComposerMeta = () => {
    const val = chatInput.value || '';
    const bLen = bytes(val);
    byteCounterEl.textContent = `${bLen} / 120 B`;
    byteCounterEl.className = bLen > 120 ? 'over' : '';
    const l = (st && st.lora) || {};
    const curSf = Number(sf?.value || l.sf || 7);
    const curBwKhz = Number(bw?.value || l.bw_khz || 250);
    const curCr = Number(cr?.value || l.cr || 5);
    const curBwHz = Math.round(curBwKhz * 1000);
    const preamble = curSf <= 8 ? 32 : 16;
    const isDm = loraActiveTarget.startsWith('dm:');
    const nodePrefixBytes = isDm ? 0 : bytes((nodeName?.value || l.node_name || 'NM-EPD-420').trim()) + 2;
    const plainLen = 5 + nodePrefixBytes + bLen;
    const encBlocks = Math.max(1, Math.ceil(plainLen / 16)) * 16;
    const wireBytes = 2 + (isDm ? 2 : 1) + 2 + encBlocks;
    const ms = loraAirtimeMs(curBwHz, curSf, curCr, preamble, wireBytes);
    airtimeEl.textContent = `Est. airtime: ~${ms} ms @ SF${curSf}/BW${curBwKhz}`;
  };
  chatInput.addEventListener('input', updateComposerMeta);

  const updateTitleUnread = () => {
    const total = Object.values(loraUnreadByKey).reduce((a, b) => a + b, 0);
    document.title = total > 0 ? `(${total}) reflbo` : 'reflbo';
  };

  const openDmTab = (pubKey, name, focusInput) => {
    if (!pubKey) return;
    const key = pubKey.toLowerCase();
    if (!loraDmTargets.some((t) => t.pub_key.toLowerCase() === key)) {
      loraDmTargets.push({ pub_key: key, name: name || key.slice(0, 8) });
    }
    loraActiveTarget = 'dm:' + key;
    delete loraUnreadByKey[loraActiveTarget];
    updateTitleUnread();
    showChat(st);
    if (focusInput) chatInput.focus();
  };

  const selectTarget = (target) => {
    loraActiveTarget = target;
    delete loraUnreadByKey[target];
    updateTitleUnread();
    showChat(st);
  };

  const chatSend = button('Send', () => busy(chatCard, chatNote, async () => {
    const txt = (chatInput.value || '').trim();
    if (!txt) return;
    let payload;
    if (loraActiveTarget.startsWith('dm:')) {
      payload = { text: txt, to: loraActiveTarget.slice(3), force_flood: !!loraForceFlood };
    } else if (loraActiveTarget.startsWith('ch:')) {
      const chIdx = Number(loraActiveTarget.slice(3)) || 0;
      payload = chIdx === 0 ? { text: txt } : { text: txt, channel: chIdx };
    } else {
      payload = { text: txt };
    }
    await api('POST', '/api/lora/send', payload);
    chatInput.value = '';
    updateComposerMeta();
    toast('Message sent');
    const fresh = await api('GET', '/api/status');
    st = fresh;
    showChat(fresh);
    showNow(fresh);
  }), 'primary');
  chatSend.id = 'lora-chat-send';
  chatSend.setAttribute('id', 'lora-chat-send');

  chatInput.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter') chatSend.click();
  });

  const showChat = (status) => {
    const l = (status && status.lora) || {};
    const chName = l.channel_name || 'Public';
    const chHash = l.channel_hash !== undefined ? '0x' + l.channel_hash.toString(16).padStart(2, '0').toUpperCase() : '0x00';
    const channels = l.channels || [];
    const contacts = l.contacts || [];
    const adverts = l.adverts || [];

    forceFloodLabel.hidden = !loraActiveTarget.startsWith('dm:');
    if (loraActiveTarget.startsWith('dm:')) {
      const dmKey = loraActiveTarget.slice(3).toLowerCase();
      const dm = loraDmTargets.find((t) => t.pub_key.toLowerCase() === dmKey);
      const dmContact = contacts.find((c) => (c.pub_key || '').toLowerCase() === dmKey);
      let dmRoute = 'FLOOD';
      if (!loraForceFlood && dmContact && dmContact.out_path_len !== undefined && dmContact.out_path_len !== null &&
          dmContact.out_path_len !== -1 && dmContact.out_path_len !== 255) {
        const pl = dmContact.out_path_len & 0xff;
        const hops = pl & 0x3f;
        const hashSz = (pl >> 6) + 1;
        if (hashSz <= 3) dmRoute = `DIRECT (${hops} hops${hashSz > 1 ? `, ${hashSz}B` : ''})`;
      }
      const peerLabel = dm ? dm.name : (dmContact ? dmContact.name : dmKey.slice(0, 8));
      chInfo.textContent = `Direct Message to @${peerLabel} · E2E X25519 · Route: ${dmRoute}`;
      chatInput.placeholder = `Direct message to @${peerLabel}…`;
    } else if (loraActiveTarget.startsWith('ch:')) {
      const chIdx = Number(loraActiveTarget.slice(3)) || 0;
      const slot = channels.find((c) => c && c.index === chIdx);
      const slotName = (slot && slot.name) || (chIdx === 0 ? chName : `Ch${chIdx}`);
      const slotHash = (slot && slot.channel_hash !== undefined && slot.channel_hash !== null)
        ? '0x' + Number(slot.channel_hash).toString(16).padStart(2, '0').toUpperCase()
        : (chIdx === 0 ? chHash : '—');
      chInfo.textContent = `Channel #${chIdx}: ${slotName} (${slotHash}) · AES-128`;
      chatInput.placeholder = `Message on ${slotName}…`;
    } else {
      chInfo.textContent = `All conversations · Primary #0: ${chName} (${chHash})`;
      chatInput.placeholder = 'Message on channel…';
    }
    updateComposerMeta();

    /* Render conversation tabs */
    const tabEls = [];
    const makeTab = (key, label, closable) => {
      const unread = loraUnreadByKey[key] || 0;
      const cls = 'conv-tab' + (loraActiveTarget === key ? ' active' : '');
      const tab = h('button', {
        type: 'button',
        class: cls,
        onclick: () => selectTarget(key),
      }, label, unread > 0 ? h('span', { class: 'badge unread', text: String(unread) }) : null);
      if (closable) {
        const closeBtn = h('span', {
          class: 'conv-tab-close',
          text: '×',
          onclick: (ev) => {
            ev.stopPropagation();
            loraDmTargets = loraDmTargets.filter((t) => 'dm:' + t.pub_key.toLowerCase() !== key);
            delete loraUnreadByKey[key];
            updateTitleUnread();
            if (loraActiveTarget === key) loraActiveTarget = 'all';
            showChat(st);
          },
        });
        tab.append(' ', closeBtn);
      }
      return tab;
    };

    const allUnread = Object.values(loraUnreadByKey).reduce((a, b) => a + b, 0);
    const allTab = makeTab('all', 'All', false);
    if (allUnread > 0 && loraActiveTarget !== 'all') {
      allTab.append(' ', h('span', { class: 'badge unread', text: String(allUnread) }));
    }
    tabEls.push(allTab);
    for (const ch of channels) {
      if (ch && (ch.enabled || ch.index === 0)) {
        tabEls.push(makeTab(`ch:${ch.index}`, `#${ch.index} ${ch.name || (ch.index === 0 ? 'Public' : 'Ch' + ch.index)}`, false));
      }
    }
    for (const dm of loraDmTargets) {
      tabEls.push(makeTab(`dm:${dm.pub_key.toLowerCase()}`, `@${dm.name}`, true));
    }
    convTabs.replaceChildren(...tabEls);

    const msgs = filterMessages(l.messages || []);
    const nearBottom = ((chatBox.scrollHeight || 0) - (chatBox.clientHeight || 0) - (chatBox.scrollTop || 0)) <= 40;
    if (msgs.length === 0) {
      chatBox.replaceChildren(h('p', { class: 'muted' }, 'No channel messages yet'));
    } else {
      chatBox.replaceChildren(...msgs.map((m) => {
        const dirCls = m.outgoing ? 'out' : 'in';
        const dmCls = m.direct ? ' dm' : '';
        const sig = (m.rssi_dbm !== undefined && m.snr_db !== undefined)
          ? `${m.rssi_dbm} dBm / ${typeof m.snr_db === 'number' ? m.snr_db.toFixed(1) : m.snr_db} dB` : '';
        const hops = m.hops !== undefined ? `${m.hops} hops` : '';
        const age = (m.age_s !== undefined && m.age_s !== null) ? `${Math.round(m.age_s)}s ago` : '';
        const timeStr = loraFormatTime(m.ts);
        const metaStr = [m.outgoing ? 'TX' : 'RX', sig, hops, age].filter(Boolean).join(' · ');

        let senderPub = m.peer_pub || '';
        if (!senderPub && m.sender) {
          const cMatch = contacts.find((c) => c.name === m.sender) || adverts.find((a) => a.name === m.sender);
          if (cMatch && cMatch.pub_key) senderPub = cMatch.pub_key;
        }
        const senderEl = (!m.outgoing && m.sender && senderPub)
          ? h('button', {
            type: 'button',
            class: 'chat-sender-btn',
            text: m.sender,
            onclick: () => openDmTab(senderPub, m.sender, true),
          })
          : (m.sender ? h('strong', { text: m.sender }) : null);

        let scopeBadge = null;
        if (m.direct) {
          scopeBadge = h('span', { class: 'badge type', text: 'DM' });
        } else if (m.channel !== undefined) {
          const label = loraChannelName(channels, m.channel, chName);
          scopeBadge = h('span', { class: 'badge type channel', text: label });
        }

        let ackEl = null;
        if (m.direct && m.outgoing) {
          if (m.ack === 1) {
            ackEl = h('div', { class: 'chat-ack pending', text: '⏳ Waiting ACK…' });
          } else if (m.ack === 2) {
            ackEl = h('div', { class: 'chat-ack delivered', text: `✓✓ Delivered (${m.ack_rtt_ms || 0} ms)` });
          } else if (m.ack === 3) {
            ackEl = h('div', { class: 'chat-ack timeout' },
              '⚠ No ACK ',
              button('Retry (Flood)', () => busy(chatCard, chatNote, async () => {
                await api('POST', '/api/lora/send', { text: m.body || m.text || '', to: m.peer_pub, force_flood: true });
                toast('Retried via flood');
                const fresh = await api('GET', '/api/status');
                st = fresh;
                showChat(fresh);
              }), 'small')
            );
          }
        }

        return h('div', { class: `chat-msg ${dirCls}${dmCls}` },
          h('div', { class: 'chat-header' },
            timeStr ? h('span', { class: 'muted small', text: timeStr }) : null,
            scopeBadge,
            senderEl,
            h('span', { class: 'chat-meta', text: metaStr })
          ),
          h('div', { class: 'chat-body', text: m.body || m.text || '' }),
          ackEl
        );
      }));
    }
    if (nearBottom) chatBox.scrollTop = chatBox.scrollHeight || 0;
  };

  const chatCard = card('Group channel chat',
    chInfo,
    convTabs,
    chatToolbar,
    chatBox,
    h('div', { class: 'actions' }, chatInput, chatSend),
    composerMeta,
    chatNote
  );

  /* Card 3: Neighbors & heard nodes */
  const nbrBox = h('div');
  const contactsHeading = h('h3', { text: 'Saved Contacts (0 / 64)' });
  let contactFilterQuery = '';
  const contactFilterInput = h('input', {
    type: 'search',
    id: 'lora-contact-filter',
    class: 'contacts-filter',
    placeholder: 'Filter contacts by name, key or role…',
  });
  contactFilterInput.id = 'lora-contact-filter';
  contactFilterInput.setAttribute('id', 'lora-contact-filter');
  const contactsBox = h('div', { id: 'lora-contacts-box', class: 'contacts-list' });
  contactsBox.id = 'lora-contacts-box';
  contactsBox.setAttribute('id', 'lora-contacts-box');

  const importName = h('input', { type: 'text', placeholder: 'Contact name' });
  const importHex = h('input', { type: 'text', placeholder: '64-hex public key' });
  const importType = choose([['1', 'Chat'], ['2', 'Repeater'], ['3', 'Room'], ['4', 'Sensor']], '1');
  const importUri = h('input', { type: 'text', placeholder: 'Paste meshcore://… URI' });
  const importNote = h('p');
  importUri.addEventListener('input', () => {
    const parsed = loraParseUri(importUri.value);
    if (parsed) {
      importHex.value = parsed.pub_key;
      if (parsed.name) importName.value = parsed.name;
      importType.value = String(parsed.node_type || 1);
    }
  });
  const importPanel = h('div', { class: 'import-box', hidden: true },
    h('div', { class: 'import-grid' },
      field('meshcore:// URI', importUri),
      field('Name', importName),
      field('Public key (64 hex)', importHex),
      field('Type', importType),
    ),
    importNote,
    actions(button('Save contact', () => busy(nodesCard, importNote, async () => {
      const parsed = importUri.value.trim() ? loraParseUri(importUri.value) : null;
      const pub_key = (importHex.value || (parsed && parsed.pub_key) || '').trim().toLowerCase();
      if (!/^[0-9a-f]{64}$/.test(pub_key)) {
        throw new ApiError('Public key must be 64 hexadecimal characters.');
      }
      const body = {
        action: 'save',
        pub_key,
        name: (importName.value || (parsed && parsed.name) || 'Node').trim(),
        type: Number(importType.value) || 1,
      };
      if (parsed && parsed.lat !== null && parsed.lon !== null) {
        body.lat = parsed.lat;
        body.lon = parsed.lon;
      }
      await api('POST', '/api/lora/contacts', body);
      importUri.value = '';
      importName.value = '';
      importHex.value = '';
      importPanel.hidden = true;
      toast('Contact saved');
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showNodes(fresh);
    }), 'primary'))
  );
  const importToggleBtn = button('+ Add / Import Contact', () => {
    importPanel.hidden = !importPanel.hidden;
  }, 'small');

  const advBox = h('div', { id: 'lora-adverts-box' });
  advBox.id = 'lora-adverts-box';
  advBox.setAttribute('id', 'lora-adverts-box');

  const radarCanvas = h('canvas', { id: 'lora-node-map', width: '560', height: '360' });
  radarCanvas.id = 'lora-node-map';
  radarCanvas.setAttribute('id', 'lora-node-map');

  const noGpsStrip = h('div', { class: 'map-nogps-strip', hidden: true });

  const inspectorBox = h('div', { id: 'lora-map-inspector', class: 'map-inspector' },
    h('p', { class: 'muted small', text: 'Click a node on the map or press Map on an advert to inspect details.' })
  );
  inspectorBox.id = 'lora-map-inspector';
  inspectorBox.setAttribute('id', 'lora-map-inspector');

  let mapW = 560;
  let mapH = 360;
  let mapViewMode = null;
  let mapCenterLat = null;
  let mapCenterLon = null;
  let mapRangeKm = 25;
  let mapSelectedKey = null;
  let mapHitTargets = [];
  let mapCachedData = null;
  let mapFetchTimer = null;
  let mapLastInspectorSig = null;
  let mapLastNoGpsSig = null;
  let lastMapStatus = st;
  let mapDragStart = null;
  let mapDidDrag = false;

  const hasGps = (obj) => obj && Number.isFinite(Number(obj.lat)) && Number.isFinite(Number(obj.lon)) && (Number(obj.lat) !== 0 || Number(obj.lon) !== 0);

  const getAllMapNodes = (status) => {
    const l = (status && status.lora) || {};
    const loc = (status && status.location) || s.location;
    const map = new Map();
    const ensure = (key) => {
      if (!map.has(key)) {
        map.set(key, {
          key,
          pub_key: '',
          pub_prefix: '',
          name: '',
          node_type: 1,
          isStation: false,
          isContact: false,
          isAdvert: false,
          isNeighbor: false,
          lat: null,
          lon: null,
          has_loc: false,
          rssi_dbm: null,
          snr_db: null,
          snr_in_db: null,
          snr_out_db: null,
          hops: null,
          out_path_len: null,
          age_s: null,
          rx_ts: null,
          adv_ts: null,
        });
      }
      return map.get(key);
    };

    const stKey = (l.pub_key || 'station').toLowerCase();
    const stNode = ensure(stKey);
    stNode.isStation = true;
    stNode.pub_key = l.pub_key || '';
    stNode.pub_prefix = stNode.pub_key ? stNode.pub_key.slice(0, 8) : '';
    stNode.name = l.node_name || 'Local Station';
    if (hasGps(loc)) {
      stNode.lat = Number(loc.lat);
      stNode.lon = Number(loc.lon);
      stNode.has_loc = true;
    }

    for (const c of (l.contacts || [])) {
      const k = (c.pub_key || c.name || '').toLowerCase();
      if (!k) continue;
      const n = ensure(k);
      n.isContact = true;
      n.contact = c;
      n.pub_key = c.pub_key || n.pub_key;
      n.pub_prefix = n.pub_key ? n.pub_key.slice(0, 8) : n.pub_prefix;
      n.name = c.name || n.name;
      n.node_type = c.type !== undefined ? c.type : (c.node_type !== undefined ? c.node_type : n.node_type);
      n.out_path_len = c.out_path_len !== undefined ? c.out_path_len : n.out_path_len;
      if (hasGps(c)) {
        n.lat = Number(c.lat);
        n.lon = Number(c.lon);
        n.has_loc = true;
      }
      if (c.age_s !== undefined && c.age_s !== null) n.age_s = c.age_s;
    }

    for (const a of (l.adverts || [])) {
      const k = (a.pub_key || a.name || '').toLowerCase();
      if (!k) continue;
      const n = ensure(k);
      n.isAdvert = true;
      n.advert = a;
      n.pub_key = a.pub_key || n.pub_key;
      n.pub_prefix = n.pub_key ? n.pub_key.slice(0, 8) : n.pub_prefix;
      n.name = a.name || n.name;
      if (a.node_type !== undefined) n.node_type = a.node_type;
      if (a.has_loc !== false && hasGps(a)) {
        n.lat = Number(a.lat);
        n.lon = Number(a.lon);
        n.has_loc = true;
      }
      if (a.rssi_dbm !== undefined) n.rssi_dbm = a.rssi_dbm;
      if (a.snr_db !== undefined) n.snr_db = a.snr_db;
      if (a.hops !== undefined) n.hops = a.hops;
      if (a.age_s !== undefined && a.age_s !== null) n.age_s = a.age_s;
      if (a.rx_ts !== undefined) n.rx_ts = a.rx_ts;
      if (a.adv_ts !== undefined) n.adv_ts = a.adv_ts;
    }

    for (const nb of (l.neighbors || [])) {
      const pfx = (nb.pub_key || '').toLowerCase();
      let target = null;
      if (pfx) {
        for (const [k, v] of map.entries()) {
          if (k.startsWith(pfx)) { target = v; break; }
        }
      }
      if (!target) target = ensure(pfx || (nb.name || 'neighbor').toLowerCase());
      target.isNeighbor = true;
      target.neighbor = nb;
      if (!target.pub_key && nb.pub_key) target.pub_key = nb.pub_key;
      if (!target.pub_prefix && nb.pub_key) target.pub_prefix = nb.pub_key.slice(0, 8);
      if (!target.name && nb.name) target.name = nb.name;
      if (nb.node_type !== undefined) target.node_type = nb.node_type;
      if (nb.rssi_dbm !== undefined) target.rssi_dbm = nb.rssi_dbm;
      if (nb.snr_in_db !== undefined) target.snr_in_db = nb.snr_in_db;
      if (nb.snr_out_db !== undefined) target.snr_out_db = nb.snr_out_db;
      if (nb.age_s !== undefined && nb.age_s !== null) target.age_s = nb.age_s;
    }

    return Array.from(map.values());
  };

  const isInsideCachedBounds = (lat, lon, rangeKm) => {
    if (!mapCachedData || !mapCachedData.bounds) return false;
    const b = mapCachedData.bounds;
    const dLat = rangeKm / 111.32;
    const cosLat = Math.max(0.15, Math.cos((lat * Math.PI) / 180));
    const dLon = rangeKm / (111.32 * cosLat);
    if (Array.isArray(b) && b.length >= 4) {
      return (lat - dLat) >= b[0] && (lat + dLat) <= b[1] &&
             (lon - dLon) >= b[2] && (lon + dLon) <= b[3];
    }
    if (b.range_km && rangeKm > b.range_km) return false;
    if (b.min_lat !== undefined && b.max_lat !== undefined && b.min_lon !== undefined && b.max_lon !== undefined) {
      return (lat - dLat) >= b.min_lat && (lat + dLat) <= b.max_lat &&
             (lon - dLon) >= b.min_lon && (lon + dLon) <= b.max_lon;
    }
    return false;
  };

  const fetchMapVectors = (lat, lon, rangeKm, immediate = false) => {
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) return;
    if (isInsideCachedBounds(lat, lon, rangeKm)) return;
    const reqRange = Math.min(300, Math.max(5, Math.round(rangeKm * 2)));
    const run = async () => {
      try {
        const res = await api('GET', `/api/lora/map?lat=${encodeURIComponent(lat.toFixed(5))}&lon=${encodeURIComponent(lon.toFixed(5))}&range_km=${reqRange}`);
        if (!res) return;
        const dLat = reqRange / 111.32;
        const cosLat = Math.max(0.15, Math.cos((lat * Math.PI) / 180));
        const dLon = reqRange / (111.32 * cosLat);
        mapCachedData = {
          bounds: res.bounds || {
            min_lat: lat - dLat,
            max_lat: lat + dLat,
            min_lon: lon - dLon,
            max_lon: lon + dLon,
            range_km: reqRange,
            lat,
            lon,
          },
          lines: res.lines || [],
          towns: res.towns || [],
          fetchedAt: Date.now(),
        };
        drawRadar(lastMapStatus);
      } catch (_) {
        /* Fallback cleanly if /api/lora/map fails */
      }
    };
    if (mapFetchTimer) clearTimeout(mapFetchTimer);
    if (immediate) {
      mapFetchTimer = null;
      run();
    } else {
      mapFetchTimer = setTimeout(() => {
        mapFetchTimer = null;
        run();
      }, 360);
    }
  };

  const formatTs = (ts) => {
    if (!ts) return '—';
    const d = new Date(ts > 1e12 ? ts : ts * 1000);
    return Number.isNaN(d.getTime()) ? String(ts) : d.toLocaleString();
  };

  const updateInspector = (status, force = false) => {
    const nodes = getAllMapNodes(status);
    const sel = mapSelectedKey ? nodes.find((n) => n.key === mapSelectedKey) : null;
    if (!sel) {
      if (!force && mapLastInspectorSig === 'none') return;
      mapLastInspectorSig = 'none';
      inspectorBox.replaceChildren(
        h('p', { class: 'muted small', text: 'Click a node on the map or press Map on an advert to inspect details.' })
      );
      return;
    }

    const sig = JSON.stringify([
      sel.key, sel.name, sel.node_type, sel.isStation, sel.isContact, sel.isAdvert, sel.isNeighbor,
      sel.lat, sel.lon, sel.rssi_dbm, sel.snr_db, sel.snr_in_db, sel.snr_out_db, sel.hops,
      sel.out_path_len, sel.age_s, sel.rx_ts, sel.adv_ts
    ]);
    if (!force && sig === mapLastInspectorSig) return;
    mapLastInspectorSig = sig;

    const loc = (status && status.location) || s.location;
    const hasStationLoc = hasGps(loc);
    const roleName = sel.isStation ? 'Station' : nodeTypeName(sel.node_type);
    const badges = [
      h('span', { class: `badge ${roleName.toLowerCase()}`, text: roleName }),
      sel.isStation ? h('span', { class: 'badge', text: 'Local Station' }) : null,
      sel.isContact ? h('span', { class: 'badge saved', text: 'Saved Contact' }) : null,
      sel.isAdvert ? h('span', { class: 'badge', text: 'Advert' }) : null,
      sel.isNeighbor ? h('span', { class: 'badge', text: 'Zero-Hop Neighbor' }) : null,
    ].filter(Boolean);

    let routeStr = 'FLOOD';
    if (sel.out_path_len !== null && sel.out_path_len !== undefined && sel.out_path_len !== -1 && sel.out_path_len !== 255) {
      const pl = sel.out_path_len & 0xff;
      const hops = pl & 0x3f;
      const hashSz = (pl >> 6) + 1;
      if (hashSz <= 3) routeStr = `DIRECT (${hops} hops${hashSz > 1 ? `, ${hashSz}B` : ''})`;
    }

    let locText = 'No GPS';
    let distText = '—';
    let osmLink = null;
    if (sel.has_loc && sel.lat !== null && sel.lon !== null) {
      locText = `${sel.lat.toFixed(5)}, ${sel.lon.toFixed(5)}`;
      if (hasStationLoc && !sel.isStation) {
        const km = loraHaversineKm(loc.lat, loc.lon, sel.lat, sel.lon);
        const brg = loraBearingDeg(loc.lat, loc.lon, sel.lat, sel.lon);
        const dist = km < 1 ? `${Math.round(km * 1000)} m` : `${km.toFixed(1)} km`;
        distText = `${dist} · ${Math.round(brg)}° ${loraCompass(brg)}`;
      }
      osmLink = h('a', {
        href: `https://www.openstreetmap.org/?mlat=${sel.lat.toFixed(5)}&mlon=${sel.lon.toFixed(5)}#map=14/${sel.lat.toFixed(5)}/${sel.lon.toFixed(5)}`,
        target: '_blank',
        rel: 'noopener',
        class: 'small',
        text: 'Open in OpenStreetMap ↗',
      });
    }

    const sigParts = [];
    if (sel.rssi_dbm !== null && sel.rssi_dbm !== undefined) sigParts.push(`RSSI ${sel.rssi_dbm} dBm`);
    if (sel.snr_db !== null && sel.snr_db !== undefined) sigParts.push(`SNR ${sel.snr_db} dB`);
    if (sel.hops !== null && sel.hops !== undefined) sigParts.push(`Hops ${sel.hops}`);
    const zhopParts = [];
    if (sel.snr_in_db !== null && sel.snr_in_db !== undefined) zhopParts.push(`In ${sel.snr_in_db} dB`);
    if (sel.snr_out_db !== null && sel.snr_out_db !== undefined) zhopParts.push(`Out ${sel.snr_out_db} dB`);

    const actionBtns = [];
    if (sel.pub_key) {
      actionBtns.push(button('Copy Key', () => loraCopyText(sel.pub_key, 'Public key copied'), 'small'));
      if (!sel.isStation) {
        actionBtns.push(button('Message', () => openDmTab(sel.pub_key, sel.name, true), 'small'));
        if (sel.isContact) {
          actionBtns.push(button('Reset path', () => busy(nodesCard, importNote, async () => {
            await api('POST', '/api/lora/contacts', { action: 'reset_path', pub_key: sel.pub_key });
            toast('Path reset');
            const fresh = await api('GET', '/api/status');
            st = fresh;
            showNodes(fresh);
          }), 'small'));
          actionBtns.push(button('Remove', () => busy(nodesCard, importNote, async () => {
            await api('POST', '/api/lora/contacts', { action: 'delete', pub_key: sel.pub_key });
            toast('Contact removed');
            const fresh = await api('GET', '/api/status');
            st = fresh;
            showNodes(fresh);
          }), 'small danger'));
        } else {
          actionBtns.push(button('+ Contact', () => busy(nodesCard, importNote, async () => {
            await api('POST', '/api/lora/contacts', {
              action: 'save',
              pub_key: sel.pub_key,
              name: sel.name || sel.pub_key.slice(0, 8),
              type: sel.node_type || 1,
              ...(sel.has_loc && sel.lat != null && sel.lon != null ? { lat: sel.lat, lon: sel.lon } : {}),
            });
            toast('Contact added');
            const fresh = await api('GET', '/api/status');
            st = fresh;
            showNodes(fresh);
          }), 'small'));
        }
        actionBtns.push(button('Copy meshcore:// URI', () => loraCopyText(loraBuildUri(sel.pub_key, sel.name, sel.node_type, sel), 'Contact URI copied'), 'small'));
      }
    }

    inspectorBox.replaceChildren(
      h('div', { class: 'row' },
        h('strong', { text: sel.name || sel.pub_prefix || 'Unknown' }),
        ' ',
        ...badges
      ),
      sel.pub_key ? h('div', { class: 'muted small' },
        `Key: `, h('code', { text: sel.pub_key }),
        sel.pub_prefix ? [` · Prefix: `, h('code', { text: sel.pub_prefix })] : null
      ) : null,
      h('div', { class: 'map-inspector-grid' },
        h('div', {}, h('span', { class: 'muted', text: 'Signal: ' }), sigParts.join(' · ') || '—'),
        h('div', {}, h('span', { class: 'muted', text: 'Route: ' }), routeStr),
        zhopParts.length ? h('div', {}, h('span', { class: 'muted', text: 'Zero-hop SNR: ' }), zhopParts.join(' / ')) : null,
        h('div', {}, h('span', { class: 'muted', text: 'Location: ' }), locText, osmLink ? [' ', osmLink] : null),
        h('div', {}, h('span', { class: 'muted', text: 'Distance / Azimuth: ' }), distText),
        h('div', {}, h('span', { class: 'muted', text: 'Age: ' }), (sel.age_s !== null && sel.age_s !== undefined) ? duration(sel.age_s) : '—'),
        sel.rx_ts ? h('div', {}, h('span', { class: 'muted', text: 'RX: ' }), formatTs(sel.rx_ts)) : null,
        sel.adv_ts ? h('div', {}, h('span', { class: 'muted', text: 'Adv: ' }), formatTs(sel.adv_ts)) : null
      ),
      actionBtns.length ? h('div', { class: 'contact-actions' }, ...actionBtns) : null
    );
  };

  const drawNodeShape = (ctx, x, y, role, color, selected) => {
    ctx.save();
    ctx.fillStyle = color;
    ctx.strokeStyle = '#ffffff';
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    if (role === 'station') {
      for (let i = 0; i < 10; i++) {
        const r = i % 2 === 0 ? 7 : 3.2;
        const a = (i * Math.PI) / 5 - Math.PI / 2;
        if (i === 0) ctx.moveTo(x + r * Math.cos(a), y + r * Math.sin(a));
        else ctx.lineTo(x + r * Math.cos(a), y + r * Math.sin(a));
      }
      ctx.closePath();
    } else if (role === 2) {
      ctx.moveTo(x, y - 6);
      ctx.lineTo(x + 5.5, y + 4.5);
      ctx.lineTo(x - 5.5, y + 4.5);
      ctx.closePath();
    } else if (role === 3) {
      ctx.rect(x - 4.5, y - 4.5, 9, 9);
    } else if (role === 4) {
      ctx.moveTo(x, y - 6);
      ctx.lineTo(x + 5.5, y);
      ctx.lineTo(x, y + 6);
      ctx.lineTo(x - 5.5, y);
      ctx.closePath();
    } else {
      ctx.arc(x, y, 4.5, 0, Math.PI * 2);
    }
    ctx.fill();
    ctx.stroke();
    if (selected) {
      ctx.strokeStyle = '#2563eb';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(x, y, 9, 0, Math.PI * 2);
      ctx.stroke();
    }
    ctx.restore();
  };

  const syncMapSize = () => {
    if (typeof radarCanvas.getBoundingClientRect === 'function') {
      const rect = radarCanvas.getBoundingClientRect();
      if (rect && rect.width > 0) {
        if (Math.round(rect.width) === 560 && Math.round(rect.height) === 360) {
          mapW = 560;
          mapH = 360;
        } else {
          mapW = Math.max(280, Math.min(900, Math.round(rect.width)));
          mapH = Math.max(260, Math.round(mapW * 0.64));
        }
      }
    }
  };

  const drawLabel = (ctx, textStr, x, y, align, occupied, font, fillStyle) => {
    if (!textStr) return false;
    ctx.font = font;
    ctx.textAlign = align;
    const w = typeof ctx.measureText === 'function' ? ctx.measureText(textStr).width : textStr.length * 6;
    const x1 = align === 'center' ? x - w / 2 : (align === 'right' ? x - w : x);
    const box = { x1: x1 - 2, y1: y - 10, x2: x1 + w + 2, y2: y + 3 };
    for (const o of occupied) {
      if (box.x1 < o.x2 && box.x2 > o.x1 && box.y1 < o.y2 && box.y2 > o.y1) return false;
    }
    occupied.push(box);
    if (typeof ctx.strokeText === 'function') {
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.85)';
      ctx.lineWidth = 3;
      ctx.strokeText(textStr, x, y);
    }
    ctx.fillStyle = fillStyle;
    ctx.fillText(textStr, x, y);
    return true;
  };

  const updateNoGpsStrip = (nodes) => {
    const noGpsNodes = nodes.filter((n) => !n.has_loc);
    const sig = noGpsNodes.map((n) => `${n.key}:${n.name || ''}:${n.key === mapSelectedKey ? 1 : 0}`).join('|');
    if (sig === mapLastNoGpsSig) return;
    mapLastNoGpsSig = sig;
    if (noGpsNodes.length === 0) {
      noGpsStrip.hidden = true;
      noGpsStrip.replaceChildren();
      return;
    }
    noGpsStrip.hidden = false;
    noGpsStrip.replaceChildren(
      h('span', { class: 'muted small', text: `No GPS (${noGpsNodes.length}):` }),
      ...noGpsNodes.map((n) => {
        const btn = h('button', {
          type: 'button',
          class: `map-nogps-chip${n.key === mapSelectedKey ? ' active' : ''}`,
          text: n.name || n.pub_prefix || n.key.slice(0, 8),
        });
        btn.addEventListener('click', () => {
          mapSelectedKey = n.key;
          drawRadar(lastMapStatus);
          updateInspector(lastMapStatus, true);
        });
        return btn;
      })
    );
  };

  const drawRadar = (status) => {
    lastMapStatus = status || lastMapStatus;
    if (!radarCanvas || typeof radarCanvas.getContext !== 'function') return;
    const ctx = radarCanvas.getContext('2d');
    if (!ctx) return;

    syncMapSize();
    const W = mapW;
    const H = mapH;
    const dpr = (typeof window !== 'undefined' && window.devicePixelRatio) ? Math.min(2, Math.max(1, window.devicePixelRatio)) : 1;
    if (radarCanvas.width !== W * dpr) radarCanvas.width = W * dpr;
    if (radarCanvas.height !== H * dpr) radarCanvas.height = H * dpr;

    ctx.save();
    if (typeof ctx.scale === 'function') ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, W, H);

    const nodes = getAllMapNodes(lastMapStatus);
    const anyGps = nodes.some((n) => n.has_loc);
    if (!mapViewMode) mapViewMode = anyGps ? 'geo' : 'radar';

    if (typeof modeBtn !== 'undefined') modeBtn.textContent = mapViewMode === 'geo' ? 'Geo Map' : 'Polar Radar';
    if (typeof rangeBadge !== 'undefined') rangeBadge.textContent = `±${Math.round(mapRangeKm)} km`;
    if (radarCanvas.style) radarCanvas.style.cursor = mapViewMode === 'geo' ? 'grab' : 'default';

    if (mapViewMode === 'geo' && anyGps) {
      if (mapCenterLat === null || mapCenterLon === null) {
        const gpsNode = nodes.find((n) => n.isStation && n.has_loc) || nodes.find((n) => n.has_loc);
        mapCenterLat = gpsNode.lat;
        mapCenterLon = gpsNode.lon;
      }
      fetchMapVectors(mapCenterLat, mapCenterLon, mapRangeKm, !mapCachedData);
    }

    const cx = W / 2;
    const cy = H / 2;
    const maxR = Math.min(cx, cy) - 24;
    mapHitTargets = [];
    const occupied = [];

    if (mapViewMode === 'geo' && mapCenterLat !== null && mapCenterLon !== null) {
      const cosLat = Math.max(0.15, Math.cos((mapCenterLat * Math.PI) / 180));
      const kmPerPx = (mapRangeKm * 2) / Math.min(W, H);
      const project = (lat, lon) => {
        const dxKm = (lon - mapCenterLon) * 111.32 * cosLat;
        const dyKm = (lat - mapCenterLat) * 111.32;
        return { x: cx + dxKm / kmPerPx, y: cy - dyKm / kmPerPx };
      };

      /* Subtle grid */
      ctx.strokeStyle = 'rgba(128,128,128,0.18)';
      ctx.lineWidth = 1;
      for (let gx = 40; gx < W; gx += 80) {
        ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, H); ctx.stroke();
      }
      for (let gy = 40; gy < H; gy += 80) {
        ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(W, gy); ctx.stroke();
      }

      /* Cached Natural Earth / PL vectors */
      if (mapCachedData && Array.isArray(mapCachedData.lines)) {
        for (const line of mapCachedData.lines) {
          if (!Array.isArray(line) || line.length < 2) continue;
          if (Array.isArray(line[0])) {
            ctx.strokeStyle = 'rgba(100,116,139,0.45)';
            ctx.lineWidth = 1;
            ctx.setLineDash([]);
            ctx.beginPath();
            line.forEach((pt, idx) => {
              const p = project(pt[0], pt[1]);
              if (idx === 0) ctx.moveTo(p.x, p.y);
              else ctx.lineTo(p.x, p.y);
            });
            ctx.stroke();
            continue;
          }
          if (line.length < 5) continue;
          const kind = line[0];
          if (kind === 0) {
            ctx.strokeStyle = 'rgba(71, 85, 105, 0.78)';
            ctx.lineWidth = 1.8;
            ctx.setLineDash([]);
          } else if (kind === 1) {
            ctx.strokeStyle = 'rgba(37, 99, 235, 0.50)';
            ctx.lineWidth = 1.4;
            ctx.setLineDash([]);
          } else {
            ctx.strokeStyle = 'rgba(100, 116, 139, 0.45)';
            ctx.lineWidth = 1;
            ctx.setLineDash([4, 3]);
          }
          ctx.beginPath();
          for (let i = 1; i + 1 < line.length; i += 2) {
            const p = project(line[i], line[i + 1]);
            if (i === 1) ctx.moveTo(p.x, p.y);
            else ctx.lineTo(p.x, p.y);
          }
          ctx.stroke();
        }
        ctx.setLineDash([]);
      }

      const gpsNodes = nodes.filter((n) => n.has_loc).sort((a, b) => {
        const pa = (a.isStation ? 2 : 0) + (a.key === mapSelectedKey ? 1 : 0);
        const pb = (b.isStation ? 2 : 0) + (b.key === mapSelectedKey ? 1 : 0);
        return pb - pa;
      });
      for (const n of gpsNodes) {
        const p = project(n.lat, n.lon);
        const x = p.x;
        const y = p.y;
        const role = n.isStation ? 'station' : n.node_type;
        const color = n.isStation ? '#2563eb' : (role === 2 ? '#16a34a' : role === 3 ? '#8b5cf6' : role === 4 ? '#06b6d4' : '#f59e0b');
        drawNodeShape(ctx, x, y, role, color, n.key === mapSelectedKey);
        if (n.name) {
          drawLabel(ctx, n.name.slice(0, 14), x, y - 9, 'center', occupied, '10px sans-serif', '#475569');
        }
        mapHitTargets.push({ key: n.key, x, y, r: 12, node: n });
      }

      if (mapCachedData && Array.isArray(mapCachedData.towns)) {
        const towns = [...mapCachedData.towns].sort((a, b) => (b.pop || 0) - (a.pop || 0));
        for (const t of towns) {
          if (t.lat === undefined || t.lon === undefined) continue;
          const p = project(t.lat, t.lon);
          if (p.x >= 0 && p.x <= W && p.y >= 0 && p.y <= H) {
            drawLabel(ctx, t.name || '', p.x + 4, p.y + 3, 'left', occupied, '10px sans-serif', '#64748b');
          }
        }
      }
      updateNoGpsStrip(nodes);
    } else {
      /* Polar Radar */
      ctx.strokeStyle = 'rgba(128,128,128,0.35)';
      ctx.lineWidth = 1;
      for (const frac of [0.33, 0.66, 1]) {
        ctx.beginPath(); ctx.arc(cx, cy, maxR * frac, 0, Math.PI * 2); ctx.stroke();
        ctx.fillStyle = '#888';
        ctx.font = '10px sans-serif';
        ctx.textAlign = 'left';
        ctx.fillText(`${Math.round(mapRangeKm * frac)} km`, cx + 4, cy - maxR * frac + 11);
      }
      ctx.beginPath();
      ctx.moveTo(cx, cy - maxR); ctx.lineTo(cx, cy + maxR);
      ctx.moveTo(cx - maxR, cy); ctx.lineTo(cx + maxR, cy);
      ctx.stroke();
      ctx.fillStyle = '#888';
      ctx.font = '11px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('N', cx, cy - maxR - 6);
      ctx.fillText('S', cx, cy + maxR + 14);
      ctx.fillText('E', cx + maxR + 10, cy + 4);
      ctx.fillText('W', cx - maxR - 10, cy + 4);

      const loc = (lastMapStatus && lastMapStatus.location) || s.location;
      const hasStationLoc = hasGps(loc);

      for (const n of nodes) {
        let x, y;
        if (n.isStation) {
          x = cx; y = cy;
        } else if (hasStationLoc && n.has_loc) {
          const km = loraHaversineKm(loc.lat, loc.lon, n.lat, n.lon);
          const brg = (loraBearingDeg(loc.lat, loc.lon, n.lat, n.lon) - 90) * (Math.PI / 180);
          const r = Math.min(maxR, Math.max(12, (km / mapRangeKm) * maxR));
          x = cx + r * Math.cos(brg);
          y = cy + r * Math.sin(brg);
        } else {
          const rssi = n.rssi_dbm !== null && n.rssi_dbm !== undefined ? n.rssi_dbm : -100;
          const norm = Math.min(1, Math.max(0.15, (-rssi - 40) / 85));
          const r = norm * maxR;
          let hash = 0;
          for (let i = 0; i < n.key.length; i++) hash = (hash * 31 + n.key.charCodeAt(i)) & 0xffff;
          const ang = (hash / 0xffff) * Math.PI * 2;
          x = cx + r * Math.cos(ang);
          y = cy + r * Math.sin(ang);
        }
        const role = n.isStation ? 'station' : n.node_type;
        const color = n.isStation ? '#2563eb' : (role === 2 ? '#16a34a' : role === 3 ? '#8b5cf6' : role === 4 ? '#06b6d4' : '#f59e0b');
        drawNodeShape(ctx, x, y, role, color, n.key === mapSelectedKey);
        if (n.name) {
          drawLabel(ctx, n.name.slice(0, 14), x, y - 9, 'center', occupied, '10px sans-serif', '#475569');
        }
        mapHitTargets.push({ key: n.key, x, y, r: 12, node: n });
      }
      updateNoGpsStrip([]);
    }
    ctx.restore();
  };

  const getCanvasPoint = (ev) => {
    if (typeof radarCanvas.getBoundingClientRect === 'function') {
      const rect = radarCanvas.getBoundingClientRect();
      const scaleX = rect.width ? (mapW / rect.width) : 1;
      const scaleY = rect.height ? (mapH / rect.height) : 1;
      return {
        x: ((ev.clientX ?? 0) - rect.left) * scaleX,
        y: ((ev.clientY ?? 0) - rect.top) * scaleY,
      };
    }
    return { x: ev.offsetX ?? ev.clientX ?? 0, y: ev.offsetY ?? ev.clientY ?? 0 };
  };

  const selectMapNodeAt = (x, y) => {
    let best = null;
    let bestDist = Infinity;
    for (const t of mapHitTargets) {
      const d = Math.hypot(t.x - x, t.y - y);
      if (d <= t.r && d < bestDist) {
        best = t;
        bestDist = d;
      }
    }
    if (best) {
      mapSelectedKey = best.key;
      drawRadar(lastMapStatus);
      updateInspector(lastMapStatus, true);
    }
  };

  radarCanvas.addEventListener('click', (ev) => {
    if (mapDidDrag) { mapDidDrag = false; return; }
    const pt = getCanvasPoint(ev);
    selectMapNodeAt(pt.x, pt.y);
  });

  radarCanvas.addEventListener('pointerdown', (ev) => {
    mapDragStart = { x: ev.clientX ?? 0, y: ev.clientY ?? 0, lat: mapCenterLat, lon: mapCenterLon };
    mapDidDrag = false;
  });
  radarCanvas.addEventListener('pointermove', (ev) => {
    if (!mapDragStart || mapViewMode !== 'geo' || mapCenterLat === null || mapCenterLon === null) return;
    let dx = (ev.clientX ?? 0) - mapDragStart.x;
    let dy = (ev.clientY ?? 0) - mapDragStart.y;
    if (typeof radarCanvas.getBoundingClientRect === 'function') {
      const rect = radarCanvas.getBoundingClientRect();
      if (rect && rect.width > 0 && rect.height > 0) {
        dx *= mapW / rect.width;
        dy *= mapH / rect.height;
      }
    }
    if (Math.hypot(dx, dy) >= 5) {
      mapDidDrag = true;
      const kmPerPx = (mapRangeKm * 2) / Math.min(mapW, mapH);
      const cosLat = Math.max(0.15, Math.cos((mapDragStart.lat * Math.PI) / 180));
      mapCenterLat = mapDragStart.lat + (dy * kmPerPx) / 111.32;
      mapCenterLon = mapDragStart.lon - (dx * kmPerPx) / (111.32 * cosLat);
      drawRadar(lastMapStatus);
      fetchMapVectors(mapCenterLat, mapCenterLon, mapRangeKm);
    }
  });
  radarCanvas.addEventListener('pointerup', () => { mapDragStart = null; });
  radarCanvas.addEventListener('pointercancel', () => { mapDragStart = null; });

  if (typeof ResizeObserver !== 'undefined') {
    const ro = new ResizeObserver(() => {
      const run = () => {
        syncMapSize();
        drawRadar(lastMapStatus);
      };
      if (typeof requestAnimationFrame === 'function') requestAnimationFrame(run);
      else run();
    });
    ro.observe(radarCanvas);
  }

  radarCanvas.addEventListener('wheel', (ev) => {
    if (typeof ev.preventDefault === 'function') ev.preventDefault();
    const factor = (ev.deltaY || 0) < 0 ? 0.8 : 1.25;
    mapRangeKm = Math.min(300, Math.max(2, mapRangeKm * factor));
    drawRadar(lastMapStatus);
    if (mapViewMode === 'geo' && mapCenterLat !== null && mapCenterLon !== null) {
      fetchMapVectors(mapCenterLat, mapCenterLon, mapRangeKm);
    }
  });

  const modeBtn = button('Geo Map', () => {
    mapViewMode = mapViewMode === 'geo' ? 'radar' : 'geo';
    drawRadar(lastMapStatus);
  }, 'small');
  const zoomInBtn = button('+', () => {
    mapRangeKm = Math.max(2, mapRangeKm * 0.75);
    drawRadar(lastMapStatus);
    if (mapViewMode === 'geo' && mapCenterLat !== null && mapCenterLon !== null) {
      fetchMapVectors(mapCenterLat, mapCenterLon, mapRangeKm);
    }
  }, 'small');
  const zoomOutBtn = button('−', () => {
    mapRangeKm = Math.min(300, mapRangeKm * 1.35);
    drawRadar(lastMapStatus);
    if (mapViewMode === 'geo' && mapCenterLat !== null && mapCenterLon !== null) {
      fetchMapVectors(mapCenterLat, mapCenterLon, mapRangeKm);
    }
  }, 'small');
  const fitBtn = button('Fit', () => {
    const gpsNodes = getAllMapNodes(lastMapStatus).filter((n) => n.has_loc);
    if (gpsNodes.length === 0) return;
    let minLat = Infinity, maxLat = -Infinity, minLon = Infinity, maxLon = -Infinity;
    for (const n of gpsNodes) {
      if (n.lat < minLat) minLat = n.lat;
      if (n.lat > maxLat) maxLat = n.lat;
      if (n.lon < minLon) minLon = n.lon;
      if (n.lon > maxLon) maxLon = n.lon;
    }
    mapCenterLat = (minLat + maxLat) / 2;
    mapCenterLon = (minLon + maxLon) / 2;
    const spanKm = loraHaversineKm(minLat, minLon, maxLat, maxLon);
    mapRangeKm = Math.min(300, Math.max(5, spanKm * 0.7));
    mapViewMode = 'geo';
    drawRadar(lastMapStatus);
    fetchMapVectors(mapCenterLat, mapCenterLon, mapRangeKm, true);
  }, 'small');
  const centerBtn = button('Center', () => {
    const loc = (lastMapStatus && lastMapStatus.location) || s.location;
    if (hasGps(loc)) {
      mapCenterLat = Number(loc.lat);
      mapCenterLon = Number(loc.lon);
      drawRadar(lastMapStatus);
      fetchMapVectors(mapCenterLat, mapCenterLon, mapRangeKm, true);
    }
  }, 'small');

  const rangeBadge = h('span', { class: 'map-range-badge muted small', text: `±${Math.round(mapRangeKm)} km` });
  const mapToolbar = h('div', { class: 'map-toolbar' }, modeBtn, zoomInBtn, zoomOutBtn, fitBtn, centerBtn, rangeBadge);
  const mapLegend = h('div', { class: 'map-legend' },
    h('span', { text: '★ Station' }),
    h('span', { text: '▲ Repeater' }),
    h('span', { text: '● Chat' }),
    h('span', { text: '■ Room' }),
    h('span', { text: '◆ Sensor' }),
    h('span', { class: 'muted', text: 'PL dense city coverage · Natural Earth' })
  );

  const showNodes = (status) => {
    const l = (status && status.lora) || {};
    const loc = (status && status.location) || s.location;
    const hasStationLoc = loc && Number.isFinite(loc.lat) && Number.isFinite(loc.lon) && (loc.lat !== 0 || loc.lon !== 0);

    const contacts = l.contacts || [];
    const advs = l.adverts || [];
    const nbrs = l.neighbors || [];
    if (nbrs.length === 0) {
      nbrBox.replaceChildren(h('p', { class: 'muted' }, 'No repeaters discovered yet'));
    } else {
      const list = h('ul', { class: 'list' });
      for (const n of nbrs) {
        const pfx = (n.pub_key || '').toLowerCase();
        const matchNode = pfx
          ? (contacts.find((c) => (c.pub_key || '').toLowerCase().startsWith(pfx)) ||
             advs.find((a) => (a.pub_key || '').toLowerCase().startsWith(pfx)))
          : null;
        const resolvedName = n.name || (matchNode && matchNode.name) || '';
        const resolvedType = n.node_type || (matchNode && (matchNode.type || matchNode.node_type)) || 2;
        const typeBadge = h('span', { class: `badge ${nodeTypeName(resolvedType).toLowerCase()}`, text: nodeTypeName(resolvedType) });
        const ageStr = (n.age_s !== undefined && n.age_s !== null) ? `${Math.round(n.age_s)}s ago` : '—';
        const snrOutStr = (n.snr_out_db !== undefined && n.snr_out_db !== null) ? `SNR Out ${n.snr_out_db} dB · ` : '';
        const meta = `${snrOutStr}In ${n.snr_in_db} dB · RSSI ${n.rssi_dbm} dBm · ${ageStr}`;
        list.append(h('li', {},
          h('div', { class: 'grow' },
            h('strong', { text: resolvedName || n.pub_key || 'Unknown' }), ' ',
            resolvedName && n.pub_key ? [h('code', { class: 'badge', text: n.pub_key.slice(0, 8) }), ' '] : null,
            typeBadge,
            h('div', { class: 'muted small', text: meta })
          )
        ));
      }
      nbrBox.replaceChildren(list);
    }

    const q = (contactFilterQuery || '').trim().toLowerCase();
    contactFilterInput.hidden = contacts.length === 0 && !q;
    const filteredContacts = !q
      ? contacts
      : contacts.filter((c) => {
          const cType = nodeTypeName(c.type !== undefined ? c.type : c.node_type).toLowerCase();
          return (c.name || '').toLowerCase().includes(q) ||
                 (c.pub_key || '').toLowerCase().includes(q) ||
                 cType.includes(q);
        });
    contactsHeading.textContent = q
      ? `Saved Contacts (${filteredContacts.length} / ${contacts.length} of 64)`
      : `Saved Contacts (${contacts.length} / 64)`;

    const prevContactsScroll = contactsBox.scrollTop || 0;
    if (contacts.length === 0) {
      contactsBox.replaceChildren(h('p', { class: 'muted' }, 'No saved contacts yet'));
    } else if (filteredContacts.length === 0) {
      contactsBox.replaceChildren(h('p', { class: 'muted' }, 'No contacts match filter'));
    } else {
      contactsBox.replaceChildren(...filteredContacts.map((c) => {
        const cType = c.type !== undefined ? c.type : c.node_type;
        const typeBadge = h('span', { class: `badge ${nodeTypeName(cType).toLowerCase()}`, text: nodeTypeName(cType) });
        let pathStr = 'FLOOD';
        if (c.out_path_len !== undefined && c.out_path_len !== null && c.out_path_len !== -1 && c.out_path_len !== 255) {
          const pl = c.out_path_len & 0xff;
          const hops = pl & 0x3f;
          const hashSz = (pl >> 6) + 1;
          if (hashSz <= 3) {
            pathStr = `DIRECT (${hops} hops${hashSz > 1 ? `, ${hashSz}B` : ''})`;
          }
        }
        let distStr = '';
        if (hasStationLoc && c.lat && c.lon && (c.lat !== 0 || c.lon !== 0)) {
          const km = loraHaversineKm(loc.lat, loc.lon, c.lat, c.lon);
          const brg = loraBearingDeg(loc.lat, loc.lon, c.lat, c.lon);
          distStr = km < 1 ? `${Math.round(km * 1000)} m ${loraCompass(brg)}` : `${km.toFixed(1)} km ${loraCompass(brg)}`;
        }
        const ageStr = (c.age_s !== undefined && c.age_s !== null) ? duration(c.age_s) : '—';
        const meta = [pathStr, distStr, ageStr].filter(Boolean).join(' · ');
        return h('div', { class: 'contact-row' },
          h('div', { class: 'contact-main' },
            h('strong', { text: c.name || (c.pub_key ? c.pub_key.slice(0, 12) : 'Unknown') }), ' ',
            typeBadge,
            h('div', { class: 'muted small', text: meta })
          ),
          h('div', { class: 'contact-actions' },
            button('Message', () => openDmTab(c.pub_key, c.name, true), 'small'),
            button('Reset path', () => busy(nodesCard, importNote, async () => {
              await api('POST', '/api/lora/contacts', { action: 'reset_path', pub_key: c.pub_key });
              toast('Path reset');
              const fresh = await api('GET', '/api/status');
              st = fresh;
              showNodes(fresh);
            }), 'small'),
            button('Copy URI', () => loraCopyText(loraBuildUri(c.pub_key, c.name, c.type !== undefined ? c.type : c.node_type, c), 'Contact URI copied'), 'small'),
            button('Remove', () => busy(nodesCard, importNote, async () => {
              await api('POST', '/api/lora/contacts', { action: 'delete', pub_key: c.pub_key });
              toast('Contact removed');
              const fresh = await api('GET', '/api/status');
              st = fresh;
              showNodes(fresh);
            }), 'small danger')
          )
        );
      }));
      contactsBox.scrollTop = prevContactsScroll;
    }

    if (advs.length === 0) {
      advBox.replaceChildren(h('p', { class: 'muted' }, 'No adverts heard yet'));
    } else {
      const contactKeys = new Set(contacts.map((c) => (c.pub_key || '').toLowerCase()));
      const list = h('ul', { class: 'list' });
      for (const a of advs) {
        const typeBadge = h('span', { class: `badge ${nodeTypeName(a.node_type).toLowerCase()}`, text: nodeTypeName(a.node_type) });
        const locStr = a.has_loc && a.lat !== null && a.lat !== undefined ? `GPS: ${Number(a.lat).toFixed(4)}, ${Number(a.lon).toFixed(4)} · ` : '';
        const ageStr = (a.age_s !== undefined && a.age_s !== null) ? `${Math.round(a.age_s)}s ago` : '—';
        const meta = `${a.hops ?? 0} hops · ${a.rssi_dbm} dBm / ${a.snr_db} dB · ${locStr}${ageStr}`;
        const isSaved = a.pub_key && contactKeys.has(a.pub_key.toLowerCase());
        list.append(h('li', {},
          h('div', { class: 'grow' },
            h('strong', { text: a.name || a.pub_key || 'Unknown' }), ' ',
            typeBadge,
            h('div', { class: 'muted small', text: meta })
          ),
          a.pub_key ? h('div', { class: 'contact-actions' },
            button('Map', () => {
              if (loraActiveTab !== 'all' && loraActiveTab !== 'map') setLoraTab('map');
              mapSelectedKey = (a.pub_key || a.name || '').toLowerCase();
              if (a.has_loc && hasGps(a)) {
                mapCenterLat = Number(a.lat);
                mapCenterLon = Number(a.lon);
                mapViewMode = 'geo';
              }
              drawRadar(st);
              updateInspector(st, true);
            }, 'small'),
            button('Message', () => openDmTab(a.pub_key, a.name, true), 'small'),
            isSaved
              ? h('span', { class: 'badge saved', text: 'Saved' })
              : button('+ Contact', () => busy(nodesCard, importNote, async () => {
                await api('POST', '/api/lora/contacts', {
                  action: 'save',
                  pub_key: a.pub_key,
                  name: a.name || a.pub_key.slice(0, 8),
                  type: a.node_type || 1,
                  ...(a.has_loc && a.lat != null && a.lon != null ? { lat: a.lat, lon: a.lon } : {}),
                });
                toast('Contact added');
                const fresh = await api('GET', '/api/status');
                st = fresh;
                showNodes(fresh);
              }), 'small')
          ) : null
        ));
      }
      advBox.replaceChildren(list);
    }

    drawRadar(status);
    updateInspector(status);
  };
  contactFilterInput.addEventListener('input', () => {
    contactFilterQuery = contactFilterInput.value || '';
    showNodes(st);
  });
  showNodes(st);

  const nodesCard = card('Neighbors & heard nodes',
    h('h3', { text: 'Interactive Geo Map / Polar Radar' }),
    mapToolbar,
    radarCanvas,
    noGpsStrip,
    mapLegend,
    inspectorBox,
    h('h3', { text: 'Zero-Hop Repeaters' }),
    nbrBox,
    contactsHeading,
    contactFilterInput,
    contactsBox,
    actions(importToggleBtn),
    importPanel,
    h('h3', { text: 'Recent Adverts' }),
    advBox
  );

  /* Card 4: LoRa & MeshCore settings */
  const enabled = h('input', { type: 'checkbox', name: 'enabled', checked: lora.enabled !== false });
  enabled.name = 'enabled';
  const nodeName = h('input', { type: 'text', name: 'node_name', maxLength: '31', value: lora.node_name || 'NM-EPD-420' });
  nodeName.name = 'node_name';

  const blePinVal = lora.ble_pin ? String(lora.ble_pin).padStart(6, '0') : '';
  const blePin = h('input', {
    type: 'text',
    id: 'f-lora-ble-pin',
    name: 'ble_pin',
    maxLength: '6',
    placeholder: 'Auto (000000)',
    value: blePinVal,
  });
  blePin.id = 'f-lora-ble-pin';
  blePin.setAttribute('id', 'f-lora-ble-pin');
  blePin.name = 'ble_pin';

  const genPinBtn = button('Generate PIN', () => {
    const randomPin = Math.floor(100000 + Math.random() * 900000);
    blePin.value = String(randomPin);
  }, 'small');
  genPinBtn.id = 'btn-lora-gen-pin';
  genPinBtn.setAttribute('id', 'btn-lora-gen-pin');

  const blePinRow = h('div', { class: 'inline' }, blePin, ' ', genPinBtn);

  const shareLoc = h('input', { type: 'checkbox', name: 'share_loc', checked: !!lora.share_loc });
  shareLoc.name = 'share_loc';
  const channelName = h('input', { type: 'text', name: 'channel_name', maxLength: '23', value: lora.channel_name || 'Public' });
  channelName.name = 'channel_name';
  const hasCustomPsk = !!(lora.keys && (lora.keys.psk || lora.keys.channel_psk)) ||
    (typeof lora.channel_psk === 'string' && lora.channel_psk !== '' && lora.channel_psk !== 'izOH6cXN6mrJ5e26oRXNcg==');
  const channelPsk = secretInput('Channel PSK (Base64)', hasCustomPsk,
    'AES-128 key in Base64. Leave empty or clear to use the default Public channel key.');
  channelPsk.input.name = 'channel_psk';
  channelPsk.input.maxLength = '47';
  const usePublicBtn = button('Use Public PSK', () => {
    channelName.value = 'Public';
    channelPsk.cleared = true;
    channelPsk.input.value = '';
    channelPsk.note.textContent = 'Public channel default PSK will be used when you save.';
  }, 'small');
  usePublicBtn.id = 'btn-lora-public-psk';
  usePublicBtn.setAttribute('id', 'btn-lora-public-psk');

  const freqKhz = lora.freq_khz || 868000;
  const freq = h('input', { type: 'number', name: 'freq_mhz', min: '430', max: '928', step: '0.001',
                           value: (freqKhz / 1000).toFixed(3) });
  freq.name = 'freq_mhz';
  const sf = choose(LORA_SF, String(lora.sf || 7));
  sf.name = 'sf';
  const bwInit = lora.bw_hz ? (lora.bw_hz % 1000 === 0 ? String(lora.bw_hz / 1000) : String(+(lora.bw_hz / 1000).toFixed(2))) : String(lora.bw_khz || 250);
  const bw = choose(LORA_BW, bwInit);
  bw.name = 'bw_khz';
  const cr = choose(LORA_CR, String(lora.cr || 5));
  cr.name = 'cr';
  const txPower = h('input', { type: 'number', name: 'tx_power_dbm', min: '-9', max: '22', step: '1',
                              value: String(lora.tx_power_dbm !== undefined ? lora.tx_power_dbm : 22) });
  txPower.name = 'tx_power_dbm';

  const swVal = lora.sync_word !== undefined ? lora.sync_word : 0x1424;
  const initialSw = (typeof swVal === 'number' ? swVal.toString(16) : String(swVal).replace(/^0x/i, '')).toUpperCase().padStart(4, '0');
  const syncWord = h('input', { type: 'text', name: 'sync_word', value: initialSw, autocapitalize: 'characters', spellcheck: 'false' });
  syncWord.name = 'sync_word';
  const rxBoosted = h('input', { type: 'checkbox', name: 'rx_boosted', checked: lora.rx_boosted !== false });
  rxBoosted.name = 'rx_boosted';
  const cadEnabled = h('input', { type: 'checkbox', name: 'cad_enabled', checked: !!lora.cad_enabled });
  cadEnabled.name = 'cad_enabled';
  const clientRepeat = h('input', { type: 'checkbox', name: 'client_repeat', checked: !!lora.client_repeat });
  clientRepeat.name = 'client_repeat';
  const pathHash = choose(LORA_PATH_HASH, String(lora.path_hash_mode || 0));
  pathHash.name = 'path_hash_mode';

  const afVal = lora.airtime_factor_x10 !== undefined ? (lora.airtime_factor_x10 / 10).toFixed(1) : (lora.airtime_factor !== undefined ? Number(lora.airtime_factor).toFixed(1) : '1.0');
  const airtimeFactor = h('input', { type: 'text', inputmode: 'decimal', name: 'airtime_factor', value: afVal });
  airtimeFactor.name = 'airtime_factor';

  const wakeOnRx = h('input', { type: 'checkbox', name: 'wake_on_rx', checked: !!lora.wake_on_rx });
  wakeOnRx.name = 'wake_on_rx';

  const applyPreset = (f, sfVal, bwVal, crVal, pwrVal, swVal) => {
    freq.value = Number(f).toFixed(3);
    sf.value = String(sfVal);
    bw.value = String(bwVal);
    cr.value = String(crVal);
    if (pwrVal !== undefined) txPower.value = String(pwrVal);
    syncWord.value = swVal;
    updateComposerMeta();
  };

  const presetActions = actions(
    button('MeshCore EU/UK Narrow', () => applyPreset(869.618, 8, 62.5, 5, 22, '1424')),
    button('MeshCore EU868', () => applyPreset(868, 11, 250, 5, 22, '1424')),
    button('MeshCore CZ/SK Narrow', () => applyPreset(869.432, 7, 62.5, 5, 22, '1424')),
    button('MeshCore US/CA/AU', () => applyPreset(910.525, 7, 62.5, 5, 22, '1424')),
    button('MeshCore AU Narrow', () => applyPreset(916.575, 7, 62.5, 8, 22, '1424')),
    button('MeshCore NZ Narrow', () => applyPreset(917.375, 7, 62.5, 5, 22, '1424')),
    button('EU868 Default', () => applyPreset(868, 7, 250, 5, 22, '1424')),
    button('Meshtastic EU868 LongFast', () => applyPreset(869.525, 11, 250, 5, 22, '2B24')),
  );

  /* Channels 0-7 subsection */
  const channelsList = (st.lora && st.lora.channels) || [];
  const chRows = [];
  for (let i = 0; i < 8; i++) {
    const ch = channelsList.find((c) => c.index === i) || { index: i, enabled: i === 0, name: i === 0 ? 'Public' : '' };
    const chEn = h('input', { type: 'checkbox', checked: !!(ch.enabled || i === 0), disabled: i === 0 });
    const chNameIn = h('input', { type: 'text', maxLength: '23', value: ch.name || (i === 0 ? 'Public' : '') });
    const chKeyMode = choose([['keep', 'keep'], ['public', 'public'], ['passphrase', 'passphrase'], ['base64', 'base64'], ['hex', 'hex']], (ch.enabled || i === 0) ? 'keep' : 'passphrase');
    const chKeyIn = h('input', { type: 'text', placeholder: 'Key / passphrase', disabled: (chKeyMode.value === 'keep' || chKeyMode.value === 'public') });
    chKeyMode.addEventListener('change', () => {
      const m = chKeyMode.value;
      chKeyIn.disabled = (m === 'keep' || m === 'public');
    });
    const saveSlotBtn = button('Save slot', () => busy(radioCard, note, async () => {
      await api('POST', '/api/lora/channels', {
        index: i,
        enabled: i === 0 ? true : chEn.checked,
        name: chNameIn.value.trim(),
        key_mode: chKeyMode.value,
        key: chKeyIn.value,
      });
      if (i === 0) channelName.value = chNameIn.value.trim();
      toast(`Channel #${i} saved`);
      const fresh = await api('GET', '/api/status');
      st = fresh;
      showChat(fresh);
    }), 'small');
    chRows.push(h('tr', {},
      h('td', { 'data-label': '#', text: `#${i}` }),
      h('td', { 'data-label': 'Enabled' }, chEn),
      h('td', { 'data-label': 'Name' }, chNameIn),
      h('td', { 'data-label': 'Key mode' }, chKeyMode),
      h('td', { 'data-label': 'Key' }, chKeyIn),
      h('td', { 'data-label': 'Action' }, saveSlotBtn)
    ));
  }
  const channelsTable = h('div', {
    class: 'channels-table-wrap',
    tabindex: '0',
    role: 'region',
    'aria-label': 'Channels 0 to 7',
  },
    h('table', { id: 'lora-channels-table', class: 'channels-table' },
      h('thead', {}, h('tr', {},
        h('th', { text: '#' }),
        h('th', { text: 'Enabled' }),
        h('th', { text: 'Name' }),
        h('th', { text: 'Key mode' }),
        h('th', { text: 'Key' }),
        h('th', { text: '' })
      )),
      h('tbody', {}, ...chRows)
    )
  );

  const gridField = (...a) => h('div', {}, ...field(...a));
  const note = h('p');
  const radioCard = card('LoRa & MeshCore settings',
    presetActions,
    h('label', { class: 'check' }, enabled, 'Enabled'),
    field('Node name', nodeName, 'Up to 31 characters.'),
    field('BLE Companion PIN', blePinRow, '6-digit PIN (100000–999999) for companion app pairing. Leave blank or 0 for auto-generated PIN.'),
    h('label', { class: 'check' }, shareLoc, 'Share location in adverts'),
    field('Channel name', channelName, 'Up to 23 characters.'),
    channelPsk.el,
    actions(usePublicBtn),
    h('div', { class: 'lora-settings-grid' },
      gridField('Frequency (MHz)', freq, '430.000 to 928.000 MHz.'),
      gridField('Spreading factor', sf),
      gridField('Bandwidth', bw),
      gridField('Coding rate', cr),
      gridField('TX power (dBm)', txPower, '-9 to +22 dBm.'),
      gridField('Sync word', syncWord, 'Hexadecimal (e.g. 1424 or 0x1424).'),
      gridField('Path hash size', pathHash),
      gridField('Airtime factor (0–9)', airtimeFactor, 'Airtime factor for duty cycle regulation (0.0 to 9.0).')
    ),
    h('label', { class: 'check' }, rxBoosted, 'RX boosted gain'),
    h('label', { class: 'check' }, cadEnabled, 'CAD (listen before talk)'),
    h('label', { class: 'check' }, clientRepeat, 'Client repeat (forward flood)'),
    h('label', { class: 'check' }, wakeOnRx, 'Wake from sleep on packet'),
    note,
    actions(button('Save', () => busy(radioCard, note, async () => {
      const f = Number(freq.value);
      if (freq.value === '' || isNaN(f) || f < 430 || f > 928) {
        throw new ApiError('Frequency must be between 430 and 928 MHz.');
      }
      const swStr = (syncWord.value || '').trim().replace(/^0x/i, '');
      if (!/^[0-9a-fA-F]{1,4}$/.test(swStr)) {
        throw new ApiError('Sync word must be a valid hex value (0 to FFFF).');
      }
      const sw = parseInt(swStr, 16);
      const bwKhzNum = Number(bw.value);
      const bwHzNum = Math.round(bwKhzNum * 1000);
      const afRaw = (airtimeFactor.value || '').trim().replace(',', '.');
      const afNum = afRaw === '' ? 1.0 : Number(afRaw);
      if (!Number.isFinite(afNum) || afNum < 0 || afNum > 9) {
        throw new ApiError('Airtime factor must be between 0.0 and 9.0.');
      }
      const afX10 = Math.round(afNum * 10);
      const txPwr = Number(txPower.value);

      let pinNum = 0;
      const pinStr = (blePin.value || '').trim();
      if (pinStr !== '' && pinStr !== '0') {
        pinNum = Number(pinStr);
        if (isNaN(pinNum) || pinNum < 100000 || pinNum > 999999) {
          throw new ApiError('BLE PIN must be 0 (auto) or a 6-digit number between 100000 and 999999.');
        }
      }

      const loraPatch = {
        enabled: enabled.checked,
        ble_pin: pinNum,
        freq_khz: Math.round(f * 1000),
        sf: Number(sf.value),
        bw_khz: bwKhzNum,
        bw_hz: bwHzNum,
        cr: Number(cr.value),
        tx_power_dbm: isNaN(txPwr) ? 22 : txPwr,
        sync_word: sw,
        rx_boosted: rxBoosted.checked,
        cad_enabled: cadEnabled.checked,
        client_repeat: clientRepeat.checked,
        path_hash_mode: Number(pathHash.value),
        airtime_factor: afNum,
        airtime_factor_x10: afX10,
        wake_on_rx: wakeOnRx.checked,
        share_loc: shareLoc.checked,
        node_name: nodeName.value.trim() || 'NM-EPD-420',
        channel_name: channelName.value.trim() || 'Public',
      };
      const pskVal = channelPsk.value();
      if (pskVal !== undefined) {
        loraPatch.channel_psk = typeof pskVal === 'string' ? pskVal.trim() : pskVal;
      }

      await api('PATCH', '/api/settings', {
        schema: 1,
        lora: loraPatch,
      });
      if (pskVal !== undefined) {
        channelPsk.saved(Boolean(loraPatch.channel_psk && loraPatch.channel_psk !== 'izOH6cXN6mrJ5e26oRXNcg=='));
      }
      note.className = 'good';
      note.textContent = 'Saved.';
      toast('Saved');
      try {
        const freshStatus = await api('GET', '/api/status');
        st = freshStatus;
        showNow(freshStatus);
        showChat(freshStatus);
        showNodes(freshStatus);
      } catch (_) {}
    }), 'primary')),
    h('h3', { text: 'Channels 0–7 (MeshCore Companion Slots)' }),
    channelsTable
  );

  /* Card 5: MeshCore CLI */
  const cliInput = h('input', { type: 'text', id: 'lora-cli-input', placeholder: 'e.g. get radio, stats-packets, set tx 22…' });
  cliInput.setAttribute('id', 'lora-cli-input');
  const cliOutput = h('pre', { id: 'lora-cli-output' }, '(ready)');
  cliOutput.setAttribute('id', 'lora-cli-output');
  const cliNote = h('p');

  const runCli = (cmd) => {
    const c = (cmd || '').trim();
    if (!c) return;
    busy(cliCard, cliNote, async () => {
      try {
        const res = await api('POST', '/api/lora/cli', { cmd: c });
        cliOutput.textContent = res.reply || '(empty reply)';
        try {
          const fresh = await api('GET', '/api/status');
          st = fresh;
          showNow(fresh);
          showChat(fresh);
          showNodes(fresh);
        } catch (_) {}
      } catch (err) {
        cliOutput.textContent = `Error: ${err.message}`;
      }
    });
  };

  const cliRun = button('Run', () => runCli(cliInput.value), 'primary');
  cliRun.id = 'lora-cli-run';
  cliRun.setAttribute('id', 'lora-cli-run');

  cliInput.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter') runCli(cliInput.value);
  });

  const quickCmds = ['ver', 'get radio', 'stats-radio', 'stats-packets', 'neighbors', 'contacts', 'channels', 'help'];
  const cliQuickActions = actions(
    ...quickCmds.map((cmd) => button(cmd, () => {
      cliInput.value = cmd;
      runCli(cmd);
    }))
  );

  const cliCard = card('MeshCore CLI',
    cliQuickActions,
    h('div', { class: 'actions' }, cliInput, cliRun),
    cliNote,
    cliOutput
  );

  const subtabDefs = [
    { id: 'all', label: 'All sections' },
    { id: 'chat', label: 'Chat' },
    { id: 'map', label: 'Map & Nodes' },
    { id: 'settings', label: 'Settings & Channels' },
    { id: 'cli', label: 'CLI' },
  ];
  const panelMap = { chat: chatCard, map: nodesCard, settings: radioCard, cli: cliCard };
  const subtabButtons = {};
  const setLoraTab = (id, moveFocus = false) => {
    loraActiveTab = id;
    for (const def of subtabDefs) {
      const btn = subtabButtons[def.id];
      const active = def.id === id;
      btn.setAttribute('aria-selected', active ? 'true' : 'false');
      btn.setAttribute('tabindex', active ? '0' : '-1');
      if (active && moveFocus && typeof btn.focus === 'function') btn.focus();
    }
    for (const [pid, panel] of Object.entries(panelMap)) {
      const visible = id === 'all' || id === pid;
      if (visible) {
        if (typeof panel.removeAttribute === 'function') panel.removeAttribute('hidden');
        else if (panel.attrs) delete panel.attrs.hidden;
        panel.hidden = false;
      } else {
        panel.setAttribute('hidden', 'until-found');
        panel.hidden = true;
      }
    }
  };
  const subtabBar = h('div', { class: 'lora-subtabs', role: 'tablist', 'aria-label': 'LoRa sections' });
  subtabBar.setAttribute('role', 'tablist');
  subtabBar.setAttribute('aria-label', 'LoRa sections');
  subtabDefs.forEach((def, idx) => {
    const btn = h('button', {
      type: 'button',
      id: `lora-subtab-${def.id}`,
      class: 'lora-subtab',
      role: 'tab',
      'aria-selected': 'false',
      tabindex: '-1',
      text: def.label,
    });
    btn.setAttribute('role', 'tab');
    btn.setAttribute('id', `lora-subtab-${def.id}`);
    btn.addEventListener('click', () => setLoraTab(def.id));
    btn.addEventListener('keydown', (ev) => {
      if (ev.key === 'ArrowRight' || ev.key === 'ArrowLeft') {
        ev.preventDefault();
        const nextIdx = ev.key === 'ArrowRight'
          ? (idx + 1) % subtabDefs.length
          : (idx - 1 + subtabDefs.length) % subtabDefs.length;
        setLoraTab(subtabDefs[nextIdx].id, true);
      }
    });
    subtabButtons[def.id] = btn;
    subtabBar.append(btn);
  });
  for (const [pid, panel] of Object.entries(panelMap)) {
    panel.setAttribute('role', 'tabpanel');
    panel.setAttribute('aria-labelledby', `lora-subtab-${pid}`);
    panel.addEventListener('beforematch', () => setLoraTab(pid));
  }
  setLoraTab(loraActiveTab);

  showChat(st);
  updateComposerMeta();
  main.replaceChildren(h('h1', { text: 'LoRa' }), subtabBar, nowCard, chatCard, nodesCard, radioCard, cliCard);

  let pollBusy = false;
  const scheduleLoraPoll = () => {
    if (loraPollTimer !== null) {
      clearTimeout(loraPollTimer);
      loraPollTimer = null;
    }
    if (pollBusy) return;
    pollBusy = true;
    loraPollTimer = setTimeout(async () => {
      loraPollTimer = null;
      if (location.hash !== '#lora') {
        pollBusy = false;
        return;
      }
      let stopPoll = false;
      try {
        let fresh;
        const curLora = (st && st.lora) || {};
        const hasSeq = curLora.msg_seq !== undefined && curLora.advert_seq !== undefined && Array.isArray(curLora.contacts);
        const chatUrl = hasSeq
          ? `/api/lora/chat?msg_seq=${curLora.msg_seq}&adv_seq=${curLora.advert_seq}&contacts=${curLora.contacts.length}`
          : '/api/lora/chat';
        try {
          fresh = await api('GET', chatUrl);
        } catch (e) {
          if (e.status === 404) fresh = await api('GET', '/api/status');
          else throw e;
        }
        if (fresh && fresh.unchanged && fresh.lora) {
          st = { ...st, lora: { ...(st.lora || {}), ...fresh.lora } };
          showNow(st);
        } else {
          const merged = fresh && fresh.lora
            ? { ...st, ...fresh, lora: { ...(st.lora || {}), ...fresh.lora } }
            : { ...st, lora: { ...(st.lora || {}), ...(fresh || {}) } };
          const newMsgs = (merged.lora && merged.lora.messages) || [];
          let gotNewIncoming = false;
          for (const m of newMsgs) {
            if (m.id && m.id > loraSeenMaxId) {
              loraSeenMaxId = m.id;
              if (!m.outgoing) {
                if (m.direct && m.peer_pub) {
                  const pKey = String(m.peer_pub).toLowerCase();
                  if (!loraDmTargets.some((t) => t.pub_key.toLowerCase() === pKey)) {
                    loraDmTargets.push({ pub_key: pKey, name: m.sender || pKey.slice(0, 8) });
                  }
                }
                const key = loraMsgKey(m);
                if (loraActiveTarget !== key && loraActiveTarget !== 'all') {
                  loraUnreadByKey[key] = (loraUnreadByKey[key] || 0) + 1;
                }
                gotNewIncoming = true;
              }
            }
          }
          if (gotNewIncoming) {
            updateTitleUnread();
            if (loraSoundEnabled) loraPlayBeep();
          }
          st = merged;
          showNow(merged);
          showChat(merged);
          showNodes(merged);
        }
      } catch (e) {
        if (e && e.relogin) stopPoll = true;
      }
      pollBusy = false;
      if (!stopPoll && location.hash === '#lora') scheduleLoraPoll();
    }, 2500);
  };
  scheduleLoraPoll();
}

/* ---- Show as (spec §5.3, §10.3, M7, D44) ---- */

const SHOW_AS = [['', 'Value'], ['chart', 'History chart'], ['bar', 'Bar'], ['ring', 'Ring'], ['gauge', 'Gauge']];
const SPANS = [10, 30, 60, 300, 900, 1800, 3600, 7200, 21600, 43200, 86400];
const spanLabel = (s) => (s % 3600 === 0 ? `${s / 3600} h` : s >= 120 && s % 60 === 0 ? `${s / 60} min` : `${s} s`);
/* Whether a visual has room in w×h, by the rules GET /api/layouts publishes. */
function showFits(as, w, hgt) {
  const v = (catalogue.visuals || {})[as];
  return !!v && w >= v.min_w && hgt >= v.min_h;
}
/* A slot's or a cell's visual, for a field that is a number: the visuals that fit, a minimum and maximum (empty for
 * the field's own, auto for a chart's), a chart's span and its numbers. `set` gets the new one, or undefined. */
/* The field a slot names: its catalogue's entry, or a PC reading the PC no longer sends, still a number. */
const fieldNamed = (ed, id) => ed.fields.find((f) => f.id === id)
  || (id && id.startsWith('pc.') ? { id, label: id, kind: 'number', numeric: true } : undefined);

function showEditor(f, w, hgt, vis, set) {
  if (!f || !f.numeric) return null;
  let cur = vis; /* the controls below change it one after another, without a redraw between */
  const update = (next, rerender) => {
    cur = next;
    set(next, rerender);
  };
  const offered = SHOW_AS.filter(([as]) => !as || showFits(as, w, hgt));
  const as = vis && offered.some(([a]) => a === vis.as) ? vis.as : '';
  const kind = choose(offered, as);
  kind.addEventListener('change', (ev) => {
    const a = ev.target.value;
    update(!a ? undefined : a === 'chart' ? { as: a, span_s: 60, numbers: true } : { as: a }, true);
  });
  if (!as) return h('div', { class: 'show' }, field('Show as', kind));
  const ownRange = f.min !== undefined && f.max !== undefined;
  const bound = (key, label) => h('div', {}, field(label, h('input', {
    type: 'text', inputmode: 'decimal', value: cur[key] === undefined ? '' : String(cur[key]),
    placeholder: ownRange ? String(f[key]) : as === 'chart' ? 'auto' : '',
    onchange: (ev) => {
      const t = ev.target.value.trim().replace(',', '.'), next = { ...cur };
      if (t === '') delete next[key];
      else if (as === 'chart' && t.toLowerCase() === 'auto') next[key] = 'auto';
      else if (Number.isFinite(Number(t))) next[key] = Number(t);
      else {
        ev.target.value = cur[key] === undefined ? '' : String(cur[key]);
        return toast(as === 'chart' ? 'A number, or auto' : 'A number');
      }
      update(next, false);
    } })));
  const parts = [field('Show as', kind), h('div', { class: 'row' }, bound('min', 'Minimum'), bound('max', 'Maximum'))];
  if (!ownRange && as !== 'chart') {
    parts.push(h('p', { class: 'muted small', text: 'This field has no range of its own: left empty, it runs from 0 to ' +
      'a round number past the value.' }));
  }
  if (as === 'chart') {
    const spans = SPANS.includes(vis.span_s ?? 60) ? SPANS : [...SPANS, vis.span_s].sort((a, b) => a - b);
    const span = choose(spans.map((x) => [String(x), spanLabel(x)]), String(vis.span_s ?? 60));
    span.addEventListener('change', (ev) => update({ ...cur, span_s: Number(ev.target.value) }, false));
    parts.push(field('Span', span, 'Up to 1 h from each value; longer from each minute\'s low, mean and high.'),
      h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: vis.numbers !== false,
        onchange: (ev) => update({ ...cur, numbers: ev.target.checked }, false) }), 'Numbers over the chart'));
  }
  return h('div', { class: 'show' }, parts);
}

const LANGUAGES = [['en', 'English'], ['cs', 'Čeština']];
const SENSOR_MIN = [1, 2, 5, 10, 15, 30];

async function devicePage() {
  const [s, st] = await Promise.all([api('GET', '/api/settings'), api('GET', '/api/status')]);
  const sensors = s.sensors || {}, display = s.display || {};
  const select = (pairs, current) => h('select', {}, pairs.map(([v, t]) => h('option', { value: v, selected: v === current }, t)));
  const language = select(LANGUAGES, s.language || 'en');
  const unit = select([['C', '°C'], ['F', '°F']], (s.units || {}).temp === 'F' ? 'F' : 'C');
  const temp = h('input', { type: 'number', step: 0.1, min: -10, max: 10, value: sensors.temp_offset_c ?? 0 });
  const hum = h('input', { type: 'number', step: 0.5, min: -20, max: 20, value: sensors.hum_offset_pct ?? 0 });
  const every = select(SENSOR_MIN.map((m) => [String(m), `${m} min`]), String(sensors.interval_min ?? 5));
  const update = select(Array.from({ length: 15 }, (_, i) => [String(i + 1), `${i + 1} min`]), String(display.update_min ?? 1));
  const bat = s.battery || {};
  const learned = Array.isArray(bat.learned_mv); /* D21: a full discharge was learned */
  const levelFrom = select([['curve', 'The built-in Li-ion curve'], ['manual', 'My own voltages'],
    ...(learned ? [['learned', `Learned from a discharge (${new Date(bat.learned_at * 1000).toISOString().slice(0, 10)})`]] : [])],
    bat.level_from === 'manual' || (learned && bat.level_from === 'learned') ? bat.level_from : 'curve');
  const learn = (st.battery && st.battery.learn) || { state: 'off', hours: 0 };
  const learnNote = h('p', { class: 'muted small', text: {
    waiting: 'Learning: waiting for a full charge. Charge the battery full, then unplug it.',
    recording: `Learning: ${learn.hours} h of discharge so far. It ends when the battery is low.`,
    failed: 'The last discharge was too short to learn from; the next full charge tries again.',
  }[learn.state] || 'The device can learn this battery\'s own curve from one full discharge: charge it full, ' +
    'unplug it, and it learns until the battery is low, about a week on this board. It then uses that curve.' });
  const running = ['waiting', 'recording', 'failed'].includes(learn.state);
  const learnButton = button(running ? 'Stop learning' : 'Learn from the next full discharge', () => busy(batCard,
    learnNote, async () => {
      await api('POST', '/api/battery/learn', running ? { stop: true } : { start: true });
      await devicePage();
    }));
  const fullV = h('input', { type: 'number', step: 0.01, min: 3.6, max: 4.4, value: bat.full_v ?? 4.2 });
  const emptyV = h('input', { type: 'number', step: 0.01, min: 3, max: 4, value: bat.empty_v ?? 3.27 });
  const canUseCurV = Boolean(st.battery && st.battery.mv >= 4000 && st.battery.usb);
  const useCurVButton = button('Use current voltage as 100%', () => {
    if (!canUseCurV) return;
    levelFrom.value = 'manual';
    levelFrom.dispatchEvent(new Event('change'));
    fullV.value = (Math.round(st.battery.mv / 10) / 100).toFixed(2);
  });
  if (!canUseCurV) {
    useCurVButton.disabled = true;
    useCurVButton.title = 'Connect USB with battery >= 4.00 V to use current voltage as 100%';
  }
  const note = h('p');
  const batCard = card('Battery', field('Battery level from', levelFrom), field('Full, V', fullV), field('Empty, V', emptyV,
    'With your own voltages the level follows the Li-ion curve stretched between them. Read them on the ' +
    'Status page: full just after the charger stops, empty when the device shows its low-battery screen.'),
  learnNote, actions(learnButton, useCurVButton));
  const form = h('div', {},
    card('Language and units', field('Language on the device', language), field('Temperature', unit)),
    card('Sensors', field('Temperature offset, °C', temp, 'Added to every reading. The board warms the sensor by ' +
      'about 2 °C, which the default −2.0 makes up for; trim it against a thermometer you trust.'),
    field('Humidity offset, %', hum), field('Measure every', every)),
    card('Screen', field('Update every', update, 'How often the dashboard is redrawn. Each update wakes the device.')),
    batCard,
    note,
    actions(button('Save', () => busy(form, note, async () => {
      const t = Number(temp.value), hu = Number(hum.value);
      if (temp.value === '' || !(t >= -10 && t <= 10)) throw new ApiError('Temperature offset: -10 to 10 °C.');
      if (hum.value === '' || !(hu >= -20 && hu <= 20)) throw new ApiError('Humidity offset: -20 to 20 %.');
      const battery = { level_from: levelFrom.value };
      if (levelFrom.value === 'manual') {
        const fv = Number(fullV.value), ev = Number(emptyV.value);
        if (emptyV.value === '' || !(ev >= 3 && ev <= 4)) throw new ApiError('Empty: 3.0 to 4.0 V.');
        if (fullV.value === '' || !(fv >= 3.6 && fv <= 4.4)) throw new ApiError('Full: 3.6 to 4.4 V.');
        if (Math.round((fv - ev) * 100) < 30) throw new ApiError('Full must be at least 0.3 V above empty.');
        Object.assign(battery, { empty_v: Math.round(ev * 100) / 100, full_v: Math.round(fv * 100) / 100 });
      }
      await api('PATCH', '/api/settings', {
        language: language.value,
        units: { temp: unit.value },
        sensors: { temp_offset_c: Math.round(t * 10) / 10, hum_offset_pct: Math.round(hu * 2) / 2,
                   interval_min: Number(every.value) },
        display: { update_min: Number(update.value) },
        battery,
      });
      toast('Saved');
    }), 'primary')));
  main.replaceChildren(h('h1', { text: 'Device' }), form);
}

/* ---- Presets (spec §5.4) ---- */

const LAYOUT_NAMES = { classic: 'Classic', weather: 'Weather', grid: 'Grid', focus: 'Focus', radar: 'Radar',
                       flights: 'Flights', split: 'Split', solar: 'Solar', energy: 'Energy', lora: 'LoRa' };
const NO_SLOTS = { radar: 'This layout draws the weather radar: its centre and zoom are on the Radar page.',
                   flights: 'This layout draws the flight radar: its centre, range and filters are on the Radar page.',
                   solar: 'This layout draws today\'s PV forecast: its source is on the Solar page.',
                   energy: 'This layout draws the house\'s energy now: its source is on the Solar page.',
                   lora: 'This layout draws the LoRa MeshCore dashboard: nodes, signal and companion status.' };
/* The sync step a field's data come from (spec §9.3, D35): a field whose step is off ages out. */
const fieldStep = (id) => (/^(wx\.)/.test(id) ? 'weather' : /^(aq|pollen)\./.test(id) ? 'air' : id === 'rain.map' ? 'radar'
  : id.startsWith('pv.') ? 'solar' : id.startsWith('energy.') ? 'energy' : id.startsWith('pc.') ? 'pc'
  : id.startsWith('lora.') ? 'lora' : null);
const FIELD_GROUPS = [['solar', 'Solar forecast'], ['energy', 'House energy'], ['pc', 'PC'], ['lora', 'LoRa']];

/* A slot's or a cell's choices: the fields `fits` allows, the Solar and Energy ones in their groups, each marked
 * when its sync step is off. */
function fieldOptions(ed, fits, selected) {
  const option = (f) => h('option', { value: f.id, selected: selected === f.id },
    `${f.label} — ${f.value || 'no data yet'}${ed.stepsOff.has(fieldStep(f.id)) ? ' (its sync step is off)' : ''}`);
  const grouped = new Set(FIELD_GROUPS.map(([k]) => k));
  /* a PC reading the PC no longer sends (spec §12a.3): it stays, named, until the slot changes */
  const gone = selected && selected.startsWith('pc.') && !fits.some((f) => f.id === selected)
    ? h('option', { value: selected, selected: true }, `${selected} (not sent now)`) : null;
  return [h('option', { value: '' }, '(empty)'), gone, fits.filter((f) => !grouped.has(fieldStep(f.id))).map(option),
          FIELD_GROUPS.map(([k, label]) => {
            const list = fits.filter((f) => fieldStep(f.id) === k);
            return list.length ? h('optgroup', { label }, list.map(option)) : null;
          })];
}
const CYCLE_S = [10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600];
const cycleLabel = (s) => (s < 60 ? `${s} s` : s < 3600 ? `${s / 60} min` : `${s / 3600} h`);
const DAYS = ['Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa', 'Su']; /* bit 0 is Monday */

/* A cycle window's end (spec §5.4, D41): "HH:MM", or sunrise or sunset with "+N" or "-N" minutes. Read as it is
 * written, offsets out of range included, so the editor shows it and windowError() names what's wrong. */
function parseBound(text) {
  const m = /^(?:(\d\d:\d\d)|(sunrise|sunset)(?:([+-]\d+))?)$/.exec(text || '');
  if (!m) return { kind: 'time', time: '07:00' };
  return m[1] ? { kind: 'time', time: m[1] } : { kind: m[2], offset: Number(m[3] || 0) };
}
const boundOk = (text) => /^(?:([01]\d|2[0-3]):[0-5]\d|(sunrise|sunset)([+-](?:[1-9]\d?|1[0-7]\d|180))?)$/.test(text || '');
const windowText = (w) => `${w.from} – ${w.until}${(w.days ?? 127) === 127 ? ''
  : `, ${DAYS.filter((d, bit) => (w.days ?? 127) & (1 << bit)).join(' ')}`}`;

/* What the device would refuse in a preset's window (spec §5.4), as a sentence; null when it takes it. */
function windowError(p) {
  const w = p.window;
  if (!w) return null;
  if (!boundOk(w.from) || !boundOk(w.until)) return `${p.name}: a window's ends are times, or sunrise or sunset within 180 minutes.`;
  if (w.from === w.until) return `${p.name}: the window's from and until can't be the same.`;
  const days = w.days ?? 127;
  if (!Number.isInteger(days) || days < 1 || days > 127) return `${p.name}: a window opens on one day at least.`;
  return null;
}
let catalogue = null; /* GET /api/layouts, cached: it doesn't change while the page is open */

function uniqueId(doc, base) {
  const stem = (base.toLowerCase().normalize('NFD').replace(/[^a-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '') || 'preset')
    .slice(0, 12);
  let id = stem;
  for (let i = 2; doc.presets.some((p) => p.id === id); i++) id = `${stem}-${i}`.slice(0, 15);
  return id;
}

/* A title as the device keeps it: at most 23 bytes of UTF-8, cut where a letter starts (spec §10.3, D42). */
function cutTitle(s) {
  let out = '';
  for (const ch of s.trim()) {
    if (bytes(out + ch) > 23) break;
    out += ch;
  }
  return out;
}

/* A slot's or cell's own title, in place of its field's label: `set` gets it cut, or '' to clear it. */
function titleInput(value, f, set) {
  const input = h('input', { type: 'text', value: value || '', placeholder: f ? f.label : '', onchange: (ev) => {
    ev.target.value = cutTitle(ev.target.value);
    set(ev.target.value);
  } });
  return [h('label', { class: 'title-label', text: 'Title' }), input];
}

/* Import (spec §10.3, D42): a file Export wrote, or a bare preset. The device checks it as it checks a save, in its
 * place after the selected preset; one it refuses isn't added. */
async function importPreset(ed, input) {
  const f = input.files && input.files[0];
  input.value = '';
  if (!f) return;
  const doc = ed.doc;
  if (doc.presets.length >= 16) return toast('16 presets at most');
  let obj;
  try { obj = JSON.parse(await f.text()); } catch (e) { return toast('That file is not JSON.'); }
  const p = obj && obj.reflbo === 'preset' ? obj.preset : obj;
  if (!p || typeof p !== 'object' || typeof p.layout !== 'string') return toast('That file holds no preset.');
  const copy = JSON.parse(JSON.stringify(p));
  copy.name = String(copy.name || copy.id || 'Preset').slice(0, 23);
  copy.id = uniqueId(doc, copy.name);
  copy.in_cycle = true; /* a new preset shows up on KEY */
  const isObj = (x) => x !== null && typeof x === 'object' && !Array.isArray(x);
  copy.options = isObj(copy.options) ? copy.options : {};
  if (copy.layout === 'split') { /* the editor's shape: a tree, or slots, never neither */
    copy.split = isObj(copy.split) ? copy.split : {};
    delete copy.slots;
  } else {
    copy.slots = isObj(copy.slots) ? copy.slots : {};
    delete copy.split;
  }
  const presets = [...doc.presets.slice(0, ed.sel + 1), copy, ...doc.presets.slice(ed.sel + 1)];
  try {
    await api('POST', '/api/preview.bmp?preset=' + encodeURIComponent(copy.id), { ...doc, presets });
  } catch (e) {
    return toast(`Can't import ${copy.name}: ${e.message}`);
  }
  doc.presets = presets;
  ed.sel++;
  toast(`${copy.name} imported. Save keeps it.`);
  changed(ed, true);
}

async function presetsPage() {
  if (!catalogue) catalogue = await api('GET', '/api/layouts');
  const [doc, fieldList, settings] = await Promise.all([api('GET', '/api/presets'), api('GET', '/api/fields'),
    api('GET', '/api/settings').catch(() => ({}))]);
  const steps = (settings.sync || {}).steps;
  const ed = {
    doc, fields: fieldList.fields, dirty: false,
    stepsOff: new Set(Array.isArray(steps) ? STEP_SWITCHES.map(([k]) => k).filter((k) => !steps.includes(k)) : []),
    sel: Math.max(0, doc.presets.findIndex((p) => p.id === doc.active)),
    img: h('img', { class: 'screen', alt: 'Preview' }), previewNote: h('p', { class: 'small' }),
    timer: null, url: null, saveNote: h('p'),
  };
  if (!ed.doc.schedule) ed.doc.schedule = { enabled: false, entries: [] };
  renderPresets(ed);
  preview(ed);
}

/* The preview, with each slot's name at its top right corner, as the slot fields below call them
 * (the renderer puts captions top left); a split preset's cells by their numbers. */
function previewBox(ed, slots) {
  const pct = (v, of) => `${+(100 * v / of).toFixed(3)}%`;
  return h('div', { class: 'preview' }, ed.img, slots.map((slot) => {
    const tag = h('span', { class: 'slot-tag', text: slot.id });
    tag.style.left = pct(slot.x + slot.w, catalogue.width);
    tag.style.top = pct(slot.y, catalogue.height);
    return tag;
  }));
}

function layoutOf(id) {
  return catalogue.layouts.find((l) => l.id === id) || catalogue.layouts[0];
}

/* ---- the split layout (spec §5.2, D31): its tree, by the rules GET /api/layouts publishes ---- */

const splitArea = () => ({ x: catalogue.split.x, y: catalogue.split.y, w: catalogue.split.w, h: catalogue.split.h });

/* A split's two parts of r: the first gets its ratio, rounded down, then a 1 px gap. */
function splitParts(node, r) {
  const [num, den] = node.ratio.split('/').map(Number);
  if (node.split === 'columns') {
    const first = Math.floor(r.w * num / den);
    return [{ x: r.x, y: r.y, w: first, h: r.h }, { x: r.x + first + 1, y: r.y, w: r.w - first - 1, h: r.h }];
  }
  const first = Math.floor(r.h * num / den);
  return [{ x: r.x, y: r.y, w: r.w, h: first }, { x: r.x, y: r.y + first + 1, w: r.w, h: r.h - first - 1 }];
}

/* The cells under `node` with their rectangles, in the device's order: its error messages count them so. */
function splitCells(node, r, out = []) {
  if (!node.split) out.push({ node, r });
  else splitParts(node, r).forEach((part, i) => splitCells(i ? node.b : node.a, part, out));
  return out;
}

/* Every part of the tree is at least min_w × min_h. */
function splitFits(node, r) {
  if (r.w < catalogue.split.min_w || r.h < catalogue.split.min_h) return false;
  if (!node.split) return true;
  const [a, b] = splitParts(node, r);
  return splitFits(node.a, a) && splitFits(node.b, b);
}

/* A cell's size class, or the size a field of `kind` draws at in it: the first size, largest first, whose
 * least width and height it has; null if none. */
function splitSize(w, h, kind) {
  const wide = w >= catalogue.split.narrow_w ? 1 : 0;
  const size = catalogue.split.sizes.find((s) => {
    const need = kind ? s.kinds[kind] : s.min_h;
    return w >= s.min_w && need && h >= need[wide];
  });
  return size ? size.size : null;
}

const cellCount = (node) => (node.split ? cellCount(node.a) + cellCount(node.b) : 1);
const firstField = (node) => (node.split ? firstField(node.a) || firstField(node.b) : node.field);
const firstCell = (node) => (node.split ? firstCell(node.a) || firstCell(node.b) : node.field ? node : null);
/* A cell's field with its title and force, as they go together (spec §10.3, D42). */
const fieldPart = (cell) => {
  const out = { field: cell.field };
  if (cell.title) out.title = cell.title;
  if (cell.force_title) out.force_title = true;
  if (cell.show) out.show = cell.show; /* M7: its visual, where it still fits */
  return out;
};
/* A cell's visual that has no room in r any more goes; true if one went. */
function dropShow(cell, r) {
  if (cell.show && !showFits(cell.show.as, r.w, r.h)) {
    delete cell.show;
    return true;
  }
  return false;
}

/* The preview's tags: each cell's number where a slot's name goes. */
function slotsOf(p) {
  if (p.layout !== 'split') return layoutOf(p.layout).slots;
  return splitCells(p.split, splitArea()).map(({ r }, i) => ({ id: String(i + 1), ...r }));
}

/* The tree as nested boxes (spec §10.3): a cell's size, class (and its field's, when smaller) and the
 * fields that fit it, Split into rows or columns; a split's ratio, Separator and Join. */
function splitEditor(ed, p) {
  const fieldOf = (id) => ed.fields.find((f) => f.id === id);
  let number = 0;
  const box = (node, r, set) => {
    if (!node.split) {
      number++;
      const fits = ed.fields.filter((f) => splitSize(r.w, r.h, f.kind));
      const select = h('select', { onchange: (ev) => {
        const was = fieldOf(node.field);
        if (ev.target.value) node.field = ev.target.value;
        else delete node.field;
        const now = fieldOf(node.field);
        if (!now || !now.numeric) delete node.show; /* a visual shows a number (spec §5.4) */
        changed(ed, !!(was && was.numeric) !== !!(now && now.numeric));
      } }, fieldOptions(ed, fits, node.field));
      const splitButton = (dir, text) => {
        const half = { split: dir, ratio: '1/2', line: true, a: {}, b: {} }; /* the field goes to the first part */
        const b = button(text, () => {
          const f = fieldOf(node.field), [a] = splitParts(half, r);
          if (f && splitSize(a.w, a.h, f.kind)) {
            half.a = fieldPart(node);
            if (dropShow(half.a, a)) toast(`No room for its ${node.show.as}`);
          } else if (f) toast(`No room for ${f.label}`);
          set(half);
          changed(ed, true);
        });
        b.disabled = cellCount(p.split) >= catalogue.split.cells || !splitFits(half, r);
        return b;
      };
      const size = splitSize(r.w, r.h), f = fieldOf(node.field), at = f && splitSize(r.w, r.h, f.kind);
      const smaller = at && at !== size ? ` (${f.label} at ${at})` : '';
      const name = `${number} · ${r.w}×${r.h} · ${size || 'too small'}${smaller}`;
      const title = titleInput(node.title, f, (v) => {
        if (v) node.title = v;
        else delete node.title;
        changed(ed, false);
      });
      const force = node.field ? h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: !!node.force_title,
        onchange: (ev) => {
          if (ev.target.checked) node.force_title = true;
          else delete node.force_title;
          changed(ed, false);
        } }), 'Always show the title') : null;
      const show = showEditor(f || fieldNamed(ed, node.field), r.w, r.h, node.show, (v, rerender) => {
        if (v) node.show = v;
        else delete node.show;
        changed(ed, rerender);
      });
      return h('div', { class: 'cell' }, h('label', { class: 'cell-name', text: name }),
        select, title, force, show,
        actions(splitButton('rows', 'Split into rows'), splitButton('columns', 'Split into columns')));
    }
    const [ra, rb] = splitParts(node, r);
    const ratio = h('select', { onchange: (ev) => {
      node.ratio = ev.target.value;
      const dropped = []; /* a smaller cell may lose its field */
      for (const { node: cell, r: cr } of splitCells(p.split, splitArea())) {
        const f = fieldOf(cell.field);
        if (f && !splitSize(cr.w, cr.h, f.kind)) {
          delete cell.field; /* its title, force and visual with it */
          delete cell.title;
          delete cell.force_title;
          delete cell.show;
          dropped.push(f.label);
        } else if (f && cell.show && dropShow(cell, cr)) {
          dropped.push(`${f.label}'s visual`);
        }
      }
      if (dropped.length) toast(`No room for ${dropped.join(', ')}`);
      changed(ed, true);
    } }, catalogue.split.ratios.map((q) => h('option', { value: q, selected: node.ratio === q,
                                                         disabled: !splitFits({ ...node, ratio: q }, r) }, q)));
    return h('div', { class: 'split' }, h('div', { class: 'row' },
      h('span', { class: 'muted', text: node.split === 'columns' ? 'Columns' : 'Rows' }), ratio,
      h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: node.line !== false,
        onchange: (ev) => { node.line = ev.target.checked; changed(ed, false); } }), 'Separator'),
      button('Join', () => {
        const c = firstCell(node); /* it had room in a part, so it has room in the whole */
        set(c ? fieldPart(c) : {});
        changed(ed, true);
      })),
    box(node.a, ra, (x) => { node.a = x; }), box(node.b, rb, (x) => { node.b = x; }));
  };
  return h('div', { class: 'tree' }, box(p.split, splitArea(), (x) => { p.split = x; }));
}

/* The device renders the preset as it stands in the editor (POST /api/preview.bmp). */
function preview(ed) {
  clearTimeout(ed.timer);
  ed.timer = setTimeout(async () => {
    const p = ed.doc.presets[ed.sel];
    try {
      const bmp = await api('POST', '/api/preview.bmp?preset=' + encodeURIComponent(p.id), ed.doc);
      if (ed.url) URL.revokeObjectURL(ed.url);
      ed.url = URL.createObjectURL(bmp);
      ed.img.src = ed.url;
      ed.previewNote.textContent = '';
    } catch (e) {
      ed.previewNote.className = 'bad small';
      ed.previewNote.textContent = e.message;
    }
  }, 250);
}

function changed(ed, rerender) {
  ed.dirty = true;
  if (rerender) renderPresets(ed);
  else ed.saveBar && (ed.saveBar.hidden = false);
  preview(ed);
}

function renderPresets(ed) {
  const doc = ed.doc, p = doc.presets[ed.sel];
  const list = h('ul', { class: 'list' }, doc.presets.map((q, i) => h('li', { class: i === ed.sel ? 'sel' : null },
    h('input', { type: 'radio', name: 'active', title: 'Show this one now', checked: q.id === doc.active,
                 onchange: () => { doc.active = q.id; changed(ed, true); } }),
    h('button', { class: 'link', type: 'button', onclick: () => { ed.sel = i; renderPresets(ed); preview(ed); } },
      h('b', { text: q.name }), ' ', h('span', { class: 'muted small', text: LAYOUT_NAMES[q.layout] || q.layout })),
    h('label', { class: 'check small', title: 'KEY and the auto-cycle step through these' },
      h('input', { type: 'checkbox', checked: q.in_cycle, onchange: (ev) => { q.in_cycle = ev.target.checked; changed(ed, false); } }),
      'cycle'),
    q.window ? h('span', { class: 'muted small', text: ` ${windowText(q.window)}` }) : null)));
  const importInput = h('input', { type: 'file', accept: '.json,application/json', hidden: true,
                                   onchange: (ev) => importPreset(ed, ev.target) });
  const listCard = card('Presets', list, h('p', { class: 'muted small', text: 'Tap a preset to edit it. The dot marks ' +
    'the one on the screen now; "cycle" puts a preset in the order KEY and the auto-cycle step through.' }), importInput,
  actions(
    button('New preset', () => {
      if (doc.presets.length >= 16) return toast('16 presets at most');
      const copy = JSON.parse(JSON.stringify(p));
      copy.name = `${p.name} copy`.slice(0, 23);
      copy.id = uniqueId(doc, copy.name);
      copy.in_cycle = true; /* a new preset shows up on KEY, whatever it was copied from */
      doc.presets.splice(ed.sel + 1, 0, copy);
      ed.sel++;
      changed(ed, true);
    }),
    button('Import…', () => importInput.click())));

  const name = h('input', { type: 'text', maxlength: 23, value: p.name,
                            onchange: (ev) => { p.name = ev.target.value.trim() || p.id; changed(ed, true); } });
  const layout = h('select', { onchange: (ev) => {
    const old = p.slots || {};
    const first = p.layout === 'split' ? firstField(p.split)
                                       : layoutOf(p.layout).slots.map((s) => old[s.id]).find(Boolean);
    const oldTitles = p.titles || {}, oldVisuals = p.visuals || {};
    delete p.titles;
    delete p.visuals;
    p.layout = ev.target.value;
    if (p.layout === 'split') { /* spec §10.3: one cell, holding the first field */
      const f = ed.fields.find((x) => x.id === first), all = splitArea();
      delete p.slots;
      p.split = f && splitSize(all.w, all.h, f.kind) ? { field: f.id } : {};
      return changed(ed, true);
    }
    delete p.split;
    p.slots = {};
    const titles = {}, visuals = {};
    for (const slot of layoutOf(p.layout).slots) { /* keep what still fits, with its title and visual */
      const f = old[slot.id] && ed.fields.find((x) => x.id === old[slot.id]);
      if (f && slot.kinds.includes(f.kind)) {
        p.slots[slot.id] = f.id;
        if (oldTitles[slot.id]) titles[slot.id] = oldTitles[slot.id];
        const v = oldVisuals[slot.id];
        if (v && showFits(v.as, slot.w, slot.h)) visuals[slot.id] = v;
      }
    }
    if (Object.keys(titles).length) p.titles = titles;
    if (Object.keys(visuals).length) p.visuals = visuals;
    changed(ed, true);
  } }, catalogue.layouts.map((l) => h('option', { value: l.id, selected: l.id === p.layout }, LAYOUT_NAMES[l.id] || l.id)));
  const slots = p.layout === 'split' ? splitEditor(ed, p)
    : h('div', { class: 'slots' }, layoutOf(p.layout).slots.map((slot) => h('div', { class: 'slot' },
    h('label', { text: `${slot.id} · ${slot.size}` }),
    h('select', { onchange: (ev) => {
      const fieldOf = (id) => ed.fields.find((f) => f.id === id), was = fieldOf(p.slots[slot.id]);
      if (ev.target.value) p.slots[slot.id] = ev.target.value;
      else delete p.slots[slot.id];
      const now = fieldOf(p.slots[slot.id]);
      if ((!now || !now.numeric) && p.visuals && p.visuals[slot.id]) { /* a visual shows a number (spec §5.4) */
        delete p.visuals[slot.id];
        if (!Object.keys(p.visuals).length) delete p.visuals;
      }
      changed(ed, !!(was && was.numeric) !== !!(now && now.numeric));
    } }, fieldOptions(ed, ed.fields.filter((f) => slot.kinds.includes(f.kind)), p.slots[slot.id])),
    titleInput((p.titles || {})[slot.id], ed.fields.find((f) => f.id === p.slots[slot.id]), (v) => {
      const titles = { ...p.titles };
      if (v) titles[slot.id] = v;
      else delete titles[slot.id];
      if (Object.keys(titles).length) p.titles = titles;
      else delete p.titles;
      changed(ed, false);
    }),
    showEditor(fieldNamed(ed, p.slots[slot.id]), slot.w, slot.h, (p.visuals || {})[slot.id],
      (v, rerender) => {
        const visuals = { ...p.visuals };
        if (v) visuals[slot.id] = v;
        else delete visuals[slot.id];
        if (Object.keys(visuals).length) p.visuals = visuals;
        else delete p.visuals;
        changed(ed, rerender);
      }))));

  const o = p.options;
  const check = (key, text) => h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: !!o[key],
    onchange: (ev) => { o[key] = ev.target.checked; changed(ed, key === 'seconds'); } }), text);
  const clock = h('select', { onchange: (ev) => {
    if (ev.target.value === '') delete o.clock_24h;
    else o.clock_24h = ev.target.value === '24';
    changed(ed, false);
  } }, h('option', { value: '', selected: o.clock_24h === undefined }, 'As set for the device'),
  h('option', { value: '24', selected: o.clock_24h === true }, '24-hour'),
  h('option', { value: '12', selected: o.clock_24h === false }, '12-hour'));
  const stale = h('select', { onchange: (ev) => { o.stale_policy = ev.target.value; changed(ed, false); } },
    [['stale', 'Show it with its age'], ['placeholder', 'Show a dash'], ['hide', 'Leave the slot empty']]
      .map(([v, t]) => h('option', { value: v, selected: (o.stale_policy || 'stale') === v }, t)));
  const battery = new Set(o.status_battery || ['percent']);
  const part = (key, text) => h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: battery.has(key),
    onchange: (ev) => {
      ev.target.checked ? battery.add(key) : battery.delete(key);
      o.status_battery = ['percent', 'voltage', 'days'].filter((k) => battery.has(k));
      changed(ed, false);
    } }), text);
  /* The cycle window (spec §5.4, D41): each end a time, or sunrise or sunset with an offset, and the days. */
  const bound = (key) => {
    const b = parseBound(p.window[key]);
    const kind = choose([['time', 'At a time'], ['sunrise', 'Sunrise'], ['sunset', 'Sunset']], b.kind);
    kind.addEventListener('change', (ev) => {
      const k = ev.target.value, now = parseBound(p.window[key]); /* the offset as typed since the last render */
      p.window[key] = k === 'time' ? '07:00' : `${k}${now.offset ? (now.offset > 0 ? '+' : '') + now.offset : ''}`;
      changed(ed, true);
    });
    const value = b.kind === 'time'
      ? h('input', { type: 'time', value: b.time, onchange: (ev) => { p.window[key] = ev.target.value; changed(ed, false); } })
      : h('input', { type: 'number', min: -180, max: 180, step: 1, value: b.offset, title: 'Minutes after (+) or before (-)',
                     onchange: (ev) => {
                       const n = Number(ev.target.value);
                       p.window[key] = n ? `${b.kind}${n > 0 ? '+' : ''}${n}` : b.kind; /* 1.5 or 200: refused at Save */
                       changed(ed, false);
                     } });
    return h('div', { class: 'row' }, kind, value);
  };
  const cycleWindow = choose([['all', 'All day'], ['window', 'Within a window']], p.window ? 'window' : 'all');
  cycleWindow.addEventListener('change', (ev) => {
    if (ev.target.value === 'window') p.window = { from: 'sunrise', until: 'sunset', days: 127 };
    else delete p.window;
    changed(ed, true);
  });
  const windowDays = () => h('div', { class: 'days' }, DAYS.map((d, bit) => h('label', {}, h('input', { type: 'checkbox',
    checked: (p.window.days ?? 127) & (1 << bit), onchange: (ev) => {
      p.window.days = ((p.window.days ?? 127) & ~(1 << bit)) | (ev.target.checked ? 1 << bit : 0);
      changed(ed, false);
    } }), d)));

  const editCard = card(`Edit ${p.name}`, previewBox(ed, slotsOf(p)), ed.previewNote,
    field('Name', name), field('Layout', layout), slots,
    NO_SLOTS[p.layout] ? h('p', { class: 'muted small', text: NO_SLOTS[p.layout] }) : null,
    field('In the cycle', cycleWindow, 'The auto-cycle skips this preset while its window is closed; KEY still reaches it.'),
    p.window ? [field('From', bound('from')), field('Until', bound('until')), field('Days', windowDays()),
                h('p', { class: 'muted small', text: 'Sunrise and sunset are the location\'s, with minutes after (+) or ' +
                  'before (-). Evening to morning runs past midnight (sunset to sunrise, 22:00 to 06:00), as do two ' +
                  'times whose end comes first; a day when the sun puts the end before the start has no window.' })] : null,
    field('Time format', clock),
    check('invert', 'White on black'), field('Old or missing data', stale),
    h('label', {}, 'Status bar'), check('status_clock', 'A small clock'),
    h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: !!o.status_sync, onchange: (ev) => {
      if (ev.target.checked) o.status_sync = true;
      else delete o.status_sync; /* the device writes it only when on (spec §5.4) */
      changed(ed, false);
    } }), 'Last sync time'),
    part('percent', 'Battery level'), part('voltage', 'Battery voltage'), part('days', 'Days of battery left'),
    actions(
      button('Export', () => download(new Blob([JSON.stringify({ reflbo: 'preset', version: 1, preset: p }, null, 2)],
                                               { type: 'application/json' }), `${p.id}.json`)),
      button('Move up', () => { if (ed.sel > 0) { doc.presets.splice(ed.sel - 1, 0, doc.presets.splice(ed.sel, 1)[0]); ed.sel--; changed(ed, true); } }),
      button('Move down', () => { if (ed.sel < doc.presets.length - 1) { doc.presets.splice(ed.sel + 1, 0, doc.presets.splice(ed.sel, 1)[0]); ed.sel++; changed(ed, true); } }),
      button('Delete', () => {
        if (doc.presets.length < 2) return toast('The last preset stays');
        if (!confirm(`Delete ${p.name}?`)) return;
        doc.presets.splice(ed.sel, 1);
        doc.schedule.entries = doc.schedule.entries.filter((e) => e.action !== 'preset' || e.preset !== p.id);
        if (doc.active === p.id) doc.active = doc.presets[0].id;
        ed.sel = Math.min(ed.sel, doc.presets.length - 1);
        changed(ed, true);
      }, 'danger')));

  const intervals = CYCLE_S.includes(doc.cycle.interval_s) ? CYCLE_S : [...CYCLE_S, doc.cycle.interval_s].sort((a, b) => a - b);
  const cycleCard = card('Auto-cycle',
    h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: doc.cycle.enabled,
      onchange: (ev) => { doc.cycle.enabled = ev.target.checked; changed(ed, false); } }), 'Step through the presets by itself'),
    field('Every', h('select', { onchange: (ev) => { doc.cycle.interval_s = Number(ev.target.value); changed(ed, false); } },
      intervals.map((s) => h('option', { value: s, selected: s === doc.cycle.interval_s }, cycleLabel(s))))),
    h('p', { class: 'muted small', text: 'Each switch wakes the device: short intervals cost battery.' }));

  const sched = doc.schedule;
  const entry = (e, i) => {
    const at = h('input', { type: 'time', value: e.at, required: true, onchange: (ev) => { e.at = ev.target.value; changed(ed, false); } });
    const days = h('div', { class: 'days' }, DAYS.map((d, bit) => h('label', {}, h('input', { type: 'checkbox',
      checked: (e.days ?? 127) & (1 << bit), onchange: (ev) => {
        e.days = ((e.days ?? 127) & ~(1 << bit)) | (ev.target.checked ? 1 << bit : 0);
        changed(ed, false);
      } }), d)));
    const kind = h('select', { onchange: (ev) => {
      e.action = ev.target.value;
      if (e.action === 'night') { delete e.preset; e.until = e.until || '06:00'; }
      else { delete e.until; e.preset = e.preset || p.id; }
      changed(ed, true);
    } }, h('option', { value: 'preset', selected: e.action === 'preset' }, 'Switch to a preset'),
    h('option', { value: 'night', selected: e.action === 'night' }, 'Night: screen off until'));
    const what = e.action === 'night'
      ? h('input', { type: 'time', value: e.until, onchange: (ev) => { e.until = ev.target.value; changed(ed, false); } })
      : h('select', { onchange: (ev) => { e.preset = ev.target.value; changed(ed, false); } },
        doc.presets.map((q) => h('option', { value: q.id, selected: q.id === e.preset }, q.name)));
    return h('div', { class: 'entry' }, h('div', { class: 'row' }, at, kind, what), days,
      actions(button('Remove', () => { sched.entries.splice(i, 1); changed(ed, true); })));
  };
  const schedCard = card('Schedule',
    h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: sched.enabled,
      onchange: (ev) => { sched.enabled = ev.target.checked; changed(ed, false); } }), 'Run the schedule'),
    h('p', { class: 'muted small', text: 'At each time, on the ticked days, the device switches preset, or turns the ' +
      'screen off for the night. A press on KEY or BOOT shows the screen for a minute.' }),
    sched.entries.map(entry),
    actions(button('Add a time', () => {
      if (sched.entries.length >= 8) return toast('8 times at most');
      sched.entries.push({ at: '22:00', days: 127, action: 'preset', preset: p.id });
      changed(ed, true);
    })));

  ed.saveBar = card(null, ed.saveNote, actions(
    button('Save', () => busy(ed.saveBar, ed.saveNote, async () => {
      const bad = ed.doc.presets.map(windowError).find(Boolean);
      if (bad) throw new ApiError(bad);
      ed.doc = await api('PUT', '/api/presets', ed.doc);
      if (!ed.doc.schedule) ed.doc.schedule = { enabled: false, entries: [] };
      ed.dirty = false;
      toast('Presets saved');
      renderPresets(ed);
    }), 'primary'),
    button('Undo changes', () => {
      if (!confirm('Drop the changes that aren\'t saved?')) return; /* the bar floats over other buttons */
      presetsPage().catch(() => {});
    })));
  ed.saveBar.hidden = !ed.dirty;
  ed.saveBar.style.position = 'sticky';
  ed.saveBar.style.bottom = '8px';
  const y = window.scrollY; /* a re-render keeps the place on the page */
  main.replaceChildren(h('h1', { text: 'Presets' }), listCard, editCard, cycleCard, schedCard, ed.saveBar);
  window.scrollTo(0, y);
}

/* ---- Firmware (spec §10.5) ---- */

function waitForRestart(text) {
  document.getElementById('nav').hidden = true;
  document.getElementById('done').hidden = true;
  const note = h('p', { class: 'muted', text: 'Waiting for the device…' });
  main.replaceChildren(card(text, h('p', { text: 'The device restarts and turns Wi-Fi on again. If this phone ' +
    'doesn\'t rejoin its network by itself, join it again.' }), note));
  (async () => {
    await sleep(8000);
    for (const until = Date.now() + 120000; Date.now() < until; await sleep(2000)) {
      try {
        await api('GET', '/api/auth');
        location.reload();
        return;
      } catch (e) { /* not back yet */ }
    }
    note.textContent = 'No answer yet. Once the device shows its Wi-Fi screen, reload this page.';
  })();
}

function upload(file, progress) {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open('POST', '/api/ota');
    x.setRequestHeader('Content-Type', 'application/octet-stream');
    x.upload.onprogress = (e) => e.lengthComputable && (progress.value = e.loaded / e.total);
    x.onload = () => {
      let d = {};
      try { d = JSON.parse(x.responseText); } catch (e) { /* not JSON */ }
      if (x.status === 200) resolve(d);
      else reject(new ApiError(sentence(d.error || x.statusText), x.status));
    };
    x.onerror = () => reject(new ApiError('The upload broke off.', 0));
    x.send(file);
  });
}

async function firmwarePage() {
  const o = await api('GET', '/api/ota/status');
  const file = h('input', { type: 'file', accept: '.bin,application/octet-stream' });
  const progress = h('progress', { max: 1, value: 0, hidden: true });
  const note = h('p');
  const upCard = card('Update', h('p', { class: 'muted small', text: 'Pick reflbo.bin from a build. The device ' +
    'checks that it is reflbo firmware for this chip, writes it next to the running one and restarts into it. ' +
    'If the new firmware doesn\'t run properly for a minute, the device goes back to this one.' }),
  file, progress, note, actions(button('Update', () => busy(upCard, note, async () => {
    const f = file.files[0];
    if (!f) throw new ApiError('Pick a file first.');
    const head = new Uint8Array(await f.slice(0, 1).arrayBuffer());
    if (f.size < 65536 || head[0] !== 0xe9) throw new ApiError('That is not an ESP32 firmware image.');
    progress.hidden = false;
    const r = await upload(f, progress);
    waitForRestart(`Updated to ${r.version}`);
  }), 'primary')));
  main.replaceChildren(h('h1', { text: 'Firmware' }),
    card('Running', facts([
      ['Version', o.version], ['Built', o.built], ['Slot', o.partition],
      o.pending ? ['State', 'New: it proves itself during its first minute'] : null,
    ]), o.rolled_back_from ? h('p', { class: 'bad', text: `An update in ${o.rolled_back_from} didn't work, so the ` +
      'device went back to this version.' }) : null),
    upCard);
}

/* ---- Backup (spec §14.4) ---- */

async function backupPage() {
  const note = h('p');
  const file = h('input', { type: 'file', accept: '.json,application/json' });
  const saveCard = card('Back up', h('p', { class: 'muted small', text: 'Settings and presets in one file. Wi-Fi ' +
    'passwords and the web password are not in it.' }), actions(button('Download backup', async () => {
    const backup = await api('GET', '/api/backup'); /* parsed, as it is JSON: a file again */
    download(new Blob([JSON.stringify(backup, null, 2)], { type: 'application/json' }),
      `${auth.host}-${new Date().toISOString().slice(0, 10)}.json`);
  }, 'primary')));
  const restoreCard = card('Restore', file, note, actions(button('Restore', () => busy(restoreCard, note, async () => {
    const f = file.files[0];
    if (!f) throw new ApiError('Pick a backup file first.');
    let bundle;
    try { bundle = JSON.parse(await f.text()); } catch (e) { throw new ApiError('That file is not JSON.'); }
    const r = await api('POST', '/api/restore', bundle);
    note.className = 'good';
    note.textContent = 'Restored.' + (r.skipped.length ? ` Left out, for a later firmware: ${r.skipped.join(', ')}.` : '') +
      (r.notes || []).map((n) => ` ${n[0].toUpperCase()}${n.slice(1)}.`).join('');
  }))));
  const resetCard = card('Factory reset', h('p', { class: 'muted small', text: 'Erases the settings, presets, saved ' +
    'Wi-Fi networks and the web password, then restarts. The device then starts like new.' }),
  actions(button('Factory reset…', async () => {
    if (!confirm('Erase everything on the device and restart it?')) return;
    await api('POST', '/api/factory-reset');
    document.getElementById('nav').hidden = true;
    document.getElementById('done').hidden = true;
    main.replaceChildren(card('Erased', h('p', { text: 'The device restarts with its first-run screen.' })));
  }, 'danger')));
  main.replaceChildren(h('h1', { text: 'Backup' }), saveCard, restoreCard, resetCard);
}

start();
