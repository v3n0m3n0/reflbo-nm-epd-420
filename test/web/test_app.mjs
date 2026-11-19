// web/app.js against a fake device (spec §10.3, §10.4). Node's own test runner, no packages:
// `node --test test/web/test_app.mjs`; ctest runs it too (test/host/CMakeLists.txt).
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const APP_JS = fileURLToPath(new URL('../../web/app.js', import.meta.url));
const INDEX_HTML = fileURLToPath(new URL('../../web/index.html', import.meta.url));

/* ---- a DOM just big enough for app.js ---- */

class FakeNode {}

class FakeElement extends FakeNode {
  constructor(tag) {
    super();
    Object.assign(this, { tag, children: [], attrs: {}, listeners: {}, className: '', textContent: '', value: '',
                          hidden: false, disabled: false, checked: false, style: {} });
    this.classList = { toggle() {}, add() {}, remove() {} };
  }
  append(...kids) { this.children.push(...kids); }
  replaceChildren(...kids) { this.children = kids; }
  remove() {}
  focus() {}
  setAttribute(k, v) { this.attrs[k] = v; }
  getAttribute(k) { return this.attrs[k]; }
  get href() { return this.attrs.href; }
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  click() { return Promise.all((this.listeners.click || []).map((fn) => fn({ preventDefault() {} }))); }
  querySelectorAll(selector) { return below(this).filter((e) => e.tag === selector); }
}

/* `el` and every element under it. */
function below(el) {
  return [el, ...el.children.filter((k) => k instanceof FakeElement).flatMap(below)];
}

/* The text of an element and everything under it. */
function text(el) {
  return typeof el === 'string' ? el : el.textContent + el.children.map(text).join('');
}

function buttonNamed(root, name) {
  const b = below(root).find((e) => e.tag === 'button' && text(e) === name);
  assert.ok(b, `no button "${name}"`);
  return b;
}

/* ---- the device ---- */

function reply(status, body, type = 'application/json') {
  return {
    status, ok: status >= 200 && status < 300, statusText: String(status),
    headers: { get: (k) => (k.toLowerCase() === 'content-type' ? type : null) },
    json: async () => body,
    blob: async () => new Blob([typeof body === 'string' ? body : JSON.stringify(body)], { type }),
  };
}

/* The server's own rule (components/webui/webui.c): a request that changes something, other than a
 * firmware upload, must say it is JSON, which a cross-site form cannot. */
function device(routes) {
  return async (path, init = {}) => {
    const method = init.method || 'GET';
    const type = (init.headers || {})['Content-Type'] || '';
    if (method !== 'GET' && path !== '/api/ota' && !type.startsWith('application/json')) {
      return reply(415, { error: 'send application/json' });
    }
    const route = routes[`${method} ${path.split('?')[0]}`];
    return route ? route(init) : reply(404, { error: 'no such API' });
  };
}

/* Loads app.js with the device answering `routes`. */
async function load(routes = {}, opts = {}) {
  const calls = [], blobs = [], byId = {};
  const document = {
    getElementById: (id) => (byId[id] ||= new FakeElement('div')),
    createElement: (tag) => new FakeElement(tag),
    querySelectorAll: () => [],
    body: new FakeElement('body'),
  };
  const url = {
    createObjectURL(obj) {
      if (!(obj instanceof Blob)) throw new TypeError('createObjectURL needs a Blob'); /* as browsers do */
      blobs.push(obj);
      return `blob:${blobs.length}`;
    },
    revokeObjectURL() {},
  };
  const answer = device({
    'GET /api/auth': () => reply(200, { password_set: true, logged_in: false, on_ap: true, host: 'reflbo-bb94',
                                        ap_ssid: 'reflbo-bb94', version: '0.4.0' }),
    ...routes,
  });
  const ctx = vm.createContext({
    document, Node: FakeNode, Blob, TextEncoder, Intl, URL: url, URLSearchParams, console,
    fetch: (path, init) => { calls.push({ path, init }); return answer(path, init); },
    setTimeout: () => 0, clearTimeout() {}, confirm: opts.confirm || (() => true),
    location: { hash: '', reload() {} }, window: { scrollTo() {}, scrollY: 0 }, ZONES: {},
  });
  vm.runInContext(fs.readFileSync(APP_JS, 'utf8'), ctx, { filename: 'app.js' });
  await settle();
  return { ctx, calls, blobs, main: byId.main, byId };
}

/* Lets the fake device's answers run through their promise chains. */
async function settle() {
  for (let i = 0; i < 10; i++) await new Promise((r) => setImmediate(r));
}

/* ---- the tests ---- */

test('requests without a body still say they are JSON', async () => {
  const ok = () => reply(200, { ok: true });
  const { ctx, calls } = await load({
    'POST /api/done': ok, 'POST /api/auth/logout': ok, 'POST /api/reboot': ok, 'POST /api/factory-reset': ok,
    'DELETE /api/wifi/networks': ok,
  });
  for (const [method, path] of [['POST', '/api/done'], ['POST', '/api/auth/logout'], ['POST', '/api/reboot'],
                                ['POST', '/api/factory-reset'], ['DELETE', '/api/wifi/networks?ssid=Home']]) {
    await ctx.api(method, path);
    assert.equal(calls.at(-1).init.headers['Content-Type'], 'application/json', `${method} ${path}`);
  }
});

test('Done turns Wi-Fi off', async () => {
  const { byId } = await load({ 'POST /api/done': () => reply(200, { ok: true }) });
  await byId.done.onclick();
  assert.match(text(byId.main), /Wi-Fi is off/);
});

test('Download backup saves the backup as a JSON file', async () => {
  const backup = { reflbo_backup: 1, device: 'reflbo-bb94', firmware: '0.4.0',
                   files: { 'settings.json': { schema: 1, language: 'cs' } } };
  const { ctx, blobs, main } = await load({ 'GET /api/backup': () => reply(200, backup) });
  await ctx.backupPage();
  await buttonNamed(main, 'Download backup').click();
  assert.equal(blobs.length, 1);
  assert.deepEqual(JSON.parse(await blobs[0].text()), backup);
});

test('a restore says what waits for a key', async () => {
  const note = 'a second plane needs a Forecast.Solar key: the forecast uses the first plane until one is set';
  const { ctx, calls, main } = await load({
    'POST /api/restore': () => reply(200, { ok: true, skipped: [], notes: [note] }),
  });
  await ctx.backupPage();
  const file = below(main).find((e) => e.tag === 'input');
  file.files = [{ text: async () => JSON.stringify({ reflbo_backup: 1, files: { 'settings.json': { schema: 1 } } }) }];
  await buttonNamed(main, 'Restore').click();
  await settle();
  assert.ok(calls.some((c) => c.path === '/api/restore' && c.init.method === 'POST'));
  assert.match(text(main), /Restored\. A second plane needs a Forecast\.Solar key: the forecast uses the first plane until one is set\./);
});

test('Factory reset erases and says so', async () => {
  const { ctx, calls, main } = await load({ 'POST /api/factory-reset': () => reply(200, { ok: true }) });
  await ctx.backupPage();
  await buttonNamed(main, 'Factory reset…').click();
  assert.equal(calls.at(-1).path, '/api/factory-reset');
  assert.match(text(main), /Erased/);
});

/* ---- the preset editor ---- */

const CATALOGUE = {
  width: 400, height: 300, status_h: 20,
  layouts: [{ id: 'classic', slots: [
    { id: 'main', x: 0, y: 21, w: 400, h: 150, size: 'XL', kinds: ['time'] },
    { id: 's1', x: 0, y: 172, w: 200, h: 128, size: 'S', kinds: ['number'] },
  ] }],
};
const FIELDS = { fields: [{ id: 'time.clock', kind: 'time', name: 'Time', value: '12:15' },
                          { id: 'env.temp', kind: 'number', name: 'Temperature', value: '23.7 °C' }] };
const preset = (id, name, inCycle) => ({ id, name, layout: 'classic', in_cycle: inCycle,
                                         slots: { main: 'time.clock', s1: 'env.temp' }, options: {} });

/* A device with presets: Home in the cycle, Weather out of it; PUTs are kept in `saved`; `edit` may change the doc. */
function presetDevice(saved, edit = () => {}) {
  const doc = { schema: 1, active: 'weather', presets: [preset('home', 'Home', true), preset('weather', 'Weather', false)],
                cycle: { enabled: false, interval_s: 60 }, schedule: { enabled: false, entries: [] } };
  edit(doc);
  return {
    'GET /api/layouts': () => reply(200, CATALOGUE),
    'GET /api/presets': () => reply(200, JSON.parse(JSON.stringify(doc))),
    'GET /api/fields': () => reply(200, FIELDS),
    'POST /api/preview.bmp': () => reply(200, 'BM', 'image/bmp'),
    'PUT /api/presets': (init) => { saved.push(JSON.parse(init.body)); return reply(200, JSON.parse(init.body)); },
  };
}

test('a new preset joins the cycle, even as a copy of one outside it', async () => {
  const saved = [];
  const { ctx, main } = await load(presetDevice(saved));
  await ctx.presetsPage(); /* Weather, the active one, is selected */
  await buttonNamed(main, 'New preset').click();
  await buttonNamed(main, 'Save').click();
  await settle();
  const added = saved.at(-1).presets.find((p) => p.name.startsWith('Weather copy'));
  assert.ok(added, 'the copy was saved');
  assert.equal(added.in_cycle, true);
});

test('Undo changes asks before it drops the edits', async () => {
  const saved = [];
  let asked = 0;
  const { ctx, calls, main } = await load(presetDevice(saved), { confirm: () => { asked++; return false; } });
  await ctx.presetsPage();
  await buttonNamed(main, 'New preset').click();
  const before = calls.filter((c) => c.path === '/api/presets').length;
  await buttonNamed(main, 'Undo changes').click();
  await settle();
  assert.equal(asked, 1);
  assert.equal(calls.filter((c) => c.path === '/api/presets').length, before, 'nothing reloaded');
  assert.match(text(main), /Weather copy/);
});

test('the preview names each slot where the layout puts it', async () => {
  const { ctx, main } = await load(presetDevice([]));
  await ctx.presetsPage();
  const tags = below(main).filter((e) => e.className.split(' ').includes('slot-tag'));
  assert.deepEqual(tags.map(text), ['main', 's1']);
  assert.deepEqual(tags.map((e) => [e.style.left, e.style.top]), [['100%', '7%'], ['50%', '57.333%']]); /* top right */
});

/* ---- a preset's cycle window (spec §5.4, §10.3, D41) ---- */

test('a preset gets a cycle window, sunrise to sunset by default, and the list shows it (D41)', async () => {
  const saved = [];
  const { ctx, main } = await load(presetDevice(saved));
  await ctx.presetsPage(); /* Weather, the active one, is selected */
  assert.equal(control(main, 'In the cycle').value, 'all');
  await type(control(main, 'In the cycle'), 'window');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.deepEqual(saved.at(-1).presets[1].window, { from: 'sunrise', until: 'sunset', days: 127 });
  assert.equal(saved.at(-1).presets[0].window, undefined);
  assert.match(text(main), /sunrise – sunset/);
  assert.match(text(main), /skips this preset while its window is closed; KEY still reaches it/);
});

test('a window\'s ends take a time, or sunrise or sunset with an offset, and its days (D41)', async () => {
  const saved = [];
  const { ctx, main } = await load(presetDevice(saved));
  await ctx.presetsPage();
  await type(control(main, 'In the cycle'), 'window');
  await type(control(main, 'From').children[0], 'time');
  await type(control(main, 'From').children[1], '22:00');
  await type(control(main, 'Until').children[1], '-30'); /* sunset, 30 min before */
  await type(control(main, 'Until').children[0], 'sunrise'); /* the offset stays */
  const days = below(control(main, 'Days')).filter((e) => e.tag === 'input');
  for (const i of [5, 6]) { days[i].checked = false; await Promise.all(days[i].listeners.change.map((fn) => fn({ target: days[i] }))); }
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.deepEqual(saved.at(-1).presets[1].window, { from: '22:00', until: 'sunrise-30', days: 31 });
  assert.match(text(main), /22:00 – sunrise-30, Mo Tu We Th Fr/);
});

test('the page refuses a window the device would refuse (D41)', async () => {
  const saved = [];
  const { ctx, main } = await load(presetDevice(saved, (doc) => {
    doc.presets[1].window = { from: 'sunset', until: 'sunset+200', days: 127 };
  }));
  await ctx.presetsPage();
  await type(control(main, 'Until').children[1], '200'); /* the page changed nothing yet: a save is due */
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.match(text(main), /Weather: a window's ends are times, or sunrise or sunset within 180 minutes\./);
  await type(control(main, 'Until').children[1], '0');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.match(text(main), /Weather: the window's from and until can't be the same\./);
  await type(control(main, 'Until').children[1], '15');
  const days = below(control(main, 'Days')).filter((e) => e.tag === 'input');
  for (const d of days) { d.checked = false; await Promise.all(d.listeners.change.map((fn) => fn({ target: d }))); }
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.match(text(main), /Weather: a window opens on one day at least\./);
  assert.equal(saved.length, 0);
});

test('All day removes the window (D41)', async () => {
  const saved = [];
  const { ctx, main } = await load(presetDevice(saved, (doc) => {
    doc.presets[1].window = { from: 'sunrise', until: 'sunset', days: 127 };
  }));
  await ctx.presetsPage();
  assert.equal(control(main, 'In the cycle').value, 'window');
  await type(control(main, 'In the cycle'), 'all');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(saved.at(-1).presets[1].window, undefined);
});

/* ---- the Device page's battery calibration (owner, 2026-09-30) ---- */

/* The control a field() label introduces: the element after it. */
function control(root, label) {
  for (const el of below(root)) {
    const i = el.children.findIndex((k) => k instanceof FakeElement && k.tag === 'label' && text(k) === label);
    if (i >= 0) return el.children[i + 1];
  }
  assert.fail(`no field "${label}"`);
}

function settingsDevice(patches, battery = {}, learn = { state: 'off', hours: 0 }, learnCalls = []) {
  const settings = { schema: 1, language: 'en', units: { temp: 'C' },
                     sensors: { interval_min: 5, temp_offset_c: -2, hum_offset_pct: 0 },
                     display: { update_min: 1, lpm_hz: 1 },
                     battery: { level_from: 'curve', empty_v: 3.27, full_v: 4.2, ...battery } };
  return {
    'GET /api/settings': () => reply(200, settings),
    'GET /api/status': () => reply(200, { battery: { percent: 82, mv: 4050, learn } }),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, settings); },
    'POST /api/battery/learn': (init) => { learnCalls.push(JSON.parse(init.body)); return reply(200, { state: 'waiting' }); },
  };
}

const options = (sel) => sel.children.filter((k) => k instanceof FakeElement).map((o) => o.value);

test('the Device page saves your own battery voltages', async () => {
  const patches = [];
  const { ctx, main } = await load(settingsDevice(patches));
  await ctx.devicePage();
  control(main, 'Battery level from').value = 'manual';
  control(main, 'Full, V').value = '4.12';
  control(main, 'Empty, V').value = '3.45';
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.deepEqual(patches.at(-1).battery, { level_from: 'manual', empty_v: 3.45, full_v: 4.12 });
});

test('the Device page wants room between the battery voltages', async () => {
  const patches = [];
  const { ctx, main } = await load(settingsDevice(patches));
  await ctx.devicePage();
  control(main, 'Battery level from').value = 'manual';
  control(main, 'Full, V').value = '4.0';
  control(main, 'Empty, V').value = '3.9';
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 0);
  assert.match(text(main), /0\.3 V/);
});

/* ---- learning the battery curve from a full discharge (D21) ---- */

test('the learned curve is on offer once there is one', async () => {
  let page = await load(settingsDevice([]));
  await page.ctx.devicePage();
  assert.deepEqual(options(control(page.main, 'Battery level from')), ['curve', 'manual']);
  const curve = Array.from({ length: 21 }, (_, i) => 3300 + 45 * i);
  page = await load(settingsDevice([], { level_from: 'learned', learned_mv: curve, learned_at: 1790786009 }));
  await page.ctx.devicePage();
  assert.deepEqual(options(control(page.main, 'Battery level from')), ['curve', 'manual', 'learned']);
});

test('Learn from the next full discharge arms it', async () => {
  const learnCalls = [];
  const { ctx, main } = await load(settingsDevice([], {}, { state: 'off', hours: 0 }, learnCalls));
  await ctx.devicePage();
  await buttonNamed(main, 'Learn from the next full discharge').click();
  await settle();
  assert.deepEqual(learnCalls, [{ start: true }]);
});

test('a discharge being learned shows its hours and can be stopped', async () => {
  const learnCalls = [];
  const { ctx, main } = await load(settingsDevice([], {}, { state: 'recording', hours: 42 }, learnCalls));
  await ctx.devicePage();
  assert.match(text(main), /42 h/);
  await buttonNamed(main, 'Stop learning').click();
  await settle();
  assert.deepEqual(learnCalls, [{ stop: true }]);
});

/* ---- sync (spec §9.3, D25) ---- */

const SYNC_SETTINGS = { schema: 1, sync: { mode: 'times', times: ['05:30'], interval_min: 60,
                                           quiet: { enabled: false, from: '23:00', to: '06:00' } } };
const syncStatus = (sync) => ({ device: {}, time: { valid: true, rtc: { trim_steps: -9, drift_s_per_day: -0.2 } },
                                battery: {}, sensors: {}, preset: {}, sync,
                                wifi: { state: sync.mode === 'always' ? 'station' : 'off', ap_on: false } });

