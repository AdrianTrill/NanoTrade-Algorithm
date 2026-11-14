// The lab in headless Chrome: it must start without errors, run a backtest, react to its settings, report a rule's
// error where it is, run a sweep, a walk-forward test, and Monte Carlo resampling, open a CSV, switch themes, answer
// its keys and its command menu, restore a backtest from a link, and fit a phone's screen, with the settings in a
// sheet over the result.
//
//   node web/ui-check.mjs SITE_DIR [--native PATH_TO_NANOTRADE] [--screenshots DIR]
//
// By default the site is served as GitHub Pages serves it, without an API, so the engine runs in WebAssembly. With
// --native, `nanotrade serve` serves it, and the page runs against the C++ build instead.
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import puppeteer from 'puppeteer-core';
import { serve } from './serve.mjs';

const args = process.argv.slice(2);
const siteDir = args[0];
const option = (name) => (args.includes(name) ? args[args.indexOf(name) + 1] : null);
const native = option('--native');
const shots = option('--screenshots');
if (!siteDir) {
  console.error('usage: node web/ui-check.mjs SITE_DIR [--native PATH_TO_NANOTRADE] [--screenshots DIR]');
  process.exit(2);
}
if (shots) mkdirSync(shots, { recursive: true });

const failures = [];
const check = (condition, message) => {
  console.log(`${condition ? 'ok  ' : 'FAIL'}  ${message}`);
  if (!condition) failures.push(message);
};
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

async function start() {
  if (!native) {
    const { server, url } = await serve(siteDir);
    return { url, stop: () => server.close() };
  }
  const child = spawn(resolve(native), ['serve', '--site', resolve(siteDir), '--port', '0', '--quiet'], { stdio: ['ignore', 'pipe', 'inherit'] });
  const url = await new Promise((done, fail) => {
    let text = '';
    child.stdout.on('data', (chunk) => {
      text += chunk;
      const match = text.match(/serving (http:\/\/\S+?)\/?\s/);
      if (match) done(`${match[1]}/`);
    });
    child.on('exit', (code) => fail(new Error(`nanotrade serve exited with ${code}`)));
  });
  return { url, stop: () => child.kill() };
}

const { url, stop } = await start();
const chrome = [process.env.CHROME_PATH, '/usr/bin/google-chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome']
  .find((path) => path && existsSync(path));
const browser = await puppeteer.launch({ executablePath: chrome, headless: true, args: process.platform === 'linux' ? ['--no-sandbox'] : [] });
const problems = [];

async function open(query = '', { width = 1440, height = 900, mobile = false, theme = 'light' } = {}) {
  const page = await browser.newPage();
  page.on('console', (message) => {
    if (message.type() === 'error') problems.push(`console: ${message.text()}`);
  });
  page.on('pageerror', (error) => problems.push(`page error: ${error.message}`));
  page.on('requestfailed', (request) => problems.push(`request failed: ${request.url()} ${request.failure()?.errorText}`));
  page.on('response', (response) => {
    if (response.status() >= 400) problems.push(`HTTP ${response.status()}: ${response.url()}`);
  });
  await page.evaluateOnNewDocument((choice) => {
    if (location.protocol.startsWith('http')) localStorage.setItem('nanotrade-theme', choice);
    // The layout shift as the page loads: what moves without being asked to.
    window.layoutShift = 0;
    new PerformanceObserver((list) => {
      for (const entry of list.getEntries()) if (!entry.hadRecentInput) window.layoutShift += entry.value;
    }).observe({ type: 'layout-shift', buffered: true });
  }, theme);
  await page.setViewport({ width, height, deviceScaleFactor: 1, isMobile: mobile, hasTouch: mobile });
  await page.goto(`${url}${query}`, { waitUntil: 'load' });
  await page.waitForFunction(() => document.body.dataset.ready === 'true' || document.body.dataset.error, { timeout: 90000 });
  return page;
}

