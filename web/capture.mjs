// Regenerates the README's images, its demo GIF, and the link-preview card from the built site:
//
//   emcmake cmake -S . -B build-wasm && cmake --build build-wasm     (the site, in build-wasm/site)
//   node web/capture.mjs build-wasm/site
//
// Writes docs/assets/{backtest,rules,optimize,walkforward,montecarlo}.png, docs/assets/demo.gif, and web/social.png
// (rebuild the site to publish it). The page runs its engine in WebAssembly, as on GitHub Pages. Needs Chrome, and
// ffmpeg for the GIF.
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer-core';
import { serve } from './serve.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const site = process.argv[2];
if (!site) {
  console.error('usage: node web/capture.mjs SITE_DIR');
  process.exit(2);
}
const assets = join(here, '..', 'docs', 'assets');
mkdirSync(assets, { recursive: true });
const { server, url } = await serve(site);
const chrome = [process.env.CHROME_PATH, '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/usr/bin/google-chrome']
  .find((path) => path && existsSync(path));
const browser = await puppeteer.launch({ executablePath: chrome, headless: true, args: ['--hide-scrollbars'] });
const wait = (ms) => new Promise((done) => setTimeout(done, ms));
const sweep = 'sx_=fast:10:200:10&sy_=slow:20:400:20';

async function open(query, { theme = 'light', width = 1600, height = 1200, scale = 1.25 } = {}) {
  const context = await browser.createBrowserContext();
  const page = await context.newPage();
  await page.evaluateOnNewDocument((edition) => localStorage.setItem('nanotrade-theme', edition), theme);
  await page.setViewport({ width, height, deviceScaleFactor: scale });
  await page.goto(`${url}${query}`, { waitUntil: 'load' });
  await page.waitForFunction(() => document.body.dataset.ready === 'true' || document.body.dataset.error, { timeout: 90_000 });
  if (await page.evaluate(() => document.body.dataset.error)) throw new Error(`${query}: the lab didn't start`);
  await page.mouse.move(0, 0);
  await wait(600);
  return page;
}

const counter = (page, key) => page.evaluate((name) => Number(document.body.dataset[name] ?? 0), key);
const after = (page, key, before) =>
  page.waitForFunction((name, n) => Number(document.body.dataset[name] ?? 0) > n, { timeout: 180_000 }, key, before);

// Runs the open view's research, from its button, and waits for the result.
async function research(page, button, key) {
  const before = await counter(page, key);
  await page.click(button);
  await after(page, key, before);
  await page.mouse.move(0, 0);
  await wait(800);
}

async function typeRule(page, index, rule) {
  const area = (await page.$$('.editor textarea'))[index];
  // A triple click selects one wrapped line, not the rule; typing then replaces the whole selection.
  await area.evaluate((node) => {
    node.focus();
    node.select();
  });
  await area.type(rule, { delay: 45 });
}

// Shortcuts don't fire while a field has the focus.
async function press(page, key) {
  await page.evaluate(() => document.activeElement?.blur());
  await page.keyboard.press(key);
}

async function scrollTo(page, selector) {
  await page.evaluate((target) => {
    const top = target ? document.querySelector(target).getBoundingClientRect().top + window.scrollY - 24 : 0;
    window.scrollTo({ top, behavior: 'smooth' });
  }, selector);
  await wait(900);
}

// The backtest, in the paper edition, tall enough for the whole chart.
let page = await open('?d=AAPL&from=2003-01-01&p=fast:20,slow:100&cb=1&sb=2', { height: 1400 });
await page.screenshot({ path: join(assets, 'backtest.png') });
const backtestMs = await page.$eval('#runtime-detail', (node) => node.textContent);
await page.browserContext().close();
console.log(`backtest.png (${backtestMs})`);

// Custom rules, with a mistake the editor points at, in the night edition.
const rules = new URLSearchParams({
  d: 'F',
  s: 'custom',
  p: 'trend:200',
  le: 'rsi(close, 2) < 10 and close > sma(close, $trend)',
  lx: 'close > sma(close, 5)',
});
page = await open(`?${rules}`, { theme: 'dark', scale: 2 });
await typeRule(page, 0, 'rsi(close, 2) < oversold and close > sma(close, $trend)');
await page.waitForFunction(() => !document.querySelector('#rule-error')?.hidden, { timeout: 30_000 });
await page.mouse.move(0, 0);
await wait(500);
const [, strategyPanel] = await page.$$('.spec-section');
await strategyPanel.screenshot({ path: join(assets, 'rules.png') });
await page.browserContext().close();
console.log('rules.png');

// The research views.
page = await open(`?view=optimize&d=GE&from=&${sweep}`, { theme: 'dark' });
await research(page, '#sweep-run', 'sweep');
await page.screenshot({ path: join(assets, 'optimize.png') });
await page.browserContext().close();
console.log('optimize.png');

page = await open(`?view=walkforward&d=BA&${sweep}`);
await research(page, '#walk-run', 'walk');
await page.screenshot({ path: join(assets, 'walkforward.png') });
await page.browserContext().close();
console.log('walkforward.png');

