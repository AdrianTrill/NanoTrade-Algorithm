// The WebAssembly build against the native one: every golden request in tests/golden, sent to the engine compiled to
// WebAssembly, must get back the exact bytes of its golden response, which CTest requires of the native build on
// Linux, macOS, and Windows.
//
//   node web/check.mjs SITE_DIR GOLDEN_DIR
//
// The bundled datasets are loaded into the engine before the first request that names one, as the page loads them.
// The one request skipped is "datasets": the in-browser engine has no data directory to list, and the page reads
// data/manifest.json itself.
import { readdirSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [siteDir, goldenDir] = process.argv.slice(2);
if (!siteDir || !goldenDir) {
  console.error('usage: node web/check.mjs SITE_DIR GOLDEN_DIR');
  process.exit(2);
}
const { default: createNanoTrade } = await import(pathToFileURL(resolve(siteDir, 'js', 'nanotrade.mjs')).href);
const engine = await createNanoTrade();
const manifest = JSON.parse(readFileSync(join(siteDir, 'data', 'manifest.json'), 'utf8'));
const loaded = new Set();

function ensureDataset(request) {
  const name = typeof request?.data === 'string' ? request.data : null;
  const entry = manifest.datasets.find((d) => d.symbol === name);
  if (!entry || loaded.has(name)) return;
  const csv = readFileSync(join(siteDir, 'data', entry.file), 'utf8');
  const load = { command: 'load', name, title: entry.name, notes: entry.notes, source: manifest.source, csv };
  const response = JSON.parse(engine.handleExact(JSON.stringify(load)));
  if (!response.ok) throw new Error(`loading ${name}: ${response.error.message}`);
  loaded.add(name);
}

const requests = readdirSync(goldenDir).filter((name) => name.endsWith('.request.json')).sort();
let checked = 0;
let failures = 0;
for (const name of requests) {
  const text = readFileSync(join(goldenDir, name), 'utf8');
  let request = null;
  try {
    request = JSON.parse(text);
  } catch {
    // A request that isn't JSON, there to test the error: sent as it is.
  }
  if (request?.command === 'datasets') {
    console.log(`skip  ${name}: the in-browser engine has no data directory`);
    continue;
  }
  ensureDataset(request);
  const expected = readFileSync(join(goldenDir, name.replace('.request.json', '.response.json')), 'utf8');
  const actual = `${engine.handleExact(text)}\n`;
  checked += 1;
  if (actual === expected) {
    console.log(`ok    ${name}`);
    continue;
  }
  failures += 1;
  let at = 0;
  while (at < actual.length && actual[at] === expected[at]) at += 1;
  console.log(`FAIL  ${name}: differs at byte ${at}`);
  console.log(`      wasm   ...${actual.slice(Math.max(0, at - 60), at + 60)}`);
  console.log(`      native ...${expected.slice(Math.max(0, at - 60), at + 60)}`);
}
console.log(`${checked - failures} of ${checked} golden responses match in WebAssembly (engine ${engine.version()})`);
process.exit(failures === 0 && checked > 0 ? 0 : 1);