const text = (page, selector) => page.$eval(selector, (node) => node.textContent);
const count = (page, selector) => page.$$eval(selector, (nodes) => nodes.length);
const backtests = (page) => page.evaluate(() => Number(document.body.dataset.backtests ?? 0));
async function nextBacktest(page, before) {
  await page.waitForFunction((n) => Number(document.body.dataset.backtests ?? 0) > n, { timeout: 60000 }, before);
  await sleep(150);
}
async function shoot(page, name) {
  if (shots) await page.screenshot({ path: join(shots, `${name}.png`) });
}
// Drags a slider by setting its value and announcing the input, as a pointer would.
async function slide(page, label, value) {
  await page.evaluate((name, v) => {
    const range = document.querySelector(`input[type="range"][aria-label="${name}"]`);
    range.value = String(v);
    range.dispatchEvent(new Event('input', { bubbles: true }));
  }, label, value);
}
async function typeRule(page, index, rule) {
  await page.evaluate((i, value) => {
    const area = document.querySelectorAll('.editor textarea')[i];
    area.value = value;
    area.dispatchEvent(new Event('input', { bubbles: true }));
  }, index, rule);
}
const runs = (page, counter) => page.evaluate((key) => Number(document.body.dataset[key] ?? 0), counter);
async function nextRun(page, counter, before) {
  await page.waitForFunction((key, n) => Number(document.body.dataset[key] ?? 0) > n, { timeout: 180000 }, counter, before);
  await sleep(300);
}
// Opens a research view, which runs by itself when its work is small.
async function open_view(page, view, counter) {
  const before = await runs(page, counter);
  await page.click(`.view[data-view="${view}"]`);
  await nextRun(page, counter, before);
}