test('the Sync page saves the mode, the times and the quiet hours', async () => {
  const patches = [];
  const { ctx, main } = await load({
    'GET /api/settings': () => reply(200, SYNC_SETTINGS),
    'GET /api/status': () => reply(200, syncStatus({ mode: 'times', running: false })),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, SYNC_SETTINGS); },
  });
  await ctx.syncPage();
  const inputs = below(main).filter((e) => e.tag === 'input');
  const time = inputs.find((e) => e.attrs.type === 'time' && e.value === '05:30');
  time.value = '04:45';
  await Promise.all((time.listeners.change || []).map((fn) => fn({ target: time })));
  inputs.find((e) => e.attrs.type === 'checkbox').checked = true; /* quiet hours 23:00-06:00 */
  await buttonNamed(main, 'Save').click();
  assert.deepEqual(patches.at(-1), { sync: { mode: 'times', times: ['04:45'], interval_min: 60,
                                             quiet: { enabled: true, from: '23:00', to: '06:00' } } });
  assert.match(text(main), /04:45 falls in the quiet hours: it runs at 06:00/);
});

test('the Balanced shortcut syncs every hour', async () => {
  const patches = [];
  const { ctx, main } = await load({
    'GET /api/settings': () => reply(200, SYNC_SETTINGS),
    'GET /api/status': () => reply(200, syncStatus({ mode: 'times', running: false })),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, SYNC_SETTINGS); },
  });
  await ctx.syncPage();
  await buttonNamed(main, 'Balanced').click();
  await buttonNamed(main, 'Save').click();
  assert.equal(patches.at(-1).sync.mode, 'interval');
  assert.equal(patches.at(-1).sync.interval_min, 60);
});

test('Sync now follows the sync to its end', async () => {
  let polls = 0;
  const done = { mode: 'times', running: false, last: { at: 1790859600, failed: 'weather', detail: 'HTTP 503',
                 steps: { wifi: 'ok', time: 'ok', weather: 'failed', air: 'ok' } } };
  const { ctx, calls, main } = await load({
    'GET /api/settings': () => reply(200, SYNC_SETTINGS),
    'GET /api/status': () => reply(200, syncStatus(++polls < 3 ? { mode: 'times', running: true, step: 'weather' } : done)),
    'POST /api/sync': () => reply(202, { started: true }),
  });
  ctx.setTimeout = (fn) => { fn(); return 0; }; /* sleep() returns at once */
  await ctx.syncPage();
  await buttonNamed(main, 'Sync now').click();
  await settle();
  assert.ok(calls.some((c) => c.path === '/api/sync' && c.init.method === 'POST'));
  assert.match(text(main), /The sync failed/);
  assert.match(text(main), /failed: HTTP 503/);
});

test('a sync whose only failure is the house\'s energy is done, as the device counts it (D36)', async () => {
  let polls = 0;
  const done = { mode: 'times', running: false, last: { at: 1790859600, ok: true, failed: 'energy',
                 detail: 'tokenId is invalid', steps: { wifi: 'ok', time: 'ok', weather: 'ok', air: 'ok', radar: 'ok',
                                                        solar: 'ok', energy: 'failed' } } };
  const { ctx, main } = await load({
    'GET /api/settings': () => reply(200, SYNC_SETTINGS),
    'GET /api/status': () => reply(200, syncStatus(++polls < 3 ? { mode: 'times', running: true, step: 'energy' } : done)),
    'POST /api/sync': () => reply(202, { started: true }),
  });
  ctx.setTimeout = (fn) => { fn(); return 0; };
  await ctx.syncPage();
  await buttonNamed(main, 'Sync now').click();
  await settle();
  assert.doesNotMatch(text(main), /The sync failed/);
  assert.match(text(main), /Synced; the house's energy failed, see above\./);
});

test('the place search fills in the name and the coordinates', async () => {
  const { ctx, calls, main } = await load({
    'GET /api/settings': () => reply(200, { schema: 1, location: { name: 'Brno', lat: 49.1951, lon: 16.6068 }, time: {} }),
    'GET /api/geocode': () => reply(200, { places: [{ name: 'Olomouc', region: 'Olomoucký', country: 'CZ', lat: 49.5938,
                                                      lon: 17.2509, timezone: 'Europe/Prague' }] }),
  });
  await ctx.placePage();
  const query = below(main).find((e) => e.tag === 'input' && e.attrs.type === 'search' && e.attrs.placeholder === 'A town or city');
  query.value = 'Olomouc';
  await buttonNamed(main, 'Search').click();
  assert.equal(calls.at(-1).path, '/api/geocode?q=Olomouc');
  await buttonNamed(main, 'Olomouc, Olomoucký, CZ').click();
  const values = below(main).filter((e) => e.tag === 'input').map((e) => e.value);
  assert.ok(values.includes('Olomouc') && values.includes('49.5938') && values.includes('17.2509'), values.join(' '));
});

test('Done keeps the page when sync mode Always on keeps the network', async () => {
  const { byId } = await load({
    'GET /api/status': () => reply(200, syncStatus({ mode: 'always', running: false })),
    'POST /api/done': () => reply(200, { ok: true }),
  });
  await byId.done.onclick();
  assert.doesNotMatch(text(byId.main), /Wi-Fi is off/);
});

/* ---- the Radar page (spec §10.3, M6) ---- */

const RADAR_SETTINGS = { schema: 1, location: { name: 'Brno', lat: 49.1951, lon: 16.6068 }, sync: { mode: 'times' },
                         radar: { weather: { zoom: 6.5 }, flights: { range_km: 50, min_alt_ft: 0, ground: false, max: 100 } } };
const radarStatus = (radar) => ({ device: {}, time: { valid: true }, battery: {}, sensors: {}, preset: {}, sync: {}, radar });

function radarDevice(settings, patches, previews) {
  return {
    'GET /api/settings': () => reply(200, settings),
    'GET /api/status': () => reply(200, radarStatus({ weather: { source: 'dwd', frames: 1, frame_at: 1790880000 },
                                                      flights: { on: false, aircraft: 0, failed: false } })),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, settings); },
    'POST /api/preview.bmp': (init) => { previews.push(JSON.parse(init.body)); return reply(200, 'BM', 'image/bmp'); },
  };
}

/* The radio buttons of a radar's card: a browser unchecks the other of a group by itself. */
function pick(main, group, label) {
  const radios = below(main).filter((e) => e.tag === 'input' && e.attrs.type === 'radio' && e.attrs.name === group);
  const labels = below(main).filter((e) => e.tag === 'label' && e.children.some((k) => radios.includes(k)));
  for (const l of labels) {
    const r = l.children.find((k) => radios.includes(k));
    r.checked = text(l).startsWith(label);
    if (r.checked) (r.listeners.change || []).forEach((fn) => fn({ target: r }));
  }
}

test('the Radar page saves the weather radar\'s own centre and zoom', async () => {
  const patches = [], previews = [];
  const { ctx, main } = await load(radarDevice(RADAR_SETTINGS, patches, previews));
  await ctx.radarPage();
  pick(main, 'wx-centre', 'A place of its own');
  const numbers = below(main).filter((e) => e.tag === 'input' && e.attrs.type === 'number');
  numbers[0].value = '50.0755'; /* the weather card's latitude and longitude come first */
  numbers[1].value = '14.4378';
  const zoom = below(main).find((e) => e.tag === 'select' && e.attrs.name === 'zoom');
  zoom.value = '7.25';
  await buttonNamed(main, 'Save weather radar').click();
  assert.deepEqual(patches.at(-1), { radar: { weather: { lat: 50.0755, lon: 14.4378, zoom: 7.25 } } });
});

test('the location again clears a radar\'s own centre', async () => {
  const patches = [], previews = [];
  const settings = { ...RADAR_SETTINGS, radar: { weather: { lat: 50.0755, lon: 14.4378, zoom: 7 }, flights: {} } };
  const { ctx, main } = await load(radarDevice(settings, patches, previews));
  await ctx.radarPage();
  pick(main, 'wx-centre', 'The location');
  await buttonNamed(main, 'Save weather radar').click();
  assert.deepEqual(patches.at(-1), { radar: { weather: { lat: null, lon: null, zoom: 7 } } }); /* RFC 7396 removes them */
});

test('the Radar page saves a weather radar source and reverts to auto with null', async () => {
  const patches = [], previews = [];
  const { ctx, main } = await load(radarDevice(RADAR_SETTINGS, patches, previews));
  await ctx.radarPage();
  const src = below(main).find((e) => e.tag === 'select' && e.attrs.name === 'source');
  assert.equal(src.value, 'auto');
  src.value = 'rainviewer';
  await buttonNamed(main, 'Save weather radar').click();
  assert.deepEqual(patches.at(-1), { radar: { weather: { lat: null, lon: null, zoom: 6.5, source: 'rainviewer' } } });

  const withSource = { ...RADAR_SETTINGS, radar: { weather: { source: 'rainviewer', zoom: 6.5 }, flights: {} } };
  const patches2 = [];
  const r2 = await load(radarDevice(withSource, patches2, []));
  await r2.ctx.radarPage();
  const src2 = below(r2.main).find((e) => e.tag === 'select' && e.attrs.name === 'source');
  assert.equal(src2.value, 'rainviewer');
  src2.value = 'auto';
  await buttonNamed(r2.main, 'Save weather radar').click();
  assert.deepEqual(patches2.at(-1), { radar: { weather: { lat: null, lon: null, zoom: 6.5, source: null } } });
});

test('the flight radar\'s filters are saved within their bounds', async () => {
  const patches = [], previews = [];
  const { ctx, main } = await load(radarDevice(RADAR_SETTINGS, patches, previews));
  await ctx.radarPage();
  const range = below(main).find((e) => e.tag === 'select' && e.attrs.name === 'range');
  range.value = '25';
  const byName = (n) => below(main).find((e) => e.tag === 'input' && e.attrs.name === n);
  byName('min_alt_ft').value = '3000';
  byName('ground').checked = true;
  byName('max').value = '0';
  await buttonNamed(main, 'Save flight radar').click();
  assert.equal(patches.length, 0);
  assert.match(text(main), /Aircraft at most: 1 to 100/);
  byName('max').value = '40';
  await buttonNamed(main, 'Save flight radar').click();
  assert.deepEqual(patches.at(-1), { radar: { flights: { lat: null, lon: null, range_km: 25, min_alt_ft: 3000,
                                                         ground: true, max: 40 } } });
});

test('the flight radar says it runs only in sync mode Always on', async () => {
  const { ctx, main } = await load(radarDevice(RADAR_SETTINGS, [], []));
  await ctx.radarPage();
  assert.match(text(main), /runs only in sync mode Always on/);
  const always = { ...RADAR_SETTINGS, sync: { mode: 'always' } };
  const again = await load(radarDevice(always, [], []));
  await again.ctx.radarPage();
  assert.doesNotMatch(text(again.main), /runs only in sync mode Always on/);
});

test('the flight radar says why its last poll failed and when routes come back', async () => {
  const device = radarDevice({ ...RADAR_SETTINGS, sync: { mode: 'always' } }, [], []);
  device['GET /api/status'] = () => reply(200, radarStatus({ weather: { source: 'dwd', frames: 1, frame_at: 1790880000 },
    flights: { on: true, aircraft: 0, failed: true, error: 'HTTP 429', routes_paused_until: 1790883600 } }));
  const { ctx, main } = await load(device);
  await ctx.radarPage();
  assert.match(text(main), /Last pollfailed: HTTP 429/);
  assert.match(text(main), /Routespaused by adsb\.lol until /);
});

test('the Radar page previews both radars and credits their sources', async () => {
  const previews = [];
  const { ctx, main } = await load(radarDevice(RADAR_SETTINGS, [], previews));
  await ctx.radarPage();
  await settle();
  assert.deepEqual(previews.map((d) => d.presets[0].layout).sort(), ['flights', 'radar']);
  const all = text(main);
  for (const credit of ['DWD', 'CC BY 4.0', 'RainViewer', 'adsb.fi', 'adsb.lol', 'GeoNames']) assert.ok(all.includes(credit), credit);
  assert.match(all, /Newest frame/);
});

test('the Sync page shows the radar\'s step', async () => {
  const last = { at: 1790880000, steps: { wifi: 'ok', time: 'ok', weather: 'ok', air: 'ok', radar: 'failed' },
                 failed: 'radar', detail: 'HTTP 503' };
  const { ctx, main } = await load({
    'GET /api/settings': () => reply(200, SYNC_SETTINGS),
    'GET /api/status': () => reply(200, syncStatus({ mode: 'times', running: false, last })),
  });
  await ctx.syncPage();
  assert.match(text(main), /Radar failed \(HTTP 503\)/);
  assert.match(text(main), /Radarfailed: HTTP 503/); /* the steps' list: its name, then its result */
});

test('a preset on a radar layout has no slots to fill', async () => {
  const catalogue = { ...CATALOGUE, layouts: [...CATALOGUE.layouts, { id: 'radar', slots: [] }, { id: 'flights', slots: [] }] };
  const doc = { schema: 1, active: 'rain', presets: [{ id: 'rain', name: 'Rain radar', layout: 'radar', in_cycle: true,
                                                         slots: {}, options: {} }],
                cycle: { enabled: false, interval_s: 60 }, schedule: { enabled: false, entries: [] } };
  const { ctx, main } = await load({
    'GET /api/layouts': () => reply(200, catalogue), 'GET /api/presets': () => reply(200, doc),
    'GET /api/fields': () => reply(200, FIELDS), 'POST /api/preview.bmp': () => reply(200, 'BM', 'image/bmp'),
  });
  await ctx.presetsPage();
  assert.match(text(main), /Rain radar Radar/); /* the list names its layout */
  assert.match(text(main), /This layout draws the weather radar/);
});

/* ---- the split layout's editor (M6b, spec §5.2, §10.3, D31) ---- */

/* The device's rules (GET /api/layouts, ui_catalog.c) for the kinds these tests use. */
const SPLIT = {
  x: 0, y: 21, w: 400, h: 279, cells: 24, min_w: 40, min_h: 20, narrow_w: 150, inset: 8, /* M6c, D34 */
  ratios: ['1/4', '1/3', '1/2', '2/3', '3/4'],
  sizes: [
    { size: 'XL', min_w: 400, min_h: [120, 120], kinds: { time: [120, 120], number: [120, 120] } },
    { size: 'L', min_w: 200, min_h: [150, 150], kinds: { time: [150, 150], number: [150, 150] } },
    { size: 'M', min_w: 130, min_h: [80, 80], kinds: { time: [80, 80], number: [80, 80], series: [80, 80] } },
    { size: 'S', min_w: 90, min_h: [40, 40], kinds: { time: [40, 40], number: [40, 40] } },
    { size: 'XS', min_w: 40, min_h: [20, 20], kinds: { time: [20, 20], number: [20, 20] } },
  ],
};
const SPLIT_CATALOGUE = { ...CATALOGUE, layouts: [...CATALOGUE.layouts, { id: 'split', slots: [] }], split: SPLIT };
const SPLIT_FIELDS = { fields: [...FIELDS.fields, { id: 'wx.hourly', kind: 'series', label: 'Next hours', value: '' }] };

/* A device with one preset, Home: Classic, or on the split layout with `tree`. */
function splitDevice(saved, tree) {
  const home = tree ? { id: 'home', name: 'Home', layout: 'split', in_cycle: true, split: tree, options: {} }
                    : preset('home', 'Home', true);
  const doc = { schema: 1, active: 'home', presets: [home], cycle: { enabled: false, interval_s: 60 },
                schedule: { enabled: false, entries: [] } };
  return {
    'GET /api/layouts': () => reply(200, SPLIT_CATALOGUE),
    'GET /api/presets': () => reply(200, JSON.parse(JSON.stringify(doc))),
    'GET /api/fields': () => reply(200, SPLIT_FIELDS),
    'POST /api/preview.bmp': () => reply(200, 'BM', 'image/bmp'),
    'PUT /api/presets': (init) => { saved.push(JSON.parse(init.body)); return reply(200, JSON.parse(init.body)); },
  };
}

async function change(el, value) {
  if (el.attrs.type === 'checkbox') el.checked = value;
  else el.value = value;
  await Promise.all((el.listeners.change || []).map((fn) => fn({ target: el })));
}

const ratioSelects = (root) => below(root).filter((e) => e.tag === 'select' && options(e).includes('1/4'));
const cellLabels = (root) => below(root).filter((e) => e.className === 'cell-name').map(text);

async function savedTree(main, saved) {
  await buttonNamed(main, 'Save').click();
  await settle();
  return saved.at(-1).presets[0];
}

test('switching a preset to Split starts from one cell holding its first field', async () => {
  const saved = [];
  const { ctx, main } = await load(splitDevice(saved));
  await ctx.presetsPage();
  await change(control(main, 'Layout'), 'split');
  assert.deepEqual(cellLabels(main), ['1 · 400×279 · XL']);
  const p = await savedTree(main, saved);
  assert.deepEqual(p.split, { field: 'time.clock' });
  assert.equal(p.slots, undefined, 'a split preset has its tree instead of slots');
});

test('Split into rows halves a cell, its field going to the first part', async () => {
  const saved = [];
  const { ctx, main } = await load(splitDevice(saved, { field: 'time.clock' }));
  await ctx.presetsPage();
  await buttonNamed(main, 'Split into rows').click();
  assert.deepEqual(cellLabels(main), ['1 · 400×139 · XL', '2 · 400×139 · XL']); /* XL from 120 px */
  const tags = below(main).filter((e) => e.className.split(' ').includes('slot-tag'));
  assert.deepEqual(tags.map((e) => [text(e), e.style.left, e.style.top]),
                   [['1', '100%', '7%'], ['2', '100%', '53.667%']]); /* each cell's number at its top right */
  const p = await savedTree(main, saved);
  assert.deepEqual(p.split, { split: 'rows', ratio: '1/2', line: true, a: { field: 'time.clock' }, b: {} });
});

