// The engine in a worker: NanoTrade's C++ compiled to WebAssembly, answering the same JSON requests as the native
// server, so long jobs never freeze the page.
import createNanoTrade from './nanotrade.mjs';

let engine = null;

const ready = createNanoTrade().then(
  (module) => {
    engine = module;
    postMessage({ type: 'ready', version: module.version() });
  },
  (error) => postMessage({ type: 'failed', message: String(error?.message ?? error) }),
);

onmessage = async ({ data }) => {
  await ready;
  if (!engine) {
    postMessage({ type: 'response', id: data.id, text: JSON.stringify({ ok: false, error: { message: 'the engine failed to load' } }) });
    return;
  }
  const progress = data.progress ? (value) => postMessage({ type: 'progress', id: data.id, value }) : null;
  const text = engine.handle(data.text, progress);
  postMessage({ type: 'response', id: data.id, text });
};