try {
  // While the engine starts, placeholders stand in for the figures and the chart; a failed start says so.
  const held = await browser.newPage();
  await held.setRequestInterception(true);
  held.on('request', (request) => (request.url().endsWith('manifest.json') ? setTimeout(() => request.continue(), 1500) : request.continue()));
  await held.goto(url, { waitUntil: 'domcontentloaded' });
  check(await held.evaluate(() => document.body.classList.contains('booting') && document.querySelectorAll('#kpis .skeleton').length === 6),
    'placeholders stand in while the engine starts');
  await held.waitForFunction(() => document.body.dataset.ready === 'true', { timeout: 90000 });
  check(await held.evaluate(() => !document.body.classList.contains('booting') && !document.querySelector('#kpis .skeleton')),
    'and give way to the backtest');
  await held.close();
  const broken = await browser.newPage();
  await broken.setRequestInterception(true);
  broken.on('request', (request) => (request.url().endsWith('manifest.json') ? request.abort() : request.continue()));
  await broken.goto(url, { waitUntil: 'load' });
  await broken.waitForFunction(() => document.body.dataset.error, { timeout: 30000 });
  check(/didn't start/.test(await text(broken, '#headline')) && Boolean(await broken.$('#deck .button')), 'a failed start says so, with a way to retry');
  await broken.close();

  // The default backtest.
  let page = await open();
  await page.keyboard.press('Tab');
  check(await page.evaluate(() => document.activeElement.classList.contains('skip-link')
    && document.activeElement.getBoundingClientRect().top >= 0), 'the first Tab offers a skip to the report');
  await page.evaluate(() => document.activeElement.blur());
  check(!(await page.evaluate(() => document.body.dataset.error)), 'the engine starts');
  const runtime = await text(page, '#runtime-label');
  check(native ? /native/i.test(runtime) : /WebAssembly/.test(runtime), `it runs ${native ? 'on the native server' : 'in WebAssembly'}: "${runtime}"`);
  check(/SMA crossover on Apple/.test(await text(page, '#headline')), 'the default backtest is the SMA crossover on Apple');
  check((await count(page, '#kpis .figure')) === 6, 'six headline figures');
  const shift = await page.evaluate(() => window.layoutShift);
  check(shift < 0.02, `nothing moves as the first result replaces its placeholders (layout shift ${shift.toFixed(3)})`);
  check((await count(page, '#trades tbody tr')) > 10, 'the trade log lists the trades');
  const inked = await page.$eval('#chart', (canvas) => {
    const context = canvas.getContext('2d');
    const { data } = context.getImageData(0, 0, canvas.width, canvas.height);
    let painted = 0;
    for (let i = 3; i < data.length; i += 4 * 97) painted += data[i] > 0 ? 1 : 0;
    return painted;
  });
  check(inked > 200, 'the chart is drawn');
  await shoot(page, 'backtest');

  // Settings rerun the backtest.
  let before = await backtests(page);
  await slide(page, 'Fast SMA', 10);
  await nextBacktest(page, before);
  check((await text(page, '#dateline')).includes('fast 10'), 'moving a slider reruns the backtest with the new parameter');

  // A pinned backtest, compared with the next ones.
  await page.evaluate(() => document.activeElement?.blur());
  await page.keyboard.press('p');
  before = await backtests(page);
  await slide(page, 'Fast SMA', 40);
  await nextBacktest(page, before);
  check(await page.evaluate(() => !document.querySelector('#pinned').hidden && document.querySelectorAll('#kpis .delta').length === 6
    && window.nanotrade.backtest.chart.data.panes.find((pane) => pane.id === 'equity').series.length === 3),
  'a pinned backtest is compared with the next: each figure\'s change, and its equity on the chart');
  await page.click('.tab[data-detail="metrics"]');
  check((await text(page, '#metrics thead')).includes('Pinned'), 'and every metric beside its own');
  await page.click('.tab[data-detail="trades"]');
  await page.evaluate(() => document.activeElement?.blur());
  await page.keyboard.press('p');
  check(await page.evaluate(() => document.querySelector('#pinned').hidden && !document.querySelector('#kpis .delta')), 'P unpins it');
  before = await backtests(page);
  await page.select('#dataset', 'GE');
  await nextBacktest(page, before);
  check(/General Electric/.test(await text(page, '#headline')), 'choosing another market reruns it on that market');
  for (const tab of ['monthly', 'metrics', 'drawdowns', 'trades']) {
    await page.click(`.tab[data-detail="${tab}"]`);
    const visible = await page.$eval(`[data-detail-panel="${tab}"]`, (panel) => !panel.hidden && panel.querySelectorAll('tr').length > 1);
    check(visible, `the ${tab} table shows`);
  }

  // A custom rule, with a mistake in it.
  await page.click('#edit-rules');
  await page.waitForSelector('.editor textarea');
  check((await page.$eval('#strategy', (select) => select.value)) === 'custom', 'editing the rules turns the strategy into a custom one');
  await typeRule(page, 0, 'rsi(close, 14) < oversold');
  await page.waitForFunction(() => !document.querySelector('#rule-error')?.hidden, { timeout: 30000 });
  check((await text(page, '#rule-error')).includes("unknown name 'oversold'"), 'a mistake in a rule is reported');
  check((await count(page, '.editor.invalid .tok-error')) === 1, 'and underlined where it is');
  before = await backtests(page);
  await typeRule(page, 0, 'rsi(close, 14) < 30 and close > sma(close, 200)');
  await typeRule(page, 1, 'rsi(close, 14) > 55');
  await nextBacktest(page, before);
  check(await page.$eval('#rule-error', (box) => box.hidden), 'fixing it clears the error');
  check(/Custom rules on General Electric/.test(await text(page, '#headline')), 'and the custom rules run');
  await shoot(page, 'custom');

  // The research views, on the SMA crossover again.
  before = await backtests(page);
  await page.select('#strategy', 'sma_cross');
  await nextBacktest(page, before);
  await open_view(page, 'optimize', 'sweep');
  check(true, 'opening Optimize runs the sweep');
  check((await count(page, '#overfit .measure')) === 2, 'a sweep reports the deflated Sharpe ratio and the probability of overfitting');
  check((await count(page, '#top tbody tr')) === 10, 'and its ten best combinations');
  await shoot(page, 'optimize');
  let sweeps = await runs(page, 'sweep');
  await slide(page, 'Commission', 5);
  await page.waitForFunction(() => document.querySelector('[data-panel="optimize"]').classList.contains('stale'), { timeout: 30000 });
  check(true, 'a change of setting marks the sweep out of date');
  await nextRun(page, 'sweep', sweeps);
  check(!(await page.$eval('[data-panel="optimize"]', (panel) => panel.classList.contains('stale'))), 'and runs it again');
  sweeps = await runs(page, 'sweep');
  await page.click('#sweep-run');
  await nextRun(page, 'sweep', sweeps);
  check(true, 'Run the sweep runs it on demand');
  before = await backtests(page);
  const [chosenFast] = await page.$$eval('#top tbody tr:first-child td', (cells) => cells.map((cell) => cell.textContent));
  await page.focus('#top tbody tr:first-child');
  await page.keyboard.press('Enter');
  await nextBacktest(page, before);
  check((await page.$eval('.view[aria-selected="true"]', (tab) => tab.dataset.view)) === 'backtest'
    && (await text(page, '#dateline')).includes(`fast ${chosenFast}`), 'choosing a combination, from the keyboard, backtests it');
  await open_view(page, 'walkforward', 'walk');
  check((await count(page, '#folds tbody tr')) > 5, 'a walk-forward test lists its windows');
  check((await count(page, '#walk-kpis .figure')) === 6, 'and its figures');
  await shoot(page, 'walkforward');
  await open_view(page, 'montecarlo', 'monte');
  check((await count(page, '#histograms canvas')) === 3, 'Monte Carlo resampling draws its three distributions');
  await shoot(page, 'montecarlo');

  // Keys, themes, the reference, and a file.
  await page.keyboard.press('1');
  check((await page.$eval('.view[aria-selected="true"]', (tab) => tab.dataset.view)) === 'backtest', '1 returns to the backtest');
  await page.focus('#tab-backtest');
  await page.keyboard.press('ArrowRight');
  check(await page.evaluate(() => document.activeElement.id === 'tab-optimize' && document.activeElement.getAttribute('aria-selected') === 'true'
    && document.querySelector('#tab-backtest').tabIndex === -1), 'the arrow keys move between the views');
  await page.keyboard.press('ArrowLeft');
  await page.evaluate(() => document.activeElement.blur());
  await page.keyboard.press('t');
  await sleep(400);
  check((await page.evaluate(() => document.documentElement.dataset.theme)) === 'dark', 'T switches to the dark theme');
  await shoot(page, 'dark');
  await page.keyboard.press('t');
  await sleep(400);
  await page.emulateMediaFeatures([{ name: 'prefers-color-scheme', value: 'dark' }]);
  await page.waitForFunction(() => !document.querySelector('.toast'), { timeout: 10000 });  // the corner's notices cover the switch
  await page.click('[data-theme-choice="system"]');
  await sleep(400);
  check(await page.evaluate(() => document.documentElement.dataset.theme === 'dark' && localStorage.getItem('nanotrade-theme') === null),
    'the footer\'s System follows the system\'s theme');
  await page.click('[data-theme-choice="light"]');
  await sleep(400);
  check(await page.evaluate(() => document.documentElement.dataset.theme === 'light' && localStorage.getItem('nanotrade-theme') === 'light'),
    'and Light keeps to the light one');
  await page.emulateMediaFeatures();
  await page.keyboard.press('?');
  check(await page.$eval('#shortcuts', (dialog) => dialog.open), '? lists the shortcuts');
  await page.keyboard.press('Escape');
  await page.keyboard.press('f');
  check(await page.$eval('#reference', (dialog) => dialog.open) && (await count(page, '#reference tbody tr')) >= 20,
    'F opens the rule language reference');
  await page.keyboard.press('Escape');

  // The command menu, from its keys and from the breadcrumbs.
  check((await text(page, '#crumb-data-label')) === 'GE · General Electric' && (await text(page, '#crumb-strategy-label')) === 'SMA crossover',
    'the breadcrumbs name the market and the strategy');
  await page.keyboard.down('Control');
  await page.keyboard.press('k');
  await page.keyboard.up('Control');
  check(await page.$eval('#command', (dialog) => dialog.open) && (await count(page, '#command [role="option"]')) > 20,
    'Ctrl K opens the command menu, with every command');
  await page.keyboard.type('walk');
  check(await page.evaluate(() => document.querySelector('#command [aria-selected="true"]')?.textContent.startsWith('Walk-forward')),
    'typing finds a command');
  await page.keyboard.press('Escape');
  await page.evaluate(() => window.scrollTo(0, 0));  // the crumbs' row scrolls away with the page on a wide screen
  await page.click('#crumb-data');
  check(await page.evaluate(() => {
    const dialog = document.querySelector('#command');
    return dialog.open && dialog.dataset.scope === 'data' && dialog.querySelector('[aria-selected="true"]')?.textContent.startsWith('GE ·');
  }), 'the market\'s breadcrumb opens it on the markets, at the current one');
  before = await backtests(page);
  await page.keyboard.type('ford');
  await page.keyboard.press('Enter');
  await nextBacktest(page, before);
  check(/on Ford/.test(await text(page, '#headline')) && (await text(page, '#crumb-data-label')) === 'F · Ford',
    'and choosing another backtests it there');

  // The run as an API request, which the engine answers with the backtest on the page.
  await page.keyboard.press('a');
  await sleep(300);
  const asked = await page.evaluate(async () => {
    const json = JSON.parse(document.querySelector('#api-code').textContent);
    const response = await window.nanotrade.engine.request(json);
    return { open: document.querySelector('#api').open, command: json.command,
      same: response.ok && response.result.metrics.sharpe === window.nanotrade.backtest.result.metrics.sharpe };
  });
  check(asked.open && asked.command === 'backtest' && asked.same, 'A shows the run as an API request, which gives the backtest on the page');
  await page.click('#api [data-format="cli"]');
  check((await text(page, '#api-code')).startsWith("nanotrade api <<'EOF'"), 'and as a command line');
  await page.keyboard.press('Escape');
  const folder = mkdtempSync(join(tmpdir(), 'nanotrade-ui-'));
  const lines = ['Date,Open,High,Low,Close,Volume'];
  let close = 100;
  for (let day = 0; day < 400; day += 1) {
    const date = new Date(Date.UTC(2020, 0, 1) + day * 86400000);
    if (date.getUTCDay() === 0 || date.getUTCDay() === 6) continue;
    const open = close;
    close = Math.max(1, close * (1 + Math.sin(day / 9) * 0.02));
    lines.push(`${date.toISOString().slice(0, 10)},${open.toFixed(2)},${(Math.max(open, close) + 1).toFixed(2)},${(Math.min(open, close) - 1).toFixed(2)},${close.toFixed(2)},1000`);
  }
  const file = join(folder, 'waves.csv');
  writeFileSync(file, `${lines.join('\n')}\n`);
  before = await backtests(page);
  await (await page.$('#file')).uploadFile(file);
  await nextBacktest(page, before);
  check((await page.$eval('#dataset', (select) => select.value)) === 'file:waves' && /waves/.test(await text(page, '#headline')),
    'an opened CSV is backtested');
  rmSync(folder, { recursive: true, force: true });
  const link = await page.evaluate(() => location.search);
  check(link.includes('d=file'), 'the address keeps the settings');
  await page.close();

  // A link restores a backtest.
  page = await open('?d=F&s=donchian&short=1&ts=0.12');
  check(/Donchian breakout on Ford/.test(await text(page, '#headline')), 'a link restores its backtest');
  check((await page.$$eval('.rule-label', (labels) => labels.map((label) => label.textContent))).includes('Go short when'),
    'with its short rules');
  await page.close();

  // A phone: the result first, and the settings in a sheet over it.
  page = await open('', { width: 390, height: 844, mobile: true });
  check(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1), 'nothing overflows a phone\'s screen');
  check(await page.$eval('#headline', (node) => node.getBoundingClientRect().top < innerHeight / 3), 'the result comes first on a phone');
  const phoneShift = await page.evaluate(() => window.layoutShift);
  check(phoneShift < 0.02, `and nothing moves as it arrives (layout shift ${phoneShift.toFixed(3)})`);
  check(await page.$eval('#spec', (node) => getComputedStyle(node).visibility === 'hidden'), 'the settings wait until they\'re asked for');
  await shoot(page, 'phone');
  await page.tap('#spec-toggle');
  await sleep(400);
  check(await page.evaluate(() => document.querySelector('#spec-toggle').getAttribute('aria-expanded') === 'true'
    && getComputedStyle(document.querySelector('#spec')).visibility === 'visible'
    && document.querySelector('#spec').contains(document.activeElement)), 'Settings opens them, with the focus inside');
  check(await page.$eval('#kpis', (node) => node.getBoundingClientRect().top < innerHeight / 3), 'with the headline figures in view above them');
  await shoot(page, 'phone-settings');
  before = await backtests(page);
  await page.tap('#direction [data-value="both"]');
  await nextBacktest(page, before);
  check((await page.evaluate(() => location.search)).includes('short=1'), 'a change in them reruns the backtest at once');
  await page.keyboard.press('Escape');
  await sleep(400);
  check(await page.evaluate(() => !document.documentElement.classList.contains('spec-open')
    && document.activeElement === document.querySelector('#spec-toggle')), 'Escape closes them and returns the focus');
  await page.tap('#crumb-data');
  await sleep(400);
  check(await page.evaluate(() => document.querySelector('#command').open && document.activeElement === document.querySelector('#command-list')
    && document.querySelector('#command [aria-selected="true"]')?.textContent.startsWith('AAPL')), 'the market\'s breadcrumb opens its list without the keyboard');
  const ford = await page.$$eval('#command [role="option"]', (options) => options.find((option) => option.textContent.startsWith('F ·')).id);
  before = await backtests(page);
  await page.tap(`#${ford}`);
  await nextBacktest(page, before);
  check(/on Ford/.test(await text(page, '#headline')), 'and a tap on another backtests it there');
  await page.close();

  // A tablet: the settings slide in from the left, beside the report.
  page = await open('', { width: 820, height: 1180, mobile: true });
  await page.tap('#spec-toggle');
  await sleep(400);
  const panel = await page.$eval('#spec', (node) => node.getBoundingClientRect().toJSON());
  check(panel.left === 0 && panel.width < 450, 'on a tablet they open as a panel beside the report');
  await page.tap('#scrim');
  await sleep(400);
  check(await page.evaluate(() => !document.documentElement.classList.contains('spec-open')), 'a tap beside it closes it');
  await page.close();

  check(problems.length === 0, `no console errors or failed requests${problems.length ? `:\n  ${problems.join('\n  ')}` : ''}`);
} catch (error) {
  failures.push(error.message);
  console.log(`FAIL  ${error.stack}`);
} finally {
  await browser.close();
  stop();
}
console.log(failures.length ? `${failures.length} check(s) failed` : 'every check passed');
process.exit(failures.length ? 1 : 0);