test('a split offers only the ratios that leave every part 40×20', async () => {
  const { ctx, main } = await load(splitDevice([], { split: 'rows', ratio: '1/4', line: true,
                                                     a: { split: 'rows', ratio: '1/2', line: true, a: {}, b: {} },
                                                     b: {} }));
  await ctx.presetsPage();
  const [outer, inner] = ratioSelects(main);
  const disabled = (sel) => sel.children.filter((o) => o.disabled).map((o) => o.value);
  assert.deepEqual(disabled(outer), []);             /* a quarter, 69 px, halves into 34 */
  assert.deepEqual(disabled(inner), ['1/4', '3/4']); /* 69 px: 17 at a quarter */
  const splitButtons = below(main).filter((e) => e.tag === 'button' && /^Split into/.test(text(e)));
  assert.deepEqual(splitButtons.map((b) => b.disabled), [true, false, true, false, false, false]); /* 400×34 */
});

/* Eight columns of halves, a row of cells 50 or 49 px wide (M6c). */
const eightColumns = () => {
  const half = (a, b) => ({ split: 'columns', ratio: '1/2', line: true, a, b });
  const four = () => half(half({}, {}), half({}, {}));
  return half(four(), four());
};

test('a cell under 90 px wide is XS', async () => {
  const { ctx, main } = await load(splitDevice([], { split: 'columns', ratio: '1/4', line: true,
    a: { split: 'columns', ratio: '1/2', line: true, a: { field: 'env.temp' }, b: {} }, b: {} }));
  await ctx.presetsPage();
  assert.deepEqual(cellLabels(main), ['1 · 50×279 · XS', '2 · 49×279 · XS', '3 · 299×279 · L']);
});

test('a tree of 24 cells splits no further', async () => {
  const rows = { split: 'rows', ratio: '1/3', line: true, a: eightColumns(),
                 b: { split: 'rows', ratio: '1/2', line: true, a: eightColumns(), b: eightColumns() } };
  const { ctx, main } = await load(splitDevice([], rows));
  await ctx.presetsPage();
  assert.equal(cellLabels(main).length, 24);
  assert.equal(cellLabels(main)[23], '24 · 49×92 · XS');
  const splitButtons = below(main).filter((e) => e.tag === 'button' && /^Split into/.test(text(e)));
  assert.equal(splitButtons.length, 48);
  assert.ok(splitButtons.every((b) => b.disabled));
});

test('a cell offers only the fields that fit it', async () => {
  const { ctx, main } = await load(splitDevice([], { split: 'rows', ratio: '3/4', line: true, a: {}, b: {} }));
  await ctx.presetsPage();
  const fieldSelects = below(main).filter((e) => e.tag === 'select' && options(e).includes(''));
  assert.deepEqual(options(fieldSelects[0]), ['', 'time.clock', 'env.temp', 'wx.hourly']); /* 400×209 */
  assert.deepEqual(options(fieldSelects[1]), ['', 'time.clock', 'env.temp']); /* 400×69: no series */
});

test('Join keeps the first field found inside the split', async () => {
  const saved = [];
  const { ctx, main } = await load(splitDevice(saved, { split: 'columns', ratio: '1/2', line: true, a: {},
    b: { split: 'rows', ratio: '1/2', line: true, a: { field: 'env.temp' }, b: { field: 'time.clock' } } }));
  await ctx.presetsPage();
  await buttonNamed(main, 'Join').click(); /* the outer split's, the first */
  const p = await savedTree(main, saved);
  assert.deepEqual(p.split, { field: 'env.temp' });
});

test('each split keeps its own separator', async () => {
  const saved = [];
  const { ctx, main } = await load(splitDevice(saved, { split: 'rows', ratio: '1/2', line: true, a: {},
                                                        b: { split: 'columns', ratio: '1/2', line: true, a: {}, b: {} } }));
  await ctx.presetsPage();
  const boxes = below(main).filter((e) => e.tag === 'label' && text(e) === 'Separator').map((l) => l.children[0]);
  assert.equal(boxes.length, 2);
  await change(boxes[1], false);
  const p = await savedTree(main, saved);
  assert.equal(p.split.line, true);
  assert.equal(p.split.b.line, false);
});

test('a ratio that leaves a field without room empties its cell, and says so', async () => {
  const saved = [];
  const { ctx, main, byId } = await load(splitDevice(saved, { split: 'rows', ratio: '1/2', line: true, a: {},
                                                              b: { field: 'wx.hourly' } }));
  await ctx.presetsPage();
  await change(ratioSelects(main)[0], '3/4'); /* the bottom cell: 400×69, too low for a series */
  assert.equal(text(byId.toast), 'No room for Next hours');
  const p = await savedTree(main, saved);
  assert.deepEqual(p.split.b, {});
});

test('a split that leaves no room for the cell\'s field says so', async () => {
  const saved = [];
  const { ctx, main, byId } = await load(splitDevice(saved, { split: 'rows', ratio: '1/2', line: true,
                                                              a: { field: 'wx.hourly' }, b: {} }));
  await ctx.presetsPage();
  await buttonNamed(main, 'Split into rows').click(); /* the first cell's: 400×69 halves, too low for a series */
  assert.equal(text(byId.toast), 'No room for Next hours');
  const p = await savedTree(main, saved);
  assert.deepEqual(p.split.a, { split: 'rows', ratio: '1/2', line: true, a: {}, b: {} });
});

test('a cell says when its field draws at a smaller size than the cell\'s', async () => {
  const { ctx, main } = await load(splitDevice([], { split: 'rows', ratio: '3/4', line: true,
                                                     a: { field: 'wx.hourly' }, b: { field: 'env.temp' } }));
  await ctx.presetsPage();
  assert.deepEqual(cellLabels(main), ['1 · 400×209 · XL (Next hours at M)', '2 · 400×69 · S']);
});

/* ---- M6d: the steps' switches, the Solar page, the editor's Solar and Energy fields ---- */

const STEP_SETTINGS = { ...SYNC_SETTINGS, sync: { ...SYNC_SETTINGS.sync,
                                                  steps: ['weather', 'air', 'radar', 'solar', 'energy'] } };

test('the Sync page switches the data steps on and off, the time always on', async () => {
  const patches = [];
  const { ctx, main } = await load({
    'GET /api/settings': () => reply(200, STEP_SETTINGS),
    'GET /api/status': () => reply(200, syncStatus({ mode: 'times', running: false })),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, STEP_SETTINGS); },
  });
  await ctx.syncPage();
  const box = (name) => below(main).find((e) => e.tag === 'label' && text(e) === name).children[0];
  assert.equal(box('Radar').checked, true);
  box('Radar').checked = false;
  box('House energy').checked = false;
  await buttonNamed(main, 'Save steps').click();
  assert.deepEqual(patches.at(-1), { sync: { steps: ['weather', 'air', 'solar'] } });
  assert.match(text(main), /The time always runs/);
});

test('the Sync page shows the Solar and Energy steps, a kept one as kept', async () => {
  const last = { at: 1790880000, failed: 'energy', detail: 'tokenId is invalid',
                 steps: { wifi: 'ok', time: 'ok', weather: 'ok', air: 'ok', radar: 'ok', solar: 'kept', energy: 'failed' } };
  const { ctx, main } = await load({
    'GET /api/settings': () => reply(200, STEP_SETTINGS),
    'GET /api/status': () => reply(200, syncStatus({ mode: 'times', running: false, last })),
  });
  await ctx.syncPage();
  assert.match(text(main), /Solar forecastkept/);
  assert.match(text(main), /House energyfailed: tokenId is invalid/);
});

test('the Sync page says why a step was kept or skipped', async () => {
  const last = { at: 1790880000, steps: { wifi: 'ok', time: 'ok', weather: 'skipped', air: 'ok', radar: 'ok',
                                          solar: 'kept', energy: 'skipped' },
                 details: { weather: 'off', solar: 'HTTP 429', energy: 'no source' } };
  const { ctx, main } = await load({
    'GET /api/settings': () => reply(200, STEP_SETTINGS),
    'GET /api/status': () => reply(200, syncStatus({ mode: 'times', running: false, last })),
  });
  await ctx.syncPage();
  assert.match(text(main), /Weatherskipped: off/);
  assert.match(text(main), /Solar forecastkept: HTTP 429/);
  assert.match(text(main), /House energyskipped: no source/);
});

const SOLAR_SETTINGS = { schema: 1, location: { name: 'Brno', lat: 49.1951, lon: 16.6068 },
                         sync: { steps: ['weather', 'air', 'radar', 'solar', 'energy'] },
                         solar: { source: 'open-meteo', planes: [{ kwp: 5, tilt: 35, azimuth: 0 }], losses_pct: 14,
                                  inverter_kw: 0, keys: { fs_key: false, solcast_key: false, solcast_sites: 0 } },
                         energy: { source: 'off', battery: 'auto', keys: { solax_token: false, solax_sn: false } } };
const solarStatus = (solar = {}, energy = {}) => ({ device: {}, time: { valid: true }, battery: {}, sensors: {},
  preset: {}, wifi: { state: 'station', ap_on: false }, sync: { mode: 'times', running: false },
  solar: { source: 'open-meteo', ...solar }, energy: { source: 'off', ...energy } });

function solarDevice(patches, settings = SOLAR_SETTINGS, extra = {}) {
  return {
    'GET /api/settings': () => reply(200, JSON.parse(JSON.stringify(settings))),
    'GET /api/status': () => reply(200, solarStatus()),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, settings); },
    ...extra,
  };
}

/* An input by its label's text, in the order the page shows them. */
function inputNamed(root, name, n = 0) {
  const labels = below(root).filter((e) => e.tag === 'label' && text(e) === name);
  assert.ok(labels.length > n, `no field "${name}"`);
  const all = below(root);
  return all.slice(all.indexOf(labels[n]) + 1).find((e) => e.tag === 'input' || e.tag === 'select');
}

async function type(el, value) {
  el.value = value;
  await Promise.all([...(el.listeners.input || []), ...(el.listeners.change || [])].map((fn) => fn({ target: el })));
}

test('the Solar page saves our model\'s planes, losses and inverter limit', async () => {
  const patches = [];
  const { ctx, main } = await load(solarDevice(patches));
  await ctx.solarPage();
  await type(inputNamed(main, 'kWp'), '5.2');
  await buttonNamed(main, 'Add a second plane').click();
  await type(inputNamed(main, 'kWp', 1), '2.4');
  await type(inputNamed(main, 'Tilt (°)', 1), '20');
  await type(inputNamed(main, 'Azimuth (°)', 1), '-90');
  await type(inputNamed(main, 'Losses (%)'), '10');
  await type(inputNamed(main, 'Inverter limit (kW)'), '4.6');
  await buttonNamed(main, 'Save').click();
  assert.deepEqual(patches.at(-1).solar, { source: 'open-meteo', planes: [{ kwp: 5.2, tilt: 35, azimuth: 0 },
                                                                        { kwp: 2.4, tilt: 20, azimuth: -90 }],
                                           losses_pct: 10, inverter_kw: 4.6 });
});

test('keys are write-only: the page says which are set and sends only what you type', async () => {
  const patches = [];
  const settings = { ...SOLAR_SETTINGS, solar: { ...SOLAR_SETTINGS.solar, source: 'forecast-solar',
                                                 keys: { fs_key: true, solcast_key: false, solcast_sites: 0 } } };
  const { ctx, main } = await load(solarDevice(patches, settings));
  await ctx.solarPage();
  const key = inputNamed(main, 'Forecast.Solar key (optional)');
  assert.equal(key.value, '');
  assert.equal(key.attrs.type, 'password');
  assert.equal(key.attrs.autocomplete, 'new-password'); /* never the web password a password manager keeps */
  assert.match(text(main), /A key is set/);
  await buttonNamed(main, 'Save').click();
  assert.equal(patches.at(-1).solar.fs_key, undefined); /* nothing typed: the key stays as it is */
  await type(key, 'AbC123');
  await buttonNamed(main, 'Save').click();
  assert.equal(patches.at(-1).solar.fs_key, 'AbC123');
  await buttonNamed(main, 'Clear the key').click();
  await buttonNamed(main, 'Save').click();
  assert.equal(patches.at(-1).solar.fs_key, null);
});

test('a refused save says why: a second plane needs a Forecast.Solar key', async () => {
  const patches = [];
  const settings = { ...SOLAR_SETTINGS, solar: { ...SOLAR_SETTINGS.solar, source: 'forecast-solar' } };
  const { ctx, main } = await load(solarDevice(patches, settings, {
    'PATCH /api/settings': () => reply(400, { error: 'a second plane needs a Forecast.Solar key' }),
  }));
  await ctx.solarPage();
  await buttonNamed(main, 'Add a second plane').click();
  await buttonNamed(main, 'Save').click();
  assert.match(text(main), /A second plane needs a Forecast.Solar key/);
});

test('Solcast takes its key and two sites, and its planes are set on solcast.com', async () => {
  const patches = [];
  const settings = { ...SOLAR_SETTINGS, solar: { ...SOLAR_SETTINGS.solar, source: 'solcast' } };
  const { ctx, main } = await load(solarDevice(patches, settings));
  await ctx.solarPage();
  assert.equal(below(main).some((e) => e.tag === 'label' && text(e) === 'kWp' && !hiddenAbove(main, e)), false);
  await type(inputNamed(main, 'Solcast API key'), 'Kk_1-2');
  await type(inputNamed(main, 'First site id'), 'ab12-cd34');
  await type(inputNamed(main, 'Second site id (optional)'), 'ef56');
  await buttonNamed(main, 'Save').click();
  assert.equal(patches.at(-1).solar.solcast_key, 'Kk_1-2');
  assert.deepEqual(patches.at(-1).solar.solcast_sites, ['ab12-cd34', 'ef56']);
  assert.match(text(main), /10 calls a day/);
});

/* Whether `el` sits in a part of the page that is hidden. */
function hiddenAbove(root, el) {
  const path = (node, target, trail = []) => {
    if (node === target) return trail;
    for (const k of node.children.filter((c) => c instanceof FakeElement)) {
      const found = path(k, target, [...trail, node]);
      if (found) return found;
    }
    return null;
  };
  return (path(root, el) || []).some((n) => n.hidden);
}

test('the house\'s energy takes SolaX Cloud\'s token and registration number, and the battery', async () => {
  const patches = [];
  const { ctx, main } = await load(solarDevice(patches));
  await ctx.solarPage();
  await type(inputNamed(main, 'Source', 1), 'solax');
  await type(inputNamed(main, 'Token ID'), '20200722');
  await type(inputNamed(main, 'Registration number'), 'SXA1B2C3D4');
  await type(inputNamed(main, 'Home battery'), 'on');
  await buttonNamed(main, 'Save').click();
  assert.deepEqual(patches.at(-1).energy, { source: 'solax', region: 'eu', battery: 'on', solax_token: '20200722',
                                            solax_sn: 'SXA1B2C3D4' });
});

test('the Developer API takes an application\'s Client ID and Client Secret and SolaX\'s region (D37)', async () => {
  const patches = [];
  const { ctx, main } = await load(solarDevice(patches));
  await ctx.solarPage();
  await type(inputNamed(main, 'Source', 1), 'solax-dev');
  assert.ok(hiddenAbove(main, inputNamed(main, 'Token ID')), 'the Token ID shows with the Developer API');
  assert.ok(!hiddenAbove(main, inputNamed(main, 'Client ID')), 'no Client ID');
  await type(inputNamed(main, 'Client ID'), 'a1b2-c3');
  await type(inputNamed(main, 'Client Secret'), 'S3cr_et');
  await type(inputNamed(main, 'Region'), 'cn');
  await buttonNamed(main, 'Save').click();
  assert.deepEqual(patches.at(-1).energy, { source: 'solax-dev', region: 'cn', battery: 'auto',
                                            solax_client_id: 'a1b2-c3', solax_client_secret: 'S3cr_et' });
  assert.match(text(main), /developer\.solaxcloud\.com/);
});

test('the Developer API is the first SolaX source, and Europe its region by default', async () => {
  const { ctx, main } = await load(solarDevice([]));
  await ctx.solarPage();
  const source = inputNamed(main, 'Source', 1);
  assert.deepEqual(source.children.filter((c) => c instanceof FakeElement).map((o) => o.value),
                   ['off', 'solax-dev', 'solax']);
  assert.equal(inputNamed(main, 'Region').value, 'eu');
  assert.ok(hiddenAbove(main, inputNamed(main, 'Client ID')), 'its keys show with the source off');
});

test('the Solar page names the plant the Developer API found', async () => {
  const { ctx, main } = await load(solarDevice([], SOLAR_SETTINGS, {
    'GET /api/status': () => reply(200, solarStatus({}, { source: 'solax-dev', plant: '1234567890123456789' })),
  }));
  await ctx.solarPage();
  assert.match(text(main), /SolaX plant1234567890123456789/);
});