page = await open('?view=montecarlo&d=AAPL&s=rsi2_pullback&cb=1&sb=2', { theme: 'dark' });
await research(page, '#monte-run', 'monte');
await page.screenshot({ path: join(assets, 'montecarlo.png') });
await page.browserContext().close();
console.log('montecarlo.png');

// The link-preview card: the parameter map of a search, its verdict, and the lab's own timings.
page = await open(`?view=optimize&d=GE&from=&${sweep}`, { width: 1400, height: 900, scale: 2 });
await research(page, '#sweep-run', 'sweep');
// The time of the search itself, which the page reports beside its button.
const sweepTime = await page.$eval('#sweep-estimate', (node) => node.textContent.match(/in ([\d.]+) s/)[1]);
const [dsr, pbo] = await page.$$eval('#overfit .measure .big', (nodes) => nodes.map((node) => node.textContent));
const mapFigure = await page.$('.heatmap-wrap');
const figure = await mapFigure.screenshot({ encoding: 'base64' });
await page.browserContext().close();
const manifest = JSON.parse(readFileSync(join(site, 'data', 'manifest.json'), 'utf8'));
const bars = manifest.datasets.reduce((sum, dataset) => sum + dataset.bars, 0);
const logo = readFileSync(join(here, 'favicon.svg'), 'utf8');
const card = readFileSync(join(here, 'social-card.html'), 'utf8')
  .replace('{{LOGO}}', logo)
  .replace('{{BARS}}', bars.toLocaleString('en-US'))
  .replace('{{BACKTEST}}', backtestMs.replace(/\.\d+ ms/, ' ms'))
  .replace('{{SWEEP}}', `${Number(sweepTime).toFixed(1)} s`)
  .replace('{{FIGURE}}', `data:image/png;base64,${figure}`)
  .replace('{{CAPTION}}', `The SMA crossover's Sharpe ratio for 300 pairs of windows on General Electric, 1962 to 2013. `
    + `Deflated Sharpe ratio <b>${dsr}</b>; probability of backtest overfitting <b>${pbo}</b>.`);
const cardPage = await browser.newPage();
await cardPage.setViewport({ width: 1200, height: 630, deviceScaleFactor: 1 });
await cardPage.setContent(card, { waitUntil: 'load' });
await cardPage.screenshot({ path: join(here, 'social.png') });
await cardPage.close();
console.log(`social.png (backtest ${backtestMs}, search ${sweepTime} s, DSR ${dsr}, PBO ${pbo})`);

// The demo: a backtest, another market, a rule written by hand, a search and its verdict, a walk-forward test,
// resampling, and the night edition.
const video = join(tmpdir(), 'nanotrade-demo.webm');
page = await open('?d=AAPL&cb=1&sb=2', { width: 1440, height: 900, scale: 1 });
const recorder = await page.screencast({ path: video });
await wait(1500);
let before = await counter(page, 'backtests');
await press(page, ']');
await after(page, 'backtests', before);
await wait(1200);
await press(page, 'e');
await page.waitForSelector('.editor textarea');
await wait(400);
before = await counter(page, 'backtests');
await typeRule(page, 0, 'rsi(close, 2) < 10 and close > sma(close, 200)');
await after(page, 'backtests', before);
await wait(1000);
await scrollTo(page, '#chart');
await wait(1600);
await scrollTo(page, null);
before = await counter(page, 'backtests');
await page.select('#strategy', 'sma_cross');
await after(page, 'backtests', before);
await wait(500);
await press(page, '2');
await wait(600);
before = await counter(page, 'sweep');
await press(page, 'Enter');
await after(page, 'sweep', before);
await scrollTo(page, '#heatmap-title');
// Valid pairs, where the fast window is below the slow one, are the map's upper left.
const map = await (await page.$('#heatmap')).boundingBox();
await page.mouse.move(map.x + map.width * 0.2, map.y + map.height * 0.3, { steps: 15 });
await wait(1200);
await page.mouse.move(map.x + map.width * 0.45, map.y + map.height * 0.15, { steps: 15 });
await wait(1600);
await scrollTo(page, null);
await press(page, '3');
await wait(400);
before = await counter(page, 'walk');
await press(page, 'Enter');
await after(page, 'walk', before);
await scrollTo(page, '#walk-kpis');
await wait(2200);
await scrollTo(page, null);
await press(page, '4');
await wait(400);
before = await counter(page, 'monte');
await press(page, 'Enter');
await after(page, 'monte', before);
await scrollTo(page, '#monte-kpis');
await wait(1800);
await press(page, 't');
await wait(1600);
await scrollTo(page, null);
await press(page, '1');
await wait(1800);
await recorder.stop();
await page.browserContext().close();

const gif = join(assets, 'demo.gif');
execFileSync('ffmpeg', [
  '-y', '-loglevel', 'error', '-i', video, '-vf',
  'fps=10,scale=1000:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle',
  gif,
]);
rmSync(video, { force: true });
console.log('demo.gif');

await browser.close();
server.close();
