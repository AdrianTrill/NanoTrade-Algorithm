import { runtime } from './runtime.js';

// Where the engine runs. In the page, as WebAssembly in two workers: one answers the interactive backtests, the other
// runs sweeps, walk-forward tests, and resampling, so dragging a slider never waits behind a long job. Or on the native
// server, when `nanotrade serve` serves the page. Both answer the same JSON requests with the same bytes.

export class Superseded extends Error {
  constructor() {
    super('superseded by a newer request');
    this.name = 'Superseded';
  }
}

export class Cancelled extends Error {
  constructor() {
    super('cancelled');
    this.name = 'Cancelled';
  }
}

// One worker, with the datasets it has been given.
class WorkerHandle {
  constructor(onState) {
    this.worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
    this.calls = new Map();
    this.loaded = new Set();
    this.next = 1;
    this.ready = new Promise((resolve, reject) => {
      this.worker.addEventListener('message', ({ data }) => {
        if (data.type === 'ready') {
          this.version = data.version;
          resolve();
          onState?.('ready');
        } else if (data.type === 'failed') {
          reject(new Error(data.message));
        } else if (data.type === 'progress') {
          this.calls.get(data.id)?.progress?.(data.value);
        } else if (data.type === 'response') {
          const call = this.calls.get(data.id);
          this.calls.delete(data.id);
          call?.resolve(data.text);
        }
      });
      this.worker.addEventListener('error', (event) => reject(new Error(event.message || 'the worker failed')));
    });
  }

  call(text, progress) {
    const id = this.next++;
    return new Promise((resolve, reject) => {
      this.calls.set(id, { resolve, reject, progress });
      this.worker.postMessage({ id, text, progress: Boolean(progress) });
    });
  }

  terminate() {
    this.worker.terminate();
    for (const call of this.calls.values()) call.reject(new Cancelled());
    this.calls.clear();
  }
}

// Keeps only the newest request: one runs at a time, and a request still waiting is dropped for a newer one.
class Latest {
  constructor() {
    this.busy = false;
    this.waiting = null;
  }

  run(job) {
    this.waiting?.reject(new Superseded());
    return new Promise((resolve, reject) => {
      this.waiting = { job, resolve, reject };
      this.pump();
    });
  }

  pump() {
    if (this.busy || !this.waiting) return;
    const { job, resolve, reject } = this.waiting;
    this.waiting = null;
    this.busy = true;
    job().then(resolve, reject).finally(() => {
      this.busy = false;
      this.pump();
    });
  }
}

// The datasets a request names: its data, or a portfolio's markets.
function datasetsOf(request) {
  const names = typeof request.data === 'string' ? [request.data] : [];
  for (const market of request.portfolio?.markets ?? []) {
    const name = typeof market === 'string' ? market : market?.dataset;
    if (typeof name === 'string') names.push(name);
  }
  return names;
}

class WasmEngine {
  constructor(manifest) {
    this.runtime = 'wasm';
    this.manifest = manifest;
    this.sources = new Map();  // dataset name -> the text of its load request
    this.fetches = new Map();
    this.interactive = new WorkerHandle();
    this.batch = new WorkerHandle();
    this.latest = new Latest();
  }

  async start() {
    await Promise.all([this.interactive.ready, this.batch.ready]);
    this.version = this.interactive.version;
  }

  // The bundled datasets are fetched once, on first use, and handed to whichever worker needs them.
  source(name) {
    if (this.sources.has(name)) return Promise.resolve(this.sources.get(name));
    if (!this.fetches.has(name)) {
      const entry = this.manifest.datasets.find((d) => d.symbol === name);
      if (!entry) return Promise.resolve(null);
      this.fetches.set(name, fetch(`data/${entry.file}`).then((response) => {
        if (!response.ok) throw new Error(`can't fetch data/${entry.file}: ${response.status}`);
        return response.text();
      }).then((csv) => {
        const text = JSON.stringify({ command: 'load', name, title: entry.name, notes: entry.notes, source: this.manifest.source, csv });
        this.sources.set(name, text);
        return text;
      }).finally(() => this.fetches.delete(name)));
    }
    return this.fetches.get(name);
  }

  // Loads the datasets a request names into a worker that doesn't have them yet, fetching them all at once.
  async prepare(handle, request) {
    const names = datasetsOf(request).filter((name) => !handle.loaded.has(name));
    const texts = await Promise.all(names.map((name) => this.source(name)));
    for (const [i, text] of texts.entries()) {
      if (!text || handle.loaded.has(names[i])) continue;  // unknown: the engine answers with a helpful error
      const response = JSON.parse(await handle.call(text));
      if (!response.ok) throw new Error(response.error.message);
      handle.loaded.add(names[i]);
    }
  }

  async call(handle, request, progress) {
    await handle.ready;
    await this.prepare(handle, request);
    return JSON.parse(await handle.call(JSON.stringify(request), progress));
  }

  backtest(request) {
    return this.latest.run(() => this.call(this.interactive, request));
  }

  request(request) {
    return this.call(this.interactive, request);
  }

  job(request, progress) {
    return this.call(this.batch, request, progress);
  }

  // A long job can't be interrupted inside the worker, so the worker goes, and a fresh one takes its place.
  cancelJob() {
    this.batch.terminate();
    this.batch = new WorkerHandle();
  }

  async upload(name, csv, title) {
    const text = JSON.stringify({ command: 'load', name, title, source: 'opened in this page', csv });
    const response = JSON.parse(await this.interactive.call(text));
    if (response.ok) {
      this.sources.set(name, text);
      this.interactive.loaded.add(name);
    }
    return response;
  }
}

class ServerEngine {
  constructor(health) {
    this.runtime = 'native';
    this.version = health.version;
    this.latest = new Latest();
    this.controller = null;
  }

  async start() {}

  async post(request, signal) {
    const response = await fetch('api', { method: 'POST', body: JSON.stringify(request), headers: { 'Content-Type': 'application/json' }, signal });
    return response.json();
  }

  backtest(request) {
    return this.latest.run(() => this.post(request));
  }

  request(request) {
    return this.post(request);
  }

  async job(request, progress) {
    this.controller = new AbortController();
    progress?.(null);  // the server doesn't report progress
    try {
      return await this.post(request, this.controller.signal);
    } catch (error) {
      if (error.name === 'AbortError') throw new Cancelled();
      throw error;
    }
  }

  cancelJob() {
    this.controller?.abort();
  }

  upload(name, csv, title) {
    return this.post({ command: 'load', name, title, source: 'opened in this page', csv });
  }
}

// The native server when the page came from `nanotrade serve` (unless ?runtime=wasm), WebAssembly otherwise.
export async function connect(manifest) {
  const forced = new URLSearchParams(location.search).get('runtime');
  if (runtime === 'native' && forced !== 'wasm') {
    const response = await fetch('api/health', { cache: 'no-store' });
    if (!response.ok) throw new Error(`the server's health check answered ${response.status}`);
    const engine = new ServerEngine(await response.json());
    await engine.start();
    return engine;
  }
  const engine = new WasmEngine(manifest);
  await engine.start();
  return engine;
}