test('Check now runs the two steps and shows what they brought', async () => {
  let polls = 0;
  const { ctx, calls, main } = await load(solarDevice([], SOLAR_SETTINGS, {
    'GET /api/status': () => reply(200, ++polls < 3 ? solarStatus({ tried_at: 100 }, { source: 'solax', tried_at: 100 })
      : solarStatus({ tried_at: 200, fetched_at: 200, day: 20731 },
                    { source: 'solax', tried_at: 200, error: 'tokenId is invalid' })),
    'POST /api/solar/check': () => reply(202, { started: true }),
  }));
  ctx.setTimeout = (fn) => { fn(); return 0; };
  await ctx.solarPage();
  await buttonNamed(main, 'Check now').click();
  await settle();
  assert.ok(calls.some((c) => c.path === '/api/solar/check' && c.init.method === 'POST'));
  assert.match(text(main), /tokenId is invalid/);
  assert.match(text(main), /Checked/);
});

test('the Solar page says when Solcast is asked again, and why its last call failed', async () => {
  const solcast = solarStatus({ source: 'solcast', fetched_at: 100, tried_at: 300, kept: 'kept', next_at: 1790890800,
                                error: 'HTTP 401' });
  const { ctx, main } = await load(solarDevice([], SOLAR_SETTINGS, { 'GET /api/status': () => reply(200, solcast) }));
  await ctx.solarPage();
  assert.match(text(main), /Its last stepkept the forecast: Solcast is asked again from /);
  assert.match(text(main), /Its last callfailed: HTTP 401/);
  const limited = solarStatus({ source: 'forecast-solar', fetched_at: 100, tried_at: 300, kept: 'HTTP 429' });
  const page = await load(solarDevice([], SOLAR_SETTINGS, { 'GET /api/status': () => reply(200, limited) }));
  await page.ctx.solarPage();
  assert.match(text(page.main), /Its last stepkept the forecast \(HTTP 429\)/);
  assert.doesNotMatch(text(page.main), /Its last call/);
});

test('the Solar page credits its sources', async () => {
  const { ctx, main } = await load(solarDevice([]));
  await ctx.solarPage();
  const all = text(main);
  for (const credit of ['Open-Meteo', 'Forecast.Solar', 'CC BY-SA 4.0', 'Solcast', 'personal use', 'SolaX Cloud']) {
    assert.ok(all.includes(credit), credit);
  }
});

test('a preset on the Solar or Energy layout has no slots to fill', async () => {
  const catalogue = { ...CATALOGUE, layouts: [...CATALOGUE.layouts, { id: 'solar', slots: [] }, { id: 'energy', slots: [] }] };
  const doc = { schema: 1, active: 'solar', presets: [{ id: 'solar', name: 'Solar', layout: 'solar', in_cycle: false,
                                                          slots: {}, options: {} }],
                cycle: { enabled: false, interval_s: 60 }, schedule: { enabled: false, entries: [] } };
  const { ctx, main } = await load({
    'GET /api/layouts': () => reply(200, catalogue), 'GET /api/presets': () => reply(200, doc),
    'GET /api/fields': () => reply(200, FIELDS), 'POST /api/preview.bmp': () => reply(200, 'BM', 'image/bmp'),
    'GET /api/settings': () => reply(200, STEP_SETTINGS),
  });
  await ctx.presetsPage();
  assert.match(text(main), /today's PV forecast: its source is on the Solar page/);
  assert.match(text(main), /Solar/);
});

test('the field lists group the Solar and Energy fields and mark those whose step is off', async () => {
  const catalogue = { ...CATALOGUE, layouts: [{ id: 'classic', slots: [
    { id: 's1', x: 0, y: 172, w: 200, h: 128, size: 'S', kinds: ['number'] }] }] };
  const fields = { fields: [{ id: 'env.temp', kind: 'number', label: 'Temperature', value: '23.7 °C' },
                            { id: 'pv.now', kind: 'number', label: 'Forecast now', value: '3.50 kW' },
                            { id: 'energy.pv', kind: 'number', label: 'Solar', value: '3.42 kW' }] };
  const doc = { schema: 1, active: 'home', presets: [{ id: 'home', name: 'Home', layout: 'classic', in_cycle: true,
                                                         slots: {}, options: {} }],
                cycle: { enabled: false, interval_s: 60 }, schedule: { enabled: false, entries: [] } };
  const settings = { ...STEP_SETTINGS, sync: { ...STEP_SETTINGS.sync, steps: ['weather', 'air', 'radar', 'solar'] } };
  const { ctx, main } = await load({
    'GET /api/layouts': () => reply(200, catalogue), 'GET /api/presets': () => reply(200, doc),
    'GET /api/fields': () => reply(200, fields), 'POST /api/preview.bmp': () => reply(200, 'BM', 'image/bmp'),
    'GET /api/settings': () => reply(200, settings),
  });
  await ctx.presetsPage();
  const groups = below(main).filter((e) => e.tag === 'optgroup').map((g) => g.attrs.label);
  assert.ok(groups.includes('Solar forecast') && groups.includes('House energy'), groups.join());
  const options = below(main).filter((e) => e.tag === 'option').map(text);
  assert.ok(options.some((o) => /^Solar — 3.42 kW \(its sync step is off\)$/.test(o)), options.join(' | '));
  assert.ok(options.some((o) => /^Forecast now — 3.50 kW$/.test(o)), options.join(' | '));
});

/* ---- titles and shared presets (D42, spec §10.3) ---- */

const inputsLabelled = (root, label) => below(root).flatMap((el) => el.children
  .map((k, i) => (k instanceof FakeElement && k.tag === 'label' && text(k) === label ? el.children[i + 1] : null))
  .filter(Boolean));
const checkNamed = (root, name) => below(root).find((e) => e.tag === 'label' && text(e) === name).children[0];

test('the status bar\'s last sync time is saved while ticked and left out when not (D43)', async () => {
  const saved = [];
  const { ctx, main } = await load(presetDevice(saved));
  await ctx.presetsPage();
  await change(checkNamed(main, 'Last sync time'), true);
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.deepEqual(saved.at(-1).presets.filter((p) => p.options && p.options.status_sync === true).length, 1);
  await change(checkNamed(main, 'Last sync time'), false);
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.ok(saved.at(-1).presets.every((p) => !p.options || !('status_sync' in p.options)), JSON.stringify(saved.at(-1)));
});

test('a cell\'s title and its force are saved with it, and gone when cleared', async () => {
  const saved = [];
  const { ctx, main } = await load(splitDevice(saved, { field: 'env.temp' }));
  await ctx.presetsPage();
  await change(control(main, 'Title'), '  Obývák ');
  await change(checkNamed(main, 'Always show the title'), true);
  let p = await savedTree(main, saved);
  assert.deepEqual(p.split, { field: 'env.temp', title: 'Obývák', force_title: true });
  await change(control(main, 'Title'), '');
  await change(checkNamed(main, 'Always show the title'), false);
  p = await savedTree(main, saved);
  assert.deepEqual(p.split, { field: 'env.temp' });
});

test('a title is cut to 23 bytes where a letter starts', async () => {
  const saved = [];
  const { ctx, main } = await load(splitDevice(saved, { field: 'env.temp' }));
  await ctx.presetsPage();
  await change(control(main, 'Title'), 'ř'.repeat(12)); /* 24 bytes */
  const p = await savedTree(main, saved);
  assert.equal(p.split.title, 'ř'.repeat(11));
});

test('a title says what shows without one: the field\'s own label', async () => {
  const { ctx, main } = await load(splitDevice([], { field: 'wx.hourly' }));
  await ctx.presetsPage();
  assert.equal(control(main, 'Title').attrs.placeholder, 'Next hours');
});

test('an empty cell offers no force for its title', async () => {
  const { ctx, main } = await load(splitDevice([], {}));
  await ctx.presetsPage();
  assert.equal(below(main).find((e) => e.tag === 'label' && text(e) === 'Always show the title'), undefined);
});

test('a fixed slot\'s title is saved in the preset\'s titles', async () => {
  const saved = [];
  const { ctx, main } = await load(presetDevice(saved));
  await ctx.presetsPage();
  const [mainTitle, s1Title] = inputsLabelled(main, 'Title');
  assert.ok(mainTitle && s1Title, 'a title for each slot');
  await change(s1Title, 'Obývák');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.deepEqual(saved.at(-1).presets[1].titles, { s1: 'Obývák' });
  await change(inputsLabelled(main, 'Title')[1], '');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(saved.at(-1).presets[1].titles, undefined);
});

test('Split and Join keep a cell\'s title and force with its field', async () => {
  const saved = [];
  const { ctx, main } = await load(splitDevice(saved, { field: 'time.clock', title: 'Praha', force_title: true }));
  await ctx.presetsPage();
  await buttonNamed(main, 'Split into rows').click();
  let p = await savedTree(main, saved);
  assert.deepEqual(p.split.a, { field: 'time.clock', title: 'Praha', force_title: true });
  assert.deepEqual(p.split.b, {});
  await buttonNamed(main, 'Join').click();
  p = await savedTree(main, saved);
  assert.deepEqual(p.split, { field: 'time.clock', title: 'Praha', force_title: true });
});

test('a ratio that leaves a field without room takes its title too', async () => {
  const saved = [];
  const { ctx, main } = await load(splitDevice(saved, { split: 'rows', ratio: '1/2', line: true, a: {},
                                                        b: { field: 'wx.hourly', title: 'Hodiny', force_title: true } }));
  await ctx.presetsPage();
  await change(ratioSelects(main)[0], '3/4');
  const p = await savedTree(main, saved);
  assert.deepEqual(p.split.b, {});
});

test('Export downloads the preset as a reflbo file named by its id', async () => {
  const { ctx, blobs, main } = await load(splitDevice([], { field: 'env.temp', title: 'Obývák' }));
  await ctx.presetsPage();
  await buttonNamed(main, 'Export').click();
  assert.equal(blobs.length, 1); /* the preview's timer never fires here */
  const file = JSON.parse(await blobs[0].text());
  assert.deepEqual(file, { reflbo: 'preset', version: 1, preset: { id: 'home', name: 'Home', layout: 'split',
                                                                  in_cycle: true, split: { field: 'env.temp', title: 'Obývák' }, options: {} } });
  const link = ctx.document.body.children.find((e) => e.tag === 'a');
  assert.equal(link.attrs.download, 'home.json');
});

/* A device for imports: Home and Weather as presetDevice(), its preview answering `preview` for each POST. */
function importDevice(saved, previews, preview = () => reply(200, 'BM', 'image/bmp')) {
  return { ...presetDevice(saved),
           'POST /api/preview.bmp': (init) => { previews.push(JSON.parse(init.body)); return preview(init); } };
}

async function importFile(main, content) {
  const input = below(main).find((e) => e.tag === 'input' && e.attrs.type === 'file');
  assert.ok(input, 'a file input');
  input.files = [{ text: async () => content }];
  await Promise.all((input.listeners.change || []).map((fn) => fn({ target: input })));
  await settle();
}

const EXPORTED = JSON.stringify({ reflbo: 'preset', version: 1, preset: {
  id: 'weather', name: 'Weather', layout: 'classic', in_cycle: false, slots: { main: 'time.clock' },
  titles: { main: 'Hodiny' }, options: { seconds: false }, window: { from: 'sunrise', until: 'sunset' } } });

test('Import adds the preset after the selected one, with an id of its own, in the cycle', async () => {
  const saved = [], previews = [];
  const { ctx, main, byId } = await load(importDevice(saved, previews));
  await ctx.presetsPage(); /* Weather, the second, is selected */
  await importFile(main, EXPORTED);
  const asked = previews.at(-1);
  assert.deepEqual(asked.presets.map((p) => p.id), ['home', 'weather', 'weather-2'], 'the device checked it in place');
  assert.match(text(byId.toast), /Weather imported/);
  await buttonNamed(main, 'Save').click();
  await settle();
  const added = saved.at(-1).presets[2];
  assert.equal(added.id, 'weather-2');
  assert.equal(added.in_cycle, true);
  assert.deepEqual(added.titles, { main: 'Hodiny' });
  assert.deepEqual(added.window, { from: 'sunrise', until: 'sunset' }, 'keys this page doesn\'t know go along');
});

test('Import takes a bare preset too', async () => {
  const saved = [], previews = [];
  const { ctx, main } = await load(importDevice(saved, previews));
  await ctx.presetsPage();
  await importFile(main, JSON.stringify({ id: 'x', name: 'Bare', layout: 'classic', slots: {}, options: {} }));
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(saved.at(-1).presets[2].name, 'Bare');
});

test('a preset the device refuses is not added, and the page says why', async () => {
  const saved = [], previews = [];
  const { ctx, main, byId } = await load(importDevice(saved, previews,
    () => reply(400, { error: 'preset "weather-2": a split\'s parts must be at least 40×20' })));
  await ctx.presetsPage();
  await importFile(main, EXPORTED);
  assert.match(text(byId.toast), /Can't import Weather: Preset "weather-2": a split's parts must be at least 40×20\./);
  assert.equal(below(main).filter((e) => e.tag === 'li').length, 2, 'the list as it was');
});

test('a file that is no preset says so', async () => {
  const previews = [];
  const { ctx, main, byId } = await load(importDevice([], previews));
  await ctx.presetsPage();
  await importFile(main, '{"foo": 1}');
  assert.equal(text(byId.toast), 'That file holds no preset.');
  await importFile(main, 'not json');
  assert.equal(text(byId.toast), 'That file is not JSON.');
  assert.equal(previews.length, 0, 'nothing asked of the device');
});

test('Import at 16 presets says so', async () => {
  const doc = { schema: 1, active: 'p0', presets: Array.from({ length: 16 }, (_, i) => preset(`p${i}`, `P${i}`, true)),
                cycle: { enabled: false, interval_s: 60 }, schedule: { enabled: false, entries: [] } };
  const { ctx, main, byId } = await load({ ...presetDevice([]), 'GET /api/presets': () => reply(200, doc) });
  await ctx.presetsPage();
  await importFile(main, EXPORTED);
  assert.equal(text(byId.toast), '16 presets at most');
});

test('a bare preset without slots or a tree imports as an empty one, and the page carries on', async () => {
  const saved = [], previews = [];
  const { ctx, main, byId } = await load({ ...importDevice(saved, previews), 'GET /api/layouts': () => reply(200, SPLIT_CATALOGUE) });
  await ctx.presetsPage();
  await importFile(main, JSON.stringify({ name: 'Bare split', layout: 'split' }));
  await importFile(main, JSON.stringify({ name: 'Bare classic', layout: 'classic', split: { field: 'env.temp' } }));
  assert.match(text(byId.toast), /Bare classic imported/);
  assert.deepEqual(cellLabels(main), [], 'the selected one, Bare classic, has slots, not a tree');
  await buttonNamed(main, 'Save').click();
  await settle();
  const [split, classic] = saved.at(-1).presets.slice(2);
  assert.deepEqual([split.name, split.split, split.slots], ['Bare split', {}, undefined]);
  assert.deepEqual([classic.name, classic.slots, classic.split], ['Bare classic', {}, undefined]);
});

/* ---- M7 (D44): the PC page, Show as, the PC's readings in the editor ---- */

const PC_SETTINGS = { schema: 1, display: { update_min: 1, lpm_hz: 1 },
                      pc: { enabled: false, host: '', port: 47123, interval_ms: 1000, auto_always: true, preset: 'pc',
                            keys: { token: false } } };
const PC_PRESETS = { schema: 1, active: 'home', presets: [preset('home', 'Home', true), { ...preset('pc', 'PC', false),
                     layout: 'pc' }], cycle: { enabled: false, interval_s: 60 }, schedule: { enabled: false, entries: [] } };
const PC_STATE = { enabled: false, session: false, connected: false, demo: false, set_always: false, name: '',
                   hwinfo: 'unknown', preset: '', since: 0, age_ms: 0, trouble: '', catalog: [],
                   step: { at: 0, result: 'not run', detail: '' }, test: { at: 0, result: 'not run', detail: '' },
                   testing: false };

function pcDevice(patches, settings = PC_SETTINGS, states = [PC_STATE], extra = {}) {
  let n = 0;
  return {
    'GET /api/settings': () => reply(200, settings),
    'GET /api/presets': () => reply(200, PC_PRESETS),
    'GET /api/pc': () => reply(200, states[Math.min(n++, states.length - 1)]),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, settings); },
    ...extra,
  };
}

test('the PC page saves the address, the port, the interval and what a session switches', async () => {
  const patches = [];
  const { ctx, main } = await load(pcDevice(patches));
  await ctx.pcPage();
  checkNamed(main, 'On').checked = true;
  await type(inputNamed(main, 'Address'), ' 192.168.1.20 ');
  await type(inputNamed(main, 'Port'), '47200');
  await type(inputNamed(main, 'A reading every (ms)'), '2000');
  checkNamed(main, 'Switch to Always on').checked = false;
  await change(control(main, 'Show preset'), '');
  await buttonNamed(main, 'Save').click();
  assert.deepEqual(patches.at(-1), { pc: { enabled: true, host: '192.168.1.20', port: 47200, interval_ms: 2000,
                                           auto_always: false, preset: '' } });
  assert.deepEqual(options(control(main, 'Show preset')), ['', 'home', 'pc']); /* none, or any preset */
});

test('the PC\'s token is write-only: the page says whether one is set and sends only what you type', async () => {
  const patches = [];
  const settings = { ...PC_SETTINGS, pc: { ...PC_SETTINGS.pc, keys: { token: true } } };
  const { ctx, main } = await load(pcDevice(patches, settings));
  await ctx.pcPage();
  const token = inputNamed(main, 'Token');
  assert.equal(token.attrs.type, 'password');
  assert.match(text(main), /A key is set/);
  await buttonNamed(main, 'Save').click();
  assert.equal(patches.at(-1).pc.token, undefined); /* nothing typed: it stays */
  await type(token, 'ABCDE-FGHIJ-KLMNO-PQRST-UVWXY-Z');
  await buttonNamed(main, 'Save').click();
  assert.equal(patches.at(-1).pc.token, 'ABCDE-FGHIJ-KLMNO-PQRST-UVWXY-Z'); /* as typed (spec §12a.1) */
});

test('an interval shorter than the panel\'s frame time shows a warning', async () => {
  const { ctx, main } = await load(pcDevice([]));
  await ctx.pcPage();
  const warning = below(main).find((e) => /frame time/.test(e.textContent));
  assert.ok(warning, 'the warning is there');
  assert.equal(warning.hidden, true); /* 1000 ms matches 1 s frame time */
  await type(inputNamed(main, 'A reading every (ms)'), '500');
  assert.equal(warning.hidden, false);
  assert.match(warning.textContent, /1 s/);
  await type(inputNamed(main, 'A reading every (ms)'), '1000');
  assert.equal(warning.hidden, true);
});

test('Test asks the PC at once and says what came back', async () => {
  const found = { ...PC_STATE, session: true, connected: true, name: 'DESKTOP-PC', hwinfo: 'ok',
                  test: { at: 1800000000, result: 'ok', detail: '' } };
  const tested = [];
  const { ctx, main } = await load(pcDevice([], PC_SETTINGS, [PC_STATE, PC_STATE, found], {
    'POST /api/pc/test': () => { tested.push(true); return reply(202, { started: true }); },
  }));
  ctx.setTimeout = (fn) => { fn(); return 0; }; /* sleep() returns at once */
  await ctx.pcPage();
  await buttonNamed(main, 'Test').click();
  await settle();
  assert.equal(tested.length, 1);
  assert.match(text(main), /Found DESKTOP-PC\./);
  assert.match(text(main), /HWiNFO\s*running/);
  const refused = { ...PC_STATE, test: { at: 1800000000, result: 'failed', detail: 'wrong token' } };
  const page = await load(pcDevice([], PC_SETTINGS, [PC_STATE, refused], {
    'POST /api/pc/test': () => reply(202, { started: true }),
  }));
  page.ctx.setTimeout = (fn) => { fn(); return 0; };
  await page.ctx.pcPage();
  await buttonNamed(page.main, 'Test').click();
  await settle();
  assert.match(text(page.main), /Not found: wrong token\./);
});

test('the PC page shows the session and the readings it knows', async () => {
  const state = { ...PC_STATE, session: true, connected: true, name: 'DESKTOP-PC', hwinfo: 'stale', age_ms: 400,
                  set_always: true, catalog: [{ id: 'cpu_load', label: 'CPU load', unit: '%' },
                                              { id: 'ram_used', label: 'RAM', unit: 'GB' }] };
  const { ctx, main } = await load(pcDevice([], PC_SETTINGS, [state]));
  await ctx.pcPage();
  const all = text(main);
  assert.match(all, /Session\s*running/);
  assert.match(all, /HWiNFO\s*not updating/);
  assert.match(all, /400 ms ago/);
  assert.match(all, /Always on, until the session ends/);
  assert.match(all, /Readings \(2\)/);
  assert.match(all, /CPU load pc\.cpu_load · %/);
});

/* The device's rules for the visuals (GET /api/layouts, ui_catalog.c). */
const VISUALS = { visuals: { chart: { min_w: 80, min_h: 40 }, bar: { min_w: 60, min_h: 16 },
                             ring: { min_w: 40, min_h: 40 }, gauge: { min_w: 60, min_h: 40 } },
                  spans: { min_s: 10, values_max_s: 3600, hours_min_s: 7200, max_s: 86400 } };
const SHOW_FIELDS = { fields: [{ id: 'time.clock', kind: 'time', label: 'Time', value: '12:15' },
                               { id: 'env.temp', kind: 'number', label: 'Temperature', value: '23.7 °C', numeric: true },
                               { id: 'env.hum', kind: 'number', label: 'Humidity', value: '45 %', numeric: true,
                                 min: 0, max: 100 },
                               { id: 'pc.cpu_load', kind: 'number', label: 'CPU load', value: '55 %', numeric: true,
                                 min: 0, max: 100, group: 'pc' }] };

function showDevice(saved, slots, tree) {
  const catalogue = { ...SPLIT_CATALOGUE, ...VISUALS, layouts: [{ id: 'classic', slots }, { id: 'split', slots: [] }] };
  const home = tree ? { id: 'home', name: 'Home', layout: 'split', in_cycle: true, split: tree, options: {} }
                    : { id: 'home', name: 'Home', layout: 'classic', in_cycle: true, slots: { s1: 'env.temp' },
                        options: {} };
  const doc = { schema: 1, active: 'home', presets: [home], cycle: { enabled: false, interval_s: 60 },
                schedule: { enabled: false, entries: [] } };
  return {
    'GET /api/layouts': () => reply(200, catalogue),
    'GET /api/presets': () => reply(200, JSON.parse(JSON.stringify(doc))),
    'GET /api/fields': () => reply(200, SHOW_FIELDS),
    'POST /api/preview.bmp': () => reply(200, 'BM', 'image/bmp'),
    'PUT /api/presets': (init) => { saved.push(JSON.parse(init.body)); return reply(200, JSON.parse(init.body)); },
  };
}
const BIG = [{ id: 's1', x: 0, y: 172, w: 200, h: 128, size: 'S', kinds: ['number', 'time'] }];

test('a number offers Show as, with only the visuals that fit its slot', async () => {
  const { ctx, main } = await load(showDevice([], BIG));
  await ctx.presetsPage();
  assert.deepEqual(options(control(main, 'Show as')), ['', 'chart', 'bar', 'ring', 'gauge']);
  const small = [{ id: 's1', x: 0, y: 172, w: 70, h: 30, size: 'XS', kinds: ['number', 'time'] }];
  const page = await load(showDevice([], small));
  await page.ctx.presetsPage();
  assert.deepEqual(options(control(page.main, 'Show as')), ['', 'bar']); /* 70×30: a bar alone */
});

test('a chart is saved in the preset\'s visuals with its span, bounds and numbers', async () => {
  const saved = [];
  const { ctx, main } = await load(showDevice(saved, BIG));
  await ctx.presetsPage();
  await change(control(main, 'Show as'), 'chart');
  await change(control(main, 'Span'), '7200');
  await type(inputNamed(main, 'Minimum'), 'auto');
  await type(inputNamed(main, 'Maximum'), '30');
  checkNamed(main, 'Numbers over the chart').checked = false;
  await Promise.all(checkNamed(main, 'Numbers over the chart').listeners.change.map((fn) => fn({ target: { checked: false } })));
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.deepEqual(saved.at(-1).presets[0].visuals, { s1: { as: 'chart', span_s: 7200, numbers: false, min: 'auto',
                                                           max: 30 } });
});

test('a field\'s own range shows where a bound is left empty', async () => {
  const { ctx, main } = await load(showDevice([], BIG));
  await ctx.presetsPage();
  const select = below(main).find((e) => e.tag === 'select' && options(e).includes('env.hum'));
  await change(select, 'env.hum');
  await change(control(main, 'Show as'), 'gauge');
  assert.equal(inputNamed(main, 'Minimum').attrs.placeholder, '0');
  assert.equal(inputNamed(main, 'Maximum').attrs.placeholder, '100');
  await change(select, 'env.temp'); /* no range of its own: the page says what empty means */
  await change(control(main, 'Show as'), 'bar');
  assert.match(text(main), /no range of its own/);
});

test('a field that isn\'t a number takes its slot\'s visual away', async () => {
  const saved = [];
  const { ctx, main } = await load(showDevice(saved, BIG));
  await ctx.presetsPage();
  await change(control(main, 'Show as'), 'ring');
  const select = below(main).find((e) => e.tag === 'select' && options(e).includes('time.clock'));
  await change(select, 'time.clock');
  assert.ok(!below(main).some((e) => e.tag === 'label' && text(e) === 'Show as'), 'no Show as for a time');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(saved.at(-1).presets[0].visuals, undefined);
});

test('a cell\'s visual goes with its field on Split, and stays out where it has no room', async () => {
  const saved = [];
  const { ctx, main } = await load(showDevice(saved, BIG, { field: 'env.temp', show: { as: 'chart', span_s: 60,
                                                                                   numbers: true } }));
  await ctx.presetsPage();
  await buttonNamed(main, 'Split into rows').click(); /* 400×139: the chart still fits */
  let p = await savedTree(main, saved);
  assert.deepEqual(p.split.a, { field: 'env.temp', show: { as: 'chart', span_s: 60, numbers: true } });
  const page = await load(showDevice(saved, BIG, { field: 'env.temp', show: { as: 'gauge' } }));
  await page.ctx.presetsPage();
  const ratio = { split: 'columns', ratio: '1/2', line: true, a: { field: 'env.temp', show: { as: 'gauge' } }, b: {} };
  const narrow = await load(showDevice(saved, BIG, ratio));
  await narrow.ctx.presetsPage();
  await change(ratioSelects(narrow.main)[0], '1/4'); /* 100 px: a gauge's 60 px still fit */
  p = await savedTree(narrow.main, saved);
  assert.deepEqual(p.split.a.show, { as: 'gauge' });
});

test('the field lists group the PC\'s readings', async () => {
  const { ctx, main } = await load(showDevice([], BIG));
  await ctx.presetsPage();
  const groups = below(main).filter((e) => e.tag === 'optgroup').map((g) => g.attrs.label);
  assert.ok(groups.includes('PC'), groups.join());
  assert.ok(below(main).filter((e) => e.tag === 'option').map(text).includes('CPU load — 55 %'));
});

test('the field lists group the LoRa readings', async () => {
  const fields = { fields: [...SHOW_FIELDS.fields,
    { id: 'lora.rssi', kind: 'number', label: 'LoRa RSSI', value: '-72 dBm', numeric: true,
      min: -130, max: -30 }] };
  const { ctx, main } = await load({ ...showDevice([], BIG), 'GET /api/fields': () => reply(200, fields) });
  await ctx.presetsPage();
  const groups = below(main).filter((e) => e.tag === 'optgroup').map((g) => g.attrs.label);
  assert.ok(groups.includes('LoRa'), groups.join());
  assert.ok(below(main).filter((e) => e.tag === 'option').map(text).includes('LoRa RSSI — -72 dBm'));
});

test('the Sync page switches the PC step too', async () => {
  const patches = [];
  const settings = { ...STEP_SETTINGS, sync: { ...STEP_SETTINGS.sync, steps: ['weather', 'pc'] } };
  const { ctx, main } = await load({
    'GET /api/settings': () => reply(200, settings),
    'GET /api/status': () => reply(200, syncStatus({ mode: 'times', running: false })),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, settings); },
  });
  await ctx.syncPage();
  const box = (name) => below(main).find((e) => e.tag === 'label' && text(e) === name).children[0];
  assert.equal(box('PC').checked, true);
  box('PC').checked = false;
  await buttonNamed(main, 'Save steps').click();
  assert.deepEqual(patches.at(-1), { sync: { steps: ['weather'] } });
});

test('a slot\'s PC reading the PC no longer sends stays, named, with its visual', async () => {
  const saved = [];
  const device = showDevice(saved, BIG);
  const doc = { schema: 1, active: 'home', presets: [{ id: 'home', name: 'Home', layout: 'classic', in_cycle: true,
                slots: { s1: 'pc.gone' }, visuals: { s1: { as: 'ring' } }, options: {} }],
                cycle: { enabled: false, interval_s: 60 }, schedule: { enabled: false, entries: [] } };
  device['GET /api/presets'] = () => reply(200, JSON.parse(JSON.stringify(doc)));
  const { ctx, main } = await load(device);
  await ctx.presetsPage();
  const select = below(main).find((e) => e.tag === 'select' && options(e).includes('pc.gone'));
  assert.ok(select, 'the slot still names it');
  assert.ok(select.children.some((o) => o instanceof FakeElement && o.value === 'pc.gone' && o.selected));
  assert.ok(below(main).filter((e) => e.tag === 'option').map(text).some((t) => /^pc\.gone \(not sent now\)$/.test(t)));
  assert.equal(control(main, 'Show as').value, 'ring');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.deepEqual(saved.at(-1).presets[0].visuals, { s1: { as: 'ring' } });
});

/* ---- LoRa (spec §10.3) ---- */

const LORA_SETTINGS = {
  schema: 1,
  lora: {
    enabled: true,
    freq_khz: 868000,
    bw_hz: 250000,
    bw_khz: 250,
    sf: 7,
    cr: 5,
    tx_power_dbm: 22,
    sync_word: 0x1424,
    rx_boosted: true,
    cad_enabled: false,
    client_repeat: false,
    path_hash_mode: 0,
    airtime_factor: 1,
    airtime_factor_x10: 10,
    wake_on_rx: false,
    share_loc: false,
    ble_pin: 0,
    node_name: 'NM-EPD-420',
    channel_name: 'Public',
    keys: { psk: false, channel_psk: false },
    is_public_psk: true,
  },
};

const LORA_STATUS = {
  device: { id: 'reflbo-bb94', firmware: '0.4.0', uptime_s: 3600, free_heap: 4194304 },
  preset: { name: 'Home', cycle: false },
  time: { valid: true, epoch: 1790880000, local: '2026-10-01T12:00:00', tz_iana: 'Europe/Prague' },
  battery: { percent: 90, mv: 4100, state: 'discharging', days_left: 20 },
  sensors: { temp_c: 21.5, hum_pct: 50, age_s: 30 },
  wifi: { state: 'station', ssid: 'Home', ip: '192.168.1.50', ap_on: false },
  lora: {
    enabled: true,
    initialized: true,
    ble_pin: 482915,
    ble_connected: false,
    node_name: 'NM-EPD-420',
    pub_key: '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
    freq_khz: 868000,
    bw_hz: 250000,
    bw_khz: 250,
    sf: 7,
    cr: 5,
    tx_power_dbm: 22,
    sync_word: 0x1424,
    rx_boosted: true,
    cad_enabled: false,
    client_repeat: false,
    path_hash_mode: 0,
    airtime_factor: 1,
    wake_on_rx: false,
    share_loc: false,
    channel_name: 'Public',
    channel_hash: 0x12,
    noise_floor_dbm: -112,
    packet_count: 42,
    packets_sent: 10,
    recv_flood: 30,
    recv_direct: 12,
    sent_flood: 8,
    sent_direct: 2,
    recv_errors: 0,
    tx_air_ms: 1200,
    rx_air_ms: 4500,
    last_rssi_dbm: -74,
    last_snr_db: 9,
    last_packet_at: 1790879950,
    age_s: 50,
    adverts: [
      {
        name: 'Repeater-1',
        node_type: 2,
        pub_key: 'aabbccddeeff0011',
        hops: 1,
        rssi_dbm: -80,
        snr_db: 7.5,
        has_loc: true,
        lat: 49.1951,
        lon: 16.6068,
        age_s: 120,
      },
    ],
    neighbors: [
      {
        name: 'Repeater-1',
        node_type: 2,
        pub_key: 'aabbccddeeff0011',
        snr_in_db: 6.5,
        snr_out_db: 7.0,
        rssi_dbm: -82,
        age_s: 100,
      },
    ],
    messages: [
      {
        text: 'Hello MeshCore',
        rssi_dbm: -75,
        snr_db: 8.0,
        hops: 0,
        outgoing: false,
        age_s: 60,
      },
    ],
  },
};

function loraDevice(patches, customSettings = {}, customStatus = {}, resetCalls = [], actions = {}) {
  const settings = { ...LORA_SETTINGS, ...customSettings, lora: { ...LORA_SETTINGS.lora, ...(customSettings.lora || {}) } };
  let currentStatus = {
    ...LORA_STATUS,
    ...customStatus,
    lora: customStatus.lora === null ? undefined : { ...LORA_STATUS.lora, ...(customStatus.lora || {}) },
  };
  return {
    'GET /api/settings': () => reply(200, settings),
    'GET /api/status': () => reply(200, currentStatus),
    'PATCH /api/settings': (init) => {
      const p = JSON.parse(init.body);
      patches.push(p);
      if (p.lora) {
        Object.assign(settings.lora, p.lora);
        if ('channel_psk' in p.lora) {
          const custom = Boolean(p.lora.channel_psk && p.lora.channel_psk !== 'izOH6cXN6mrJ5e26oRXNcg==');
          settings.lora.keys = { psk: custom, channel_psk: custom };
          settings.lora.is_public_psk = !custom;
          delete settings.lora.channel_psk;
        }
        if (currentStatus.lora) Object.assign(currentStatus.lora, p.lora);
      }
      return reply(200, settings);
    },
    'POST /api/lora/reset': () => {
      resetCalls.push(true);
      currentStatus = { ...currentStatus, lora: { ...currentStatus.lora, packet_count: 0 } };
      return reply(200, { ok: true });
    },
    'POST /api/lora/advert': (init) => {
      actions.advert = JSON.parse(init.body || '{}');
      return reply(200, { ok: true });
    },
    'POST /api/lora/discover': () => {
      actions.discover = true;
      return reply(200, { ok: true });
    },
    'POST /api/lora/send': (init) => {
      actions.send = JSON.parse(init.body || '{}');
      (actions.sends ||= []).push(actions.send);
      return reply(200, { ok: true });
    },
    'GET /api/lora/chat': () => {
      actions.chatGets = (actions.chatGets || 0) + 1;
      return reply(200, { messages: currentStatus.lora?.messages || [] });
    },
    'POST /api/lora/contacts': (init) => {
      actions.contacts = JSON.parse(init.body || '{}');
      (actions.contactsLog ||= []).push(actions.contacts);
      return reply(200, { ok: true });
    },
    'POST /api/lora/channels': (init) => {
      actions.channels = JSON.parse(init.body || '{}');
      return reply(200, { ok: true });
    },
    'POST /api/lora/messages/clear': () => {
      actions.clearMessages = (actions.clearMessages || 0) + 1;
      if (currentStatus.lora) currentStatus.lora.messages = [];
      return reply(200, { ok: true });
    },
    'POST /api/lora/cli': (init) => {
      const b = JSON.parse(init.body || '{}');
      actions.cli = b;
      return reply(200, { ok: true, reply: `CLI output for ${b.cmd}` });
    },
    'POST /api/lora/rekey': () => {
      actions.rekey = (actions.rekey || 0) + 1;
      currentStatus = {
        ...currentStatus,
        lora: {
          ...currentStatus.lora,
          legacy_identity: false,
          pub_key: 'fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210',
          ble_pin: 765432,
        },
      };
      return reply(200, {
        ok: true,
        pub_key: currentStatus.lora.pub_key,
        ble_pin: 765432,
        legacy_identity: false,
      });
    },
  };
}

test('web/index.html includes LoRa nav link between PC and Device', () => {
  const html = fs.readFileSync(INDEX_HTML, 'utf8');
  assert.match(html, /<a href="#pc">PC<\/a>\s*<a href="#lora">LoRa<\/a>\s*<a href="#device">Device<\/a>/);
});

test('navigating to #lora displays status facts and settings', async () => {
  const patches = [];
  const { ctx, main } = await load(loraDevice(patches));
  ctx.location.hash = '#lora';
  ctx.route();
  await settle();

  assert.match(text(main), /Listening on 868\.000 MHz · SF7 · 250 kHz · CR 4\/5/);
  assert.match(text(main), /42/);
  assert.match(text(main), /-74 dBm \/ 9 dB/);

  assert.equal(checkNamed(main, 'Enabled').checked, true);
  assert.equal(control(main, 'Frequency (MHz)').value, '868.000');
  assert.equal(control(main, 'Spreading factor').value, '7');
  assert.equal(control(main, 'Bandwidth').value, '250');
  assert.equal(control(main, 'Coding rate').value, '5');
  assert.equal(control(main, 'Sync word').value, '1424');
  assert.equal(checkNamed(main, 'Wake from sleep on packet').checked, false);
});

test('statusPage shows LoRa row when present in status', async () => {
  const { ctx, main } = await load(loraDevice([]));
  await ctx.statusPage();
  assert.match(text(main), /LoRaListening on 868\.000 MHz · SF7 · 250 kHz · CR 4\/5/);

  const offDevice = loraDevice([], {}, { lora: { enabled: false } });
  const offPage = await load(offDevice);
  await offPage.ctx.statusPage();
  assert.match(text(offPage.main), /LoRaOff/);

  const uninitDevice = loraDevice([], {}, { lora: { enabled: true, initialized: false } });
  const uninitPage = await load(uninitDevice);
  await uninitPage.ctx.statusPage();
  assert.match(text(uninitPage.main), /LoRaModule not detected/);

  const noLoraDevice = loraDevice([], {}, { lora: null });
  const noLoraPage = await load(noLoraDevice);
  await noLoraPage.ctx.statusPage();
  assert.equal(/LoRa/.test(text(noLoraPage.main)), false);
});

test('reset counter resets packet count via POST /api/lora/reset', async () => {
  const resetCalls = [];
  const { ctx, main } = await load(loraDevice([], {}, {}, resetCalls));
  await ctx.loraPage();

  assert.match(text(main), /42/);
  await buttonNamed(main, 'Reset counter').click();
  await settle();

  assert.equal(resetCalls.length, 1);
  assert.match(text(main), /Packets0/);
});

test('LoRa Refresh button updates facts from fresh status', async () => {
  let statusCount = 42;
  const dev = {
    'GET /api/settings': () => reply(200, LORA_SETTINGS),
    'GET /api/status': () => reply(200, {
      ...LORA_STATUS,
      lora: { ...LORA_STATUS.lora, packet_count: statusCount },
    }),
  };
  const { ctx, main } = await load(dev);
  await ctx.loraPage();
  assert.match(text(main), /Packets42/);

  statusCount = 43;
  await buttonNamed(main, 'Refresh').click();
  await settle();
  assert.match(text(main), /Packets43/);
});

test('LoRa reset counter and save handle API error replies', async () => {
  const failDevice = {
    'GET /api/settings': () => reply(200, LORA_SETTINGS),
    'GET /api/status': () => reply(200, LORA_STATUS),
    'POST /api/lora/reset': () => reply(500, { error: 'radio busy' }),
    'PATCH /api/settings': () => reply(400, { error: 'frequency rejected' }),
  };
  const { ctx, main } = await load(failDevice);
  await ctx.loraPage();

  await buttonNamed(main, 'Reset counter').click();
  await settle();
  assert.match(text(main), /Radio busy/i);

  await buttonNamed(main, 'Save').click();
  await settle();
  assert.match(text(main), /Frequency rejected/i);
});

test('LoRa preset buttons update form controls', async () => {
  const patches = [];
  const { ctx, main } = await load(loraDevice(patches));
  await ctx.loraPage();

  await buttonNamed(main, 'MeshCore EU868').click();
  assert.equal(control(main, 'Frequency (MHz)').value, '868.000');
  assert.equal(control(main, 'Spreading factor').value, '11');
  assert.equal(control(main, 'Bandwidth').value, '250');
  assert.equal(control(main, 'Coding rate').value, '5');
  assert.equal(control(main, 'Sync word').value, '1424');

  await buttonNamed(main, 'Meshtastic EU868 LongFast').click();
  assert.equal(control(main, 'Frequency (MHz)').value, '869.525');
  assert.equal(control(main, 'Spreading factor').value, '11');
  assert.equal(control(main, 'Bandwidth').value, '250');
  assert.equal(control(main, 'Coding rate').value, '5');
  assert.equal(control(main, 'Sync word').value, '2B24');

  await buttonNamed(main, 'EU868 Default').click();
  assert.equal(control(main, 'Frequency (MHz)').value, '868.000');
  assert.equal(control(main, 'Spreading factor').value, '7');
  assert.equal(control(main, 'Bandwidth').value, '250');
  assert.equal(control(main, 'Coding rate').value, '5');
  assert.equal(control(main, 'Sync word').value, '1424');
});

test('changing LoRa settings saves via PATCH /api/settings', async () => {
  const patches = [];
  const { ctx, main } = await load(loraDevice(patches));
  await ctx.loraPage();

  checkNamed(main, 'Enabled').checked = false;
  await type(control(main, 'Frequency (MHz)'), '915.000');
  await change(control(main, 'Spreading factor'), '10');
  await change(control(main, 'Bandwidth'), '125');
  await change(control(main, 'Coding rate'), '7');
  await type(control(main, 'Sync word'), '0x2B24');
  checkNamed(main, 'Wake from sleep on packet').checked = true;

  await buttonNamed(main, 'Save').click();
  await settle();

  assert.match(text(main), /Saved\./);
  assert.deepEqual(patches.at(-1), {
    schema: 1,
    lora: {
      enabled: false,
      ble_pin: 0,
      freq_khz: 915000,
      sf: 10,
      bw_khz: 125,
      bw_hz: 125000,
      cr: 7,
      tx_power_dbm: 22,
      sync_word: 0x2b24,
      rx_boosted: true,
      cad_enabled: false,
      client_repeat: false,
      path_hash_mode: 0,
      airtime_factor: 1,
      airtime_factor_x10: 10,
      wake_on_rx: true,
      share_loc: false,
      node_name: 'NM-EPD-420',
      channel_name: 'Public',
    },
  });
  assert.equal('channel_psk' in patches.at(-1).lora, false);
});

test('LoRa settings form validates frequency and sync word', async () => {
  const patches = [];
  const { ctx, main } = await load(loraDevice(patches));
  await ctx.loraPage();

  await type(control(main, 'Frequency (MHz)'), '300.000');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 0);
  assert.match(text(main), /Frequency must be between 430 and 928 MHz/);

  await type(control(main, 'Frequency (MHz)'), '868.000');
  await type(control(main, 'Sync word'), 'NOT_HEX');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 0);
  assert.match(text(main), /Sync word must be a valid hex value/);

  await type(control(main, 'Sync word'), '12345');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 0);
  assert.match(text(main), /Sync word must be a valid hex value/);
});

test('LoRa boundary values, hex variations and missing settings/status', async () => {
  const patches = [];
  const bareDevice = {
    'GET /api/settings': () => reply(200, { schema: 1 }),
    'GET /api/status': () => reply(200, { device: { id: 'reflbo-bb94', firmware: '0.4.0', uptime_s: 10, free_heap: 1000000 },
                                          preset: { name: 'Home', cycle: false }, time: { valid: true, epoch: 1000, local: '2026-10-01T12:00:00' },
                                          battery: {}, wifi: { state: 'off' }, sensors: {} }),
    'PATCH /api/settings': (init) => { patches.push(JSON.parse(init.body)); return reply(200, { schema: 1 }); },
  };
  const { ctx, main } = await load(bareDevice);
  await ctx.loraPage();

  assert.equal(control(main, 'Frequency (MHz)').value, '868.000');
  assert.equal(control(main, 'Sync word').value, '1424');
  assert.match(text(main), /StateOff/);
  assert.match(text(main), /Packets0/);
  assert.match(text(main), /Last packetnone yet/);
  assert.match(text(main), /RSSI \/ SNR—/);

  await type(control(main, 'Frequency (MHz)'), '430.000');
  await type(control(main, 'Sync word'), '0');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.at(-1).lora.freq_khz, 430000);
  assert.equal(patches.at(-1).lora.sync_word, 0);

  await type(control(main, 'Frequency (MHz)'), '928.000');
  await type(control(main, 'Sync word'), 'FFFF');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.at(-1).lora.freq_khz, 928000);
  assert.equal(patches.at(-1).lora.sync_word, 65535);

  await type(control(main, 'Frequency (MHz)'), '429.999');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 2);

  await type(control(main, 'Frequency (MHz)'), '928.001');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 2);
});

test('LoRa handles string sync_word in settings and formats age/signal metrics', async () => {
  const hexDevice = {
    'GET /api/settings': () => reply(200, {
      schema: 1,
      lora: { freq_khz: 868000, sync_word: '0x2B24' },
    }),
    'GET /api/status': () => reply(200, {
      device: { id: 'reflbo-bb94', firmware: '0.4.0', uptime_s: 3600, free_heap: 4194304 },
      preset: { name: 'Home', cycle: false },
      time: { valid: true, epoch: 1790880000, local: '2026-10-01T12:00:00' },
      battery: {}, wifi: { state: 'off' }, sensors: {},
      lora: {
        enabled: true,
        initialized: true,
        packet_count: 5,
        last_packet_at: 1790879850,
        age_s: 150,
        last_rssi_dbm: -80,
        last_snr_db: 7,
      },
    }),
  };
  const { ctx, main } = await load(hexDevice);
  await ctx.loraPage();
  assert.equal(control(main, 'Sync word').value, '2B24');
  assert.match(text(main), /2 min ago/);
  assert.match(text(main), /-80 dBm \/ 7 dB/);
});

test('quick radio ON/OFF toggle button (#lora-toggle-btn) updates settings and status', async () => {
  const patches = [];
  const { ctx, main } = await load(loraDevice(patches));
  ctx.location.hash = '#lora';
  ctx.route();
  await settle();

  const toggleBtn = below(main).find((e) => e.attrs?.id === 'lora-toggle-btn' || e.id === 'lora-toggle-btn');
  assert.ok(toggleBtn, 'toggle button exists');
  assert.equal(text(toggleBtn), 'Turn LoRa Off');

  await toggleBtn.click();
  await settle();

  assert.equal(patches.length, 1);
  assert.equal(patches[0].lora.enabled, false);
  assert.equal(text(toggleBtn), 'Turn LoRa On');
});

test('applying MeshCore EU/UK Narrow preset saves all MeshCore fields', async () => {
  const patches = [];
  const { ctx, main } = await load(loraDevice(patches));
  await ctx.loraPage();

  await buttonNamed(main, 'MeshCore EU/UK Narrow').click();
  assert.equal(control(main, 'Frequency (MHz)').value, '869.618');
  assert.equal(control(main, 'Spreading factor').value, '8');
  assert.equal(control(main, 'Bandwidth').value, '62.5');
  assert.equal(control(main, 'Coding rate').value, '5');
  assert.equal(control(main, 'Sync word').value, '1424');
  assert.equal(control(main, 'TX power (dBm)').value, '22');

  await type(control(main, 'Node name'), 'TestNode');
  await type(control(main, 'Channel name'), 'CustomCh');
  await type(control(main, 'Channel PSK (Base64)'), 'dGVzdGtleTEyMzQ1Njc4OQ==');
  await buttonNamed(main, 'Save').click();
  await settle();

  assert.equal(patches.length, 1);
  const p = patches[0].lora;
  assert.equal(p.freq_khz, 869618);
  assert.equal(p.sf, 8);
  assert.equal(p.bw_khz, 62.5);
  assert.equal(p.bw_hz, 62500);
  assert.equal(p.cr, 5);
  assert.equal(p.tx_power_dbm, 22);
  assert.equal(p.sync_word, 0x1424);
  assert.equal(p.node_name, 'TestNode');
  assert.equal(p.channel_name, 'CustomCh');
  assert.equal(p.channel_psk, 'dGVzdGtleTEyMzQ1Njc4OQ==');
  assert.equal(p.rx_boosted, true);
  assert.equal(p.cad_enabled, false);
  assert.equal(p.client_repeat, false);
  assert.equal(p.path_hash_mode, 0);
  assert.equal(p.airtime_factor, 1.0);
  assert.equal(p.share_loc, false);
  assert.equal(p.ble_pin, 0);
});

test('action buttons trigger advert, discover, and reset endpoints', async () => {
  const actionsCalled = {};
  const resetCalls = [];
  const dev = loraDevice([], {}, {}, resetCalls, actionsCalled);
  const { ctx, main } = await load(dev);
  await ctx.loraPage();

  await buttonNamed(main, 'Send Advert (Flood)').click();
  await settle();
  assert.deepEqual(actionsCalled.advert, { flood: true });

  await buttonNamed(main, 'Send Advert (Zero-Hop)').click();
  await settle();
  assert.deepEqual(actionsCalled.advert, { flood: false });

  await buttonNamed(main, 'Discover Repeaters').click();
  await settle();
  assert.equal(actionsCalled.discover, true);

  await buttonNamed(main, 'Reset counter').click();
  await settle();
  assert.equal(resetCalls.length, 1);
});

test('sending a group channel chat message calls POST /api/lora/send', async () => {
  const actionsCalled = {};
  const dev = loraDevice([], {}, {}, [], actionsCalled);
  const { ctx, main } = await load(dev);
  await ctx.loraPage();

  const chatInput = below(main).find((e) => e.attrs?.id === 'lora-chat-input' || e.id === 'lora-chat-input');
  const chatSend = below(main).find((e) => e.attrs?.id === 'lora-chat-send' || e.id === 'lora-chat-send');
  assert.ok(chatInput, 'chat input exists');
  assert.ok(chatSend, 'chat send exists');

  assert.match(text(main), /Hello MeshCore/);

  await type(chatInput, 'Broadcast test message');
  await chatSend.click();
  await settle();

  assert.deepEqual(actionsCalled.send, { text: 'Broadcast test message' });
});

test('executing a MeshCore CLI command calls POST /api/lora/cli and displays output', async () => {
  const actionsCalled = {};
  const dev = loraDevice([], {}, {}, [], actionsCalled);
  const { ctx, main } = await load(dev);
  await ctx.loraPage();

  const cliInput = below(main).find((e) => e.attrs?.id === 'lora-cli-input' || e.id === 'lora-cli-input');
  const cliRun = below(main).find((e) => e.attrs?.id === 'lora-cli-run' || e.id === 'lora-cli-run');
  const cliOut = below(main).find((e) => e.attrs?.id === 'lora-cli-output' || e.id === 'lora-cli-output');
  assert.ok(cliInput, 'cli input exists');
  assert.ok(cliRun, 'cli run exists');
  assert.ok(cliOut, 'cli output exists');

  await type(cliInput, 'get radio');
  await cliRun.click();
  await settle();

  assert.deepEqual(actionsCalled.cli, { cmd: 'get radio' });
  assert.match(text(cliOut), /CLI output for get radio/);

  await buttonNamed(main, 'stats-radio').click();
  await settle();
  assert.deepEqual(actionsCalled.cli, { cmd: 'stats-radio' });
});

test('navigating to #lora displays companion BLE status with PIN and state in #lora-live-status', async () => {
  const { ctx, main } = await load(loraDevice([]));
  await ctx.loraPage();

  const liveStatus = below(main).find((e) => e.attrs?.id === 'lora-live-status' || e.id === 'lora-live-status');
  assert.ok(liveStatus, '#lora-live-status element exists');
  const txt = text(liveStatus);
  assert.match(txt, /Companion BLE/);
  assert.match(txt, /Advertising/);
  assert.match(txt, /MeshCore-NM-EPD-420/);
  assert.match(txt, /482915/);

  /* Now test with connected state */
  const connectedDevice = loraDevice([], {}, { lora: { ble_connected: true, node_name: 'CustomNode', ble_pin: 123456 } });
  const connPage = await load(connectedDevice);
  await connPage.ctx.loraPage();
  const connLive = below(connPage.main).find((e) => e.attrs?.id === 'lora-live-status' || e.id === 'lora-live-status');
  const connTxt = text(connLive);
  assert.match(connTxt, /Connected/);
  assert.match(connTxt, /MeshCore-CustomNode/);
  assert.match(connTxt, /123456/);
});

test('LoRa settings form contains #f-lora-ble-pin and #btn-lora-gen-pin and saves custom PIN', async () => {
  const patches = [];
  const { ctx, main } = await load(loraDevice(patches));
  await ctx.loraPage();

  const pinInput = below(main).find((e) => e.attrs?.id === 'f-lora-ble-pin' || e.id === 'f-lora-ble-pin');
  const genBtn = below(main).find((e) => e.attrs?.id === 'btn-lora-gen-pin' || e.id === 'btn-lora-gen-pin');
  assert.ok(pinInput, '#f-lora-ble-pin exists');
  assert.ok(genBtn, '#btn-lora-gen-pin exists');

  await type(pinInput, '654321');
  await buttonNamed(main, 'Save').click();
  await settle();

  assert.match(text(main), /Saved\./);
  assert.equal(patches.at(-1).lora.ble_pin, 654321);
});

test('clicking #btn-lora-gen-pin generates a valid 6-digit random PIN', async () => {
  const { ctx, main } = await load(loraDevice([]));
  await ctx.loraPage();

  const pinInput = below(main).find((e) => e.attrs?.id === 'f-lora-ble-pin' || e.id === 'f-lora-ble-pin');
  const genBtn = below(main).find((e) => e.attrs?.id === 'btn-lora-gen-pin' || e.id === 'btn-lora-gen-pin');
  await genBtn.click();

  const generated = pinInput.value;
  assert.match(generated, /^[1-9]\d{5}$/, 'generated PIN is 6 digits');
});

test('LoRa settings form validates BLE PIN out-of-range', async () => {
  const patches = [];
  const { ctx, main } = await load(loraDevice(patches));
  await ctx.loraPage();

  const pinInput = below(main).find((e) => e.attrs?.id === 'f-lora-ble-pin' || e.id === 'f-lora-ble-pin');
  await type(pinInput, '12345'); /* only 5 digits */
  await buttonNamed(main, 'Save').click();
  await settle();

  assert.equal(patches.length, 0);
  assert.match(text(main), /BLE PIN must be 0/);
});

test('a preset on the LoRa layout has no slots to fill', async () => {
  const catalogue = { ...CATALOGUE, layouts: [...CATALOGUE.layouts, { id: 'lora', slots: [] }] };
  const doc = { schema: 1, active: 'lora', presets: [{ id: 'lora', name: 'LoRa Node', layout: 'lora', in_cycle: false,
                                                          slots: {}, options: {} }],
                cycle: { enabled: false, interval_s: 60 }, schedule: { enabled: false, entries: [] } };
  const { ctx, main } = await load({
    'GET /api/layouts': () => reply(200, catalogue), 'GET /api/presets': () => reply(200, doc),
    'GET /api/fields': () => reply(200, FIELDS), 'POST /api/preview.bmp': () => reply(200, 'BM', 'image/bmp'),
    'GET /api/settings': () => reply(200, STEP_SETTINGS),
  });
  await ctx.presetsPage();
  assert.match(text(main), /LoRa MeshCore dashboard/);
  assert.match(text(main), /LoRa Node/);
});

test('the Device page no longer renders Refresh rate and omits lpm_hz on Save', async () => {
  const patches = [];
  const { ctx, main } = await load(settingsDevice(patches));
  await ctx.devicePage();
  assert.throws(() => control(main, 'Refresh rate'), /no field "Refresh rate"/);
  control(main, 'Update every').value = '5';
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 1);
  assert.deepEqual(patches[0].display, { update_min: 5 });
  assert.equal('lpm_hz' in patches[0].display, false);
});

test('LoRa Card 4 initializes fractional bw_hz and sends schema: 1 on Save', async () => {
  for (const [bwHz, expectedVal] of [[62500, '62.5'], [31250, '31.25']]) {
    const patches = [];
    const { ctx, main } = await load(loraDevice(patches, { lora: { bw_hz: bwHz } }));
    await ctx.loraPage();
    assert.equal(control(main, 'Bandwidth').value, expectedVal);
    await buttonNamed(main, 'Save').click();
    await settle();
    assert.equal(patches.at(-1).schema, 1);
  }
});

test('executing a MeshCore CLI command refreshes /api/status in #lora', async () => {
  let statusGets = 0;
  let pktCount = 10;
  const dev = {
    'GET /api/settings': () => reply(200, LORA_SETTINGS),
    'GET /api/status': () => {
      statusGets += 1;
      return reply(200, { ...LORA_STATUS, lora: { ...LORA_STATUS.lora, packet_count: pktCount } });
    },
    'POST /api/lora/cli': (init) => {
      pktCount = 99;
      return reply(200, { ok: true, reply: `ran ${JSON.parse(init.body).cmd}` });
    },
  };
  const { ctx, main } = await load(dev);
  await ctx.loraPage();
  assert.equal(statusGets, 1);
  assert.match(text(main), /Packets10/);

  const cliInput = below(main).find((e) => e.attrs?.id === 'lora-cli-input' || e.id === 'lora-cli-input');
  const cliRun = below(main).find((e) => e.attrs?.id === 'lora-cli-run' || e.id === 'lora-cli-run');
  await type(cliInput, 'set af 1.5');
  await cliRun.click();
  await settle();

  assert.equal(statusGets, 2);
  assert.match(text(main), /Packets99/);
});

test('LoRa Card 4 write-only Channel PSK omits secret when untouched and supports Use Public PSK', async () => {
  const patches = [];
  const customDev = loraDevice(patches, {
    lora: {
      channel_name: 'TeamCh',
      keys: { psk: true, channel_psk: true },
      is_public_psk: false,
    },
  });
  const { ctx, main } = await load(customDev);
  await ctx.loraPage();

  const pskInput = control(main, 'Channel PSK (Base64)');
  assert.equal(pskInput.attrs.type, 'password');
  assert.equal(pskInput.value, '');
  assert.match(text(main), /A key is set\./);

  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 1);
  assert.equal('channel_psk' in patches[0].lora, false);

  await buttonNamed(main, 'Use Public PSK').click();
  assert.equal(control(main, 'Channel name').value, 'Public');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.length, 2);
  assert.equal(patches[1].lora.channel_name, 'Public');
  assert.equal(patches[1].lora.channel_psk, null);
});

test('LoRa live status shows legacy_identity warning and Reset identity calls POST /api/lora/rekey', async () => {
  const actionsCalled = {};
  const devDenied = loraDevice([], {}, { lora: { legacy_identity: true } }, [], actionsCalled);
  const denied = await load(devDenied, { confirm: () => false });
  await denied.ctx.loraPage();

  assert.match(text(denied.main), /Legacy MAC-derived identity \(reset recommended\)/);
  const rekeyDeniedBtn = below(denied.main).find((e) => e.attrs?.id === 'btn-lora-rekey' || e.id === 'btn-lora-rekey');
  assert.ok(rekeyDeniedBtn, '#btn-lora-rekey exists');
  await rekeyDeniedBtn.click();
  await settle();
  assert.equal(actionsCalled.rekey, undefined);

  const actionsOk = {};
  const devOk = loraDevice([], {}, { lora: { legacy_identity: true } }, [], actionsOk);
  const { ctx, main } = await load(devOk, { confirm: () => true });
  await ctx.loraPage();
  assert.match(text(main), /Legacy MAC-derived identity \(reset recommended\)/);

  const rekeyBtn = below(main).find((e) => e.attrs?.id === 'btn-lora-rekey' || e.id === 'btn-lora-rekey');
  await rekeyBtn.click();
  await settle();

  assert.equal(actionsOk.rekey, 1);
  assert.equal(/Legacy MAC-derived identity/.test(text(main)), false);
  assert.match(text(main), /fedcba9876543210/);
  assert.match(text(main), /765432/);
});

test('LoRa multi-channel tabs switch composer placeholder and send channel payload', async () => {
  const actionsCalled = {};
  const dev = loraDevice([], {}, {
    lora: {
      channels: [
        { index: 0, enabled: true, name: 'Public' },
        { index: 1, enabled: true, name: 'TeamOps' },
      ],
    },
  }, [], actionsCalled);
  const { ctx, main } = await load(dev);
  await ctx.loraPage();

  const ch1Tab = below(main).find((e) => e.tag === 'button' && text(e) === '#1 TeamOps');
  assert.ok(ch1Tab, 'channel #1 tab exists');
  await ch1Tab.click();
  await settle();

  const chatInput = below(main).find((e) => e.attrs?.id === 'lora-chat-input' || e.id === 'lora-chat-input');
  const chatSend = below(main).find((e) => e.attrs?.id === 'lora-chat-send' || e.id === 'lora-chat-send');
  assert.match(chatInput.placeholder || '', /TeamOps|#1/);

  await type(chatInput, 'Team ping');
  await chatSend.click();
  await settle();
  assert.deepEqual(actionsCalled.send, { text: 'Team ping', channel: 1 });

  const allTab = below(main).find((e) => e.tag === 'button' && text(e) === 'All');
  assert.ok(allTab, 'All tab exists');
  await allTab.click();
  await settle();

  await type(chatInput, 'Back to public');
  await chatSend.click();
  await settle();
  assert.deepEqual(actionsCalled.send, { text: 'Back to public' });
});

test('LoRa contacts lifecycle: Advert + Contact, URI import, Reset path, Remove, and Haversine distance', async () => {
  const actionsCalled = {};
  const pub64 = 'aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899';
  const dev = loraDevice([], { location: { lat: 50.0755, lon: 14.4378 } }, {
    location: { lat: 50.0755, lon: 14.4378 },
    lora: {
      contacts: [
        {
          pub_key: pub64,
          name: 'Bob',
          type: 1,
          out_path_len: 2,
          lat: 49.1951,
          lon: 16.6068,
          age_s: 30,
        },
      ],
    },
  }, [], actionsCalled);
  const { ctx, main } = await load(dev);
  await ctx.loraPage();

  assert.match(text(main), /Saved Contacts \(1 \/ 64\)/);
  assert.match(text(main), /km [NESW]/);

  // + Contact on Recent Adverts
  await buttonNamed(main, '+ Contact').click();
  await settle();
  assert.deepEqual(actionsCalled.contacts, {
    action: 'save',
    pub_key: 'aabbccddeeff0011',
    name: 'Repeater-1',
    type: 2,
    lat: 49.1951,
    lon: 16.6068,
  });

  // + Add / Import Contact via meshcore:// URI
  await buttonNamed(main, '+ Add / Import Contact').click();
  const aliceHex = '1122334455667788990011223344556677889900112233445566778899001122';
  const uriInput = below(main).find((e) => e.tag === 'input' && e.attrs?.placeholder === 'Paste meshcore://… URI');
  assert.ok(uriInput, 'URI import input exists');
  uriInput.value = `meshcore://${aliceHex}?name=Alice&type=1&lat=50.0&lon=20.0`;
  await Promise.all((uriInput.listeners.input || []).map((fn) => fn({ target: uriInput })));

  await buttonNamed(main, 'Save contact').click();
  await settle();
  assert.deepEqual(actionsCalled.contacts, {
    action: 'save',
    pub_key: aliceHex,
    name: 'Alice',
    type: 1,
    lat: 50,
    lon: 20,
  });

  // Reset path & Remove on saved contact
  await buttonNamed(main, 'Reset path').click();
  await settle();
  assert.deepEqual(actionsCalled.contacts, { action: 'reset_path', pub_key: pub64 });

  await buttonNamed(main, 'Remove').click();
  await settle();
  assert.deepEqual(actionsCalled.contacts, { action: 'delete', pub_key: pub64 });
});

test('LoRa Direct Messaging, ACK lifecycle states, and Retry (Flood)', async () => {
  const actionsCalled = {};
  const peerPub = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff';
  const dev = loraDevice([], {}, {
    lora: {
      contacts: [{ pub_key: peerPub, name: 'Alice', type: 1, age_s: 10 }],
      messages: [
        { direct: true, outgoing: true, peer_pub: peerPub, sender: 'Me', text: 'Ping 1', ack: 1, age_s: 5 },
        { direct: true, outgoing: true, peer_pub: peerPub, sender: 'Me', text: 'Ping 2', ack: 2, ack_rtt_ms: 340, age_s: 4 },
        { direct: true, outgoing: true, peer_pub: peerPub, sender: 'Me', text: 'Ping 3', ack: 3, age_s: 3 },
      ],
    },
  }, [], actionsCalled);
  const { ctx, main } = await load(dev);
  await ctx.loraPage();

  const txt = text(main);
  assert.match(txt, /Waiting ACK…/);
  assert.match(txt, /Delivered \(340 ms\)/);
  assert.match(txt, /No ACK/);

  // Open DM tab from Saved Contacts Message button
  await buttonNamed(main, 'Message').click();
  await settle();

  const chatInput = below(main).find((e) => e.attrs?.id === 'lora-chat-input' || e.id === 'lora-chat-input');
  const chatSend = below(main).find((e) => e.attrs?.id === 'lora-chat-send' || e.id === 'lora-chat-send');
  await type(chatInput, 'Secret DM');
  await chatSend.click();
  await settle();
  assert.deepEqual(actionsCalled.send, { text: 'Secret DM', to: peerPub, force_flood: false });

  // Retry (Flood) on ack: 3
  await buttonNamed(main, 'Retry (Flood)').click();
  await settle();
  assert.deepEqual(actionsCalled.send, { text: 'Ping 3', to: peerPub, force_flood: true });
});

test('LoRa channels manager, Clear history, composer meta, copy URI, and route() cleanup', async () => {
  const actionsCalled = {};
  const dev = loraDevice([], { location: { lat: 50.0755, lon: 14.4378 } }, {
    location: { lat: 50.0755, lon: 14.4378 },
  }, [], actionsCalled);
  const { ctx, main } = await load(dev);
  await ctx.loraPage();

  // Channel slot #1 Save slot
  const chTable = below(main).find((e) => e.attrs?.id === 'lora-channels-table' || e.id === 'lora-channels-table');
  assert.ok(chTable, '#lora-channels-table exists');
  const tbody = chTable.children.find((c) => c.tag === 'tbody');
  const row1 = tbody.children[1];
  const ch1Enable = row1.children[1].children[0];
  const ch1Name = row1.children[2].children[0];
  const ch1Mode = row1.children[3].children[0];
  const ch1Key = row1.children[4].children[0];
  ch1Enable.checked = true;
  ch1Name.value = 'Ops';
  ch1Mode.value = 'passphrase';
  ch1Key.value = 'secret-pass';
  await buttonNamed(row1, 'Save slot').click();
  await settle();
  assert.deepEqual(actionsCalled.channels, {
    index: 1,
    enabled: true,
    name: 'Ops',
    key_mode: 'passphrase',
    key: 'secret-pass',
  });

  // Live UTF-8 byte counter & airtime estimate
  const chatInput = below(main).find((e) => e.attrs?.id === 'lora-chat-input' || e.id === 'lora-chat-input');
  chatInput.value = 'Ahoj světe';
  await Promise.all((chatInput.listeners.input || []).map((fn) => fn({ target: chatInput })));
  const metaEl = below(main).find((e) => (e.className || '').includes('composer-meta'));
  assert.ok(metaEl, '.composer-meta exists');
  assert.match(text(metaEl), /11 \/ 120 B/);
  assert.match(text(metaEl), /Est\. airtime: ~\d+ ms @ SF7\/BW250/);

  // Clear history
  await buttonNamed(main, 'Clear history').click();
  await settle();
  assert.equal(actionsCalled.clearMessages, 1);

  // Copy meshcore:// URI button
  const copyBtn = below(main).find((e) => e.attrs?.id === 'lora-btn-copy-uri' || e.id === 'lora-btn-copy-uri');
  assert.ok(copyBtn, '#lora-btn-copy-uri exists');
  await copyBtn.click();

  // Navigating away from #lora to #status restores document.title = 'reflbo'
  ctx.document.title = '(2) reflbo';
  ctx.location.hash = '#status';
  ctx.route();
  await settle();
  assert.equal(ctx.document.title, 'reflbo');
});

test('LoRa 2B/3B out_path_len decoding, Sensor URI import, and Clear history cancel guard', async () => {
  const actionsCalled = {};
  const pub2b = '1111111111111111111111111111111111111111111111111111111111111111';
  const pub3b = '2222222222222222222222222222222222222222222222222222222222222222';
  const pubSens = '4444444444444444444444444444444444444444444444444444444444444444';
  const dev = loraDevice([], {}, {
    lora: {
      msg_seq: 5,
      advert_seq: 3,
      contacts: [
        { pub_key: pub2b, name: 'TwoBytePeer', type: 1, out_path_len: 0x42, age_s: 12 },
        { pub_key: pub3b, name: 'ThreeBytePeer', type: 2, out_path_len: -126, age_s: 20 }, /* 0x82 as int8_t */
      ],
    },
  }, [], actionsCalled);

  const { ctx, main } = await load(dev, { confirm: () => false });
  await ctx.loraPage();

  const txt = text(main);
  assert.match(txt, /DIRECT \(2 hops, 2B\)/);
  assert.match(txt, /DIRECT \(2 hops, 3B\)/);

  // Cancel guard on Clear history must prevent API call
  await buttonNamed(main, 'Clear history').click();
  await settle();
  assert.equal(actionsCalled.clearMessages || 0, 0);

  // Import Sensor (type=4) via meshcore:// URI
  await buttonNamed(main, '+ Add / Import Contact').click();
  const uriIn = control(main, 'meshcore:// URI');
  await type(uriIn, `meshcore://${pubSens}?name=RoofSensor&type=4`);
  await buttonNamed(main, 'Save contact').click();
  await settle();
  assert.deepEqual(actionsCalled.contacts, {
    action: 'save',
    pub_key: pubSens,
    name: 'RoofSensor',
    type: 4,
  });
});

test('LoRa Priority 2 UX: neighbor name resolution, contact search filter, and dynamic chat header', async () => {
  const pubRpt = '44625f6c316df85d000000000000000000000000000000000000000000000000';
  const pubPeer = '969692f111111111111111111111111111111111111111111111111111111111';
  const dev = loraDevice([], {}, {
    lora: {
      channel_name: 'Public',
      channel_hash: 0x11,
      channels: [
        { index: 0, enabled: true, name: 'Public', channel_hash: 0x11 },
        { index: 1, enabled: true, name: '#test', channel_hash: 0x9a },
      ],
      neighbors: [
        { name: '', node_type: 2, pub_key: '44625f6c316df85d', snr_in_db: 11.25, snr_out_db: 9.5, rssi_dbm: -82, age_s: 15 },
      ],
      contacts: [
        { pub_key: pubRpt, name: 'SQ7ECZ-P1', type: 2, out_path_len: 0x40, age_s: 30 },
        { pub_key: pubPeer, name: 'WG Sobolew Most', type: 2, out_path_len: 0x43, age_s: 60 },
      ],
    },
  });

  const { ctx, main } = await load(dev);
  await ctx.loraPage();

  // 1. Neighbor prefix 44625f6c316df85d resolves friendly name SQ7ECZ-P1 from contacts
  assert.match(text(main), /SQ7ECZ-P1\s*44625f6c\s*Repeater/);
  assert.match(text(main), /SNR Out 9\.5 dB · In 11\.25 dB/);

  // 2. Dynamic chat header updates across All -> #1 #test -> @DM
  assert.match(text(main), /All conversations · Primary #0: Public \(0x11\)/);
  await buttonNamed(main, '#1 #test').click();
  await settle();
  assert.match(text(main), /Channel #1: #test \(0x9A\) · AES-128/);

  // 3. Contact filter narrows list and updates Saved Contacts counter
  const filterIn = below(main).find((e) => e.attrs?.id === 'lora-contact-filter' || e.id === 'lora-contact-filter');
  assert.ok(filterIn, '#lora-contact-filter exists');
  await type(filterIn, 'sobolew');
  assert.match(text(main), /Saved Contacts \(1 \/ 2 of 64\)/);
  const contactsBox = below(main).find((e) => e.attrs?.id === 'lora-contacts-box' || e.id === 'lora-contacts-box');
  assert.match(text(contactsBox), /WG Sobolew Most/);
  assert.doesNotMatch(text(contactsBox), /SQ7ECZ-P1/);

  // Opening DM from filtered contact shows dynamic DM header with 2B route
  await buttonNamed(contactsBox, 'Message').click();
  await settle();
  assert.match(text(main), /Direct Message to @WG Sobolew Most · E2E X25519 · Route: DIRECT \(3 hops, 2B\)/);
});

test('LoRa P1.3 noise_floor_dbm: 0 displays dash and full channel names in chat badges & Export .txt', async () => {
  const peerPub = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff';
  const dev = loraDevice([], {}, {
    lora: {
      noise_floor_dbm: 0,
      channel_name: 'Public',
      channels: [
        { index: 0, enabled: true, name: 'Public' },
        { index: 1, enabled: true, name: 'Ops' },
        { index: 2, enabled: true, name: '#bot' },
      ],
      messages: [
        { channel: 0, outgoing: false, sender: 'Alice', text: 'Hi public', age_s: 10 },
        { channel: 1, outgoing: false, sender: 'Bob', text: 'Ops check', age_s: 8 },
        { channel: 2, outgoing: false, sender: 'BotUser', text: 'Bot ping', age_s: 5 },
        { direct: true, outgoing: true, peer_pub: peerPub, sender: 'Me', text: 'Secret DM', age_s: 2 },
      ],
    },
  });

  const { ctx, blobs, main } = await load(dev);
  await ctx.loraPage();

  // 1. noise_floor_dbm: 0 shows '—' (not '0 dBm') in #lora-live-status
  const liveStatus = below(main).find((e) => e.attrs?.id === 'lora-live-status' || e.id === 'lora-live-status');
  assert.ok(liveStatus, '#lora-live-status exists');
  assert.match(text(liveStatus), /Noise floor\s*—/);
  assert.doesNotMatch(text(liveStatus), /0 dBm/);

  // 2. Chat badges: channel 0 has 'Public'; channel 1 has 'Ops'; channel 2 has '#bot'; DM has 'DM'
  const chatBox = below(main).find((e) => e.attrs?.id === 'lora-chat-box' || e.id === 'lora-chat-box');
  assert.ok(chatBox, '#lora-chat-box exists');
  const badges = below(chatBox).filter((e) => e.tag === 'span' && (e.className || '').includes('badge'));
  const badgeTexts = badges.map((b) => text(b));
  assert.deepEqual(badgeTexts, ['Public', 'Ops', '#bot', 'DM']);

  // 3. Export .txt: includes [Public], [Ops], [#bot], [DM]
  await buttonNamed(main, 'Export .txt').click();
  assert.equal(blobs.length, 1);
  const exported = await blobs[0].text();
  assert.doesNotMatch(exported, /\[#0\]/);
  assert.match(exported, /\[RX\] \[Public\] Alice: Hi public/);
  assert.match(exported, /\[RX\] \[Ops\] Bob: Ops check/);
  assert.match(exported, /\[RX\] \[#bot\] BotUser: Bot ping/);
  assert.match(exported, /\[TX\] \[DM\] Me: Secret DM/);
});

test('LoRa P1.2 messages without .text (having sender + body) render and export identically', async () => {
  const dev = loraDevice([], {}, {
    lora: {
      channel_name: 'Public',
      channels: [
        { index: 0, enabled: true, name: 'Public' },
        { index: 1, enabled: true, name: '#bot' },
        { index: 2, enabled: true, name: '#test' },
      ],
      messages: [
        { channel: 0, outgoing: false, sender: 'Alice', body: 'Hello Public', age_s: 12 },
        { channel: 1, outgoing: false, sender: 'BotUser', body: 'Bot status', age_s: 8 },
        { channel: 2, outgoing: true, sender: 'Me', body: 'Test ping', age_s: 4 },
      ],
    },
  });

  const { ctx, blobs, main } = await load(dev);
  await ctx.loraPage();

  const chatBox = below(main).find((e) => e.attrs?.id === 'lora-chat-box' || e.id === 'lora-chat-box');
  assert.ok(chatBox, '#lora-chat-box exists');
  const badges = below(chatBox).filter((e) => e.tag === 'span' && (e.className || '').includes('badge'));
  assert.deepEqual(badges.map((b) => text(b)), ['Public', '#bot', '#test']);
  assert.match(text(chatBox), /Alice/);
  assert.match(text(chatBox), /Hello Public/);
  assert.match(text(chatBox), /BotUser/);
  assert.match(text(chatBox), /Bot status/);
  assert.match(text(chatBox), /Me/);
  assert.match(text(chatBox), /Test ping/);

  await buttonNamed(main, 'Export .txt').click();
  assert.equal(blobs.length, 1);
  const exported = await blobs[0].text();
  assert.match(exported, /\[RX\] \[Public\] Alice: Hello Public/);
  assert.match(exported, /\[RX\] \[#bot\] BotUser: Bot status/);
  assert.match(exported, /\[TX\] \[#test\] Me: Test ping/);
});

test('LoRa Interactive Geo Map, /api/lora/map cache reuse, click hit-testing, and #lora-map-inspector actions', async () => {
  const actionsCalled = { mapFetches: 0, contacts: [] };
  const advKey = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';
  const noGpsKey = 'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc';
  const baseDev = loraDevice([], {}, {
    location: { lat: 52.2297, lon: 21.0122 },
    lora: {
      node_name: 'WarsawBase',
      pub_key: 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
      adverts: [
        {
          pub_key: advKey,
          name: 'AdvRepeater',
          node_type: 2,
          has_loc: true,
          lat: 52.25,
          lon: 21.05,
          rssi_dbm: -78,
          snr_db: 7.5,
          hops: 1,
          age_s: 15,
          rx_ts: 1700000000,
          adv_ts: 1699999990,
        },
        {
          pub_key: noGpsKey,
          name: 'NoGpsSensor',
          node_type: 4,
          has_loc: false,
          rssi_dbm: -92,
          snr_db: 3.0,
          hops: 0,
          age_s: 25,
        },
      ],
      contacts: [],
      neighbors: [],
    },
  }, [], actionsCalled);

  const dev = baseDev;
  dev['GET /api/lora/map'] = () => {
    actionsCalled.mapFetches += 1;
    return reply(200, {
      bounds: [51.5, 53.0, 20.0, 22.0],
      lines: [
        [0, 52.2, 21.0, 52.25, 21.05],
        [2, 52.1, 20.9, 52.3, 21.1],
      ],
      towns: [
        { name: 'Warszawa', lat: 52.2297, lon: 21.0122, pop: 1800000 },
        { name: 'Pruszków', lat: 52.17, lon: 20.80, pop: 65000 },
      ],
    });
  };
  dev['POST /api/lora/contacts'] = (init) => {
    actionsCalled.contacts.push(JSON.parse(init.body));
    return reply(200, { ok: true });
  };

  const { ctx, main } = await load(dev);
  const origCreate = ctx.document.createElement;
  const drawCalls = [];
  const pathCalls = [];
  const dashCalls = [];
  ctx.document.createElement = (tag) => {
    const el = origCreate(tag);
    if (tag === 'canvas') {
      el.getContext = () => ({
        save() {}, restore() {}, scale() {}, clearRect() {},
        beginPath() {},
        moveTo(x, y) { pathCalls.push(['moveTo', x, y]); },
        lineTo(x, y) { pathCalls.push(['lineTo', x, y]); },
        arc(x, y, r) { pathCalls.push(['arc', x, y, r]); },
        rect(x, y, w, h) { pathCalls.push(['rect', x, y, w, h]); },
        fillRect(x, y, w, h) { pathCalls.push(['fillRect', x, y, w, h]); },
        closePath() {},
        stroke() {}, fill() {},
        setLineDash(d) { dashCalls.push(d); },
        measureText(t) { return { width: String(t).length * 6 }; },
        strokeText(t, x, y) { drawCalls.push({ op: 'strokeText', t, x, y }); },
        fillText(t, x, y) { drawCalls.push({ op: 'fillText', t, x, y }); },
      });
      el.getBoundingClientRect = () => ({ left: 0, top: 0, width: 560, height: 360 });
    }
    return el;
  };

  await ctx.loraPage();
  await settle();

  assert.equal(actionsCalled.mapFetches, 1, 'initial /api/lora/map fetch');
  assert.ok(drawCalls.some((c) => c.t === 'Pruszków'), 'renders town label from /api/lora/map');
  assert.ok(pathCalls.every((p) => p.slice(1).every((n) => Number.isFinite(n))), 'zero NaN canvas path calls');
  assert.ok(!dashCalls.some((d) => Array.isArray(d) && d[0] === 4 && d[1] === 4), 'no dashed outer circle in Geo Map mode');
  assert.ok(!drawCalls.some((c) => c.t === 'NoGpsSensor'), 'no-GPS node is not drawn on Geo Map canvas');

  const noGpsStrip = below(main).find((e) => (e.className || e.attrs?.class || '').includes('map-nogps-strip'));
  assert.ok(noGpsStrip && !noGpsStrip.hidden, '.map-nogps-strip is visible');
  const noGpsBtn = buttonNamed(noGpsStrip, 'NoGpsSensor');
  assert.ok(noGpsBtn, 'NoGpsSensor chip exists in .map-nogps-strip');
  await noGpsBtn.click();
  await settle();
  const inspNoGps = below(main).find((e) => e.attrs?.id === 'lora-map-inspector' || e.id === 'lora-map-inspector');
  assert.match(text(inspNoGps), /NoGpsSensor/);

  // Local zoom in should reuse cached bounds without extra /api/lora/map fetch
  await buttonNamed(main, '+').click();
  await settle();
  assert.equal(actionsCalled.mapFetches, 1, 'reuses cached /api/lora/map vectors on local zoom');

  // Click at projected position of AdvRepeater (lat 52.25, lon 21.05)
  const canvas = below(main).find((e) => e.attrs?.id === 'lora-node-map' || e.id === 'lora-node-map');
  assert.ok(canvas, '#lora-node-map exists');
  await Promise.all((canvas.listeners.click || []).map((fn) => fn({ clientX: 305, clientY: 158 })));
  await settle();

  const inspector = below(main).find((e) => e.attrs?.id === 'lora-map-inspector' || e.id === 'lora-map-inspector');
  assert.ok(inspector, '#lora-map-inspector exists');
  const inspText = text(inspector);
  assert.match(inspText, /AdvRepeater/);
  assert.match(inspText, new RegExp(advKey));
  assert.match(inspText, /RSSI -78 dBm/);
  assert.match(inspText, /52\.25000, 21\.05000/);

  // + Contact action from inspector
  await buttonNamed(inspector, '+ Contact').click();
  await settle();
  assert.equal(actionsCalled.contacts.length, 1);
  assert.equal(actionsCalled.contacts[0].action, 'save');
  assert.equal(actionsCalled.contacts[0].pub_key, advKey);

  // Message action from inspector opens DM & focuses #lora-chat-input
  await buttonNamed(inspector, 'Message').click();
  await settle();
  const chatInput = below(main).find((e) => e.attrs?.id === 'lora-chat-input' || e.id === 'lora-chat-input');
  assert.match(chatInput.placeholder || chatInput.attrs.placeholder || '', /AdvRepeater/);
});

test('Etap 2: #lora sub-tabs, aria-current, colored badges, danger buttons, airtime comma, Channels 0-7 region, and 401 poll stop', async () => {
  const patches = [];
  let chatCalls = 0;
  let return401 = false;
  const dev = {
    ...loraDevice(patches),
    'GET /api/status': () => reply(200, {
      ...LORA_STATUS,
      lora: {
        ...LORA_STATUS.lora,
        contacts: [{ pub_key: 'aa'.repeat(32), name: 'AlphaRep', type: 2, out_path_len: 1 }],
      },
    }),
    'GET /api/lora/chat': () => {
      chatCalls++;
      if (return401) return reply(401, { error: 'unauthorized' });
      return reply(200, { unchanged: true, lora: LORA_STATUS.lora });
    },
  };
  const { ctx, main } = await load(dev);
  ctx.location.hash = '#lora';
  ctx.route();
  await settle();

  // 1. aria-current="page" on active nav link
  const navLinks = ctx.document.querySelectorAll('nav a');
  if (navLinks.length > 0) {
    const loraNav = navLinks.find((a) => a.getAttribute('href') === '#lora');
    assert.equal(loraNav.attrs['aria-current'], 'page');
    for (const a of navLinks) {
      if (a !== loraNav) assert.equal(a.attrs['aria-current'], undefined);
    }
  }

  // 2. #lora sub-tabs: 'All sections' default, non-collision with chat 'All'
  const subtabAll = buttonNamed(main, 'All sections');
  assert.equal(subtabAll.attrs['aria-selected'], 'true');
  assert.ok(buttonNamed(main, 'All'), 'chat All tab still exists');

  const subtabChat = buttonNamed(main, 'Chat');
  await subtabChat.click();
  assert.equal(subtabChat.attrs['aria-selected'], 'true');
  assert.equal(subtabAll.attrs['aria-selected'], 'false');

  // ArrowRight switches from Chat -> Map & Nodes
  await subtabChat.listeners.keydown[0]({ key: 'ArrowRight', preventDefault() {} });
  assert.equal(buttonNamed(main, 'Map & Nodes').attrs['aria-selected'], 'true');
  await buttonNamed(main, 'All sections').click();

  // 3. Colored contact badges (no 'type' class)
  const alphaBadge = below(main).find((e) => e.tag === 'span' && text(e) === 'Repeater' && (e.className || e.attrs?.class || '').includes('badge'));
  assert.ok(alphaBadge, 'contact role badge exists');
  assert.equal((alphaBadge.className || alphaBadge.attrs?.class || '').includes('type'), false);

  // 4. Danger buttons
  const rekeyBtn = buttonNamed(main, 'Reset identity');
  const clearHistBtn = buttonNamed(main, 'Clear history');
  assert.ok((rekeyBtn.className || rekeyBtn.attrs?.class || '').includes('danger'));
  assert.ok((clearHistBtn.className || clearHistBtn.attrs?.class || '').includes('danger'));

  // 5. airtime_factor comma normalization '1,5'
  await type(control(main, 'Airtime factor (0–9)'), '1,5');
  await buttonNamed(main, 'Save').click();
  await settle();
  assert.equal(patches.at(-1).lora.airtime_factor, 1.5);
  assert.equal(patches.at(-1).lora.airtime_factor_x10, 15);

  // 6. Channels 0-7 region & data-label
  const chWrap = below(main).find((e) => (e.className || e.attrs?.class || '').includes('channels-table-wrap'));
  assert.equal(chWrap.attrs.role, 'region');
  assert.equal(chWrap.attrs['aria-label'], 'Channels 0 to 7');
  assert.equal(chWrap.attrs.tabindex, '0');
  const firstTd = below(chWrap).find((e) => e.tag === 'td');
  assert.equal(firstTd.attrs['data-label'], '#');
});





