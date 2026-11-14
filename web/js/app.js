// NanoTrade Lab: wires the settings to the engine and the views.
import { BacktestView } from './backtest.js';
import { avatar, CommandMenu } from './command.js';
import { Controls } from './controls.js';
import { $, $$, element, rovingTabs } from './dom.js';
import { connect, Superseded } from './engine.js';
import { highlight } from './editor.js';
import * as format from './format.js';
import { Drawer, trackMasthead } from './layout.js';
import { readPalette } from './palette.js';
import { runtime } from './runtime.js';
import { MonteCarloView, OptimizeView, WalkForwardView } from './research.js';
import { startShortcuts } from './shortcuts.js';
import { dataSpec, fromQuery, request, saveQuery, state } from './state.js';
import { currentTheme, setTheme, startTheme, themeChoice, toggleTheme } from './theme.js';
import { toast } from './toast.js';

const VIEWS = ['backtest', 'optimize', 'walkforward', 'montecarlo'];
const VIEW_COMMANDS = [['backtest', 'Backtest', 'chart'], ['optimize', 'Optimize', 'grid'], ['walkforward', 'Walk-forward', 'steps'], ['montecarlo', 'Monte Carlo', 'dice']];
const SOURCE = 'https://github.com/AdrianTrill/NanoTrade-Algorithm';

const EXAMPLES = [
  ['Golden cross', { long_entry: 'sma(close, 50) > sma(close, 200)' }],
  ['Dips in an uptrend', { long_entry: 'close > sma(close, 200) and rsi(close, 2) < 10', long_exit: 'close > sma(close, 5)' }],
  ['Breakout in calm markets', { long_entry: 'close > highest(high, 50)[1] and atr(14) / close < 0.025', long_exit: 'close < lowest(low, 20)[1]' }],
  ['MACD cross above zero', { long_entry: 'crossover(macd(close, 12, 26), macd_signal(close, 12, 26, 9)) and macd(close, 12, 26) > 0', long_exit: 'crossunder(macd(close, 12, 26), macd_signal(close, 12, 26, 9))' }],
  ['Stretched below the mean', { long_entry: 'zscore(close, 20) < -2', long_exit: 'zscore(close, 20) > 0' }],
  ['Momentum with a trend filter', { long_entry: 'roc(close, 126) > 0 and close > ema(close, 100)' }],
];

function setEngineStatus(text, kind) {
  const status = $('#status-engine');
  status.className = `status-engine ${kind}`;
  status.lastElementChild.textContent = text;
  $('#runtime').className = `runtime ${kind}`;
}

class App {
  constructor(engine, catalog, manifest, drawer) {
    this.engine = engine;
    this.drawer = drawer;
    this.catalog = catalog;
    this.manifest = manifest;
    this.bars = null;
    this.barsKey = '';
    this.scheduled = false;
    this.backtests = 0;
    this.backtestOk = false;
    this.pending = 0;  // backtests in flight, for the line under the masthead
  }

  start() {
    this.controls = new Controls({
      catalog: this.catalog,
      manifest: this.manifest,
      onChange: (what) => this.changed(what),
      onUpload: (file) => this.upload(file),
      onReference: () => this.openReference(),
    });
    this.backtest = new BacktestView({ catalog: this.catalog });
    const onError = (error) => this.jobError(error);
    // What the research views need to know of the backtest: its size, to judge whether a run is quick enough to
    // start by itself, and whether it ran, since a research run would only fail the same way.
    const context = {
      bars: () => this.backtest.result?.range?.bars ?? null,
      barsPerYear: () => this.backtest.result?.dataset?.bars_per_year ?? null,
      canRun: () => this.backtestOk,
    };
    this.optimize = new OptimizeView({ engine: this.engine, catalog: this.catalog, context, onApply: (params) => this.applyParams(params), onError });
    this.walk = new WalkForwardView({ engine: this.engine, catalog: this.catalog, context, onError });
    this.monte = new MonteCarloView({ engine: this.engine, context, onError });
    startTheme(() => this.redraw());
    // Geist can arrive after the first drawing, which then used a fallback: the canvases draw again in it.
    document.fonts?.addEventListener('loadingdone', () => this.redraw());

    for (const tab of $$('.view')) tab.addEventListener('click', () => this.showView(tab.dataset.view));
    for (const list of $$('[role="tablist"]')) rovingTabs(list);
    $('#share').addEventListener('click', () => this.share());
    $('#help').addEventListener('click', () => $('#shortcuts').showModal());
    $('#status-help').addEventListener('click', () => $('#shortcuts').showModal());
    this.listenForDrops();
    const actions = {
      1: () => this.showView('backtest'),
      2: () => this.showView('optimize'),
      3: () => this.showView('walkforward'),
      4: () => this.showView('montecarlo'),
      '[': () => this.stepDataset(-1),
      ']': () => this.stepDataset(1),
      n: () => {
        if (state.data !== 'random') {
          state.data = 'random';
          this.controls.renderAll();
        }
        this.controls.reseed();
      },
      o: () => $('#file').click(),
      e: () => {
        this.drawer.open({ focus: false });
        this.controls.editRules();
      },
      f: () => this.openReference(),
      l: () => $('#log').click(),
      c: () => $('#candles').click(),
      r: () => this.backtest.chart.reset(),
      s: () => this.share(),
      t: () => $('#theme').click(),
      '?': () => {
        const dialog = $('#shortcuts');
        if (dialog.open) dialog.close();
        else dialog.showModal();
      },
      Enter: () => {
        if (state.view === 'optimize') this.optimize.run();
        else if (state.view === 'walkforward') this.walk.run();
        else if (state.view === 'montecarlo') this.monte.run();
      },
    };
    startShortcuts(actions);
    this.menu = new CommandMenu(() => this.commands(actions));
    $('#command-open').addEventListener('click', () => this.menu.open());
    $('#crumb-data').addEventListener('click', () => this.menu.open('data'));
    $('#crumb-strategy').addEventListener('click', () => this.menu.open('strategy'));
    this.updateCrumbs();
    this.refreshResearch();
    this.showView(VIEWS.includes(state.view) ? state.view : 'backtest');
    document.body.classList.remove('booting');
    this.runBacktest();
  }

  showView(view) {
    state.view = view;
    for (const tab of $$('.view')) tab.setAttribute('aria-selected', String(tab.dataset.view === view));
    for (const panel of $$('.view-panel')) panel.hidden = panel.dataset.panel !== view;
    saveQuery();
    this.redraw();
    this.syncResearch({ immediate: true });
  }

  // Lets the research views catch up with the settings: the open one reruns if that's quick, and the others mark
  // their results out of date.
  syncResearch(options) {
    for (const view of [this.optimize, this.walk, this.monte]) view.sync(options);
  }

  redraw() {
    this.backtest.chart.draw();
    this.walk.chart.draw();
    this.monte.chart.draw();
    this.optimize.redraw();
    this.monte.redraw();
    if (this.backtest.result) {
      this.backtest.renderLegend();
      this.backtest.renderChart(true);
    }
  }

  refreshResearch() {
    this.optimize.refresh();
    this.walk.refresh();
    this.monte.refresh();
  }

  changed(what) {
    if (what === 'strategy' || what === 'rules') {
      this.refreshResearch();
    }
    this.updateCrumbs();
    saveQuery();
    this.scheduleBacktest();
  }

  // The breadcrumbs name the market and the strategy, each with its avatar.
  updateCrumbs() {
    const info = this.controls.datasetInfo();
    const market = state.data === 'random' ? 'Random market' : info?.symbol ? `${info.symbol} · ${info.name}` : info?.title ?? state.data;
    const strategy = this.controls.strategyInfo()?.name ?? state.strategy;
    for (const [id, label, key] of [['data', market, state.data], ['strategy', strategy, state.strategy]]) {
      $(`#crumb-${id}-label`).textContent = label;
      $(`#crumb-${id} .avatar`).replaceWith(avatar(key));
    }
  }

  // What the command menu offers, as it opens: the views, the markets, the strategies, and the page's actions.
  commands(actions) {
    const list = VIEW_COMMANDS.map(([view, label, glyph], index) => ({
      group: 'Views', label, icon: glyph, hint: String(index + 1), current: state.view === view, run: () => this.showView(view),
    }));
    for (const option of this.controls.datasetSelect.options) {
      list.push({
        group: 'Markets', scope: 'data', label: option.textContent, keywords: option.parentElement.label, avatar: avatar(option.value),
        current: option.value === state.data, run: () => this.choose(this.controls.datasetSelect, option.value),
      });
    }
    list.push(
      { group: 'Markets', scope: 'data', label: 'Open a CSV of daily bars…', keywords: 'file upload import', icon: 'upload', hint: 'O', run: actions.o },
      { group: 'Markets', scope: 'data', label: 'Another random market', keywords: 'seed synthetic', icon: 'dice', hint: 'N', run: actions.n },
    );
    for (const strategy of this.catalog.strategies) {
      list.push({
        group: 'Strategies', scope: 'strategy', label: strategy.name, keywords: `${strategy.family} ${strategy.summary ?? ''}`, avatar: avatar(strategy.id),
        current: strategy.id === state.strategy, run: () => this.choose(this.controls.strategySelect, strategy.id),
      });
    }
    if (state.strategy !== 'custom' && state.strategy !== 'buy_hold') {
      list.push({ group: 'Strategies', scope: 'strategy', label: 'Edit these rules as a custom strategy', keywords: 'custom rules', icon: 'edit', hint: 'E', run: actions.e });
    }
    const dark = currentTheme() === 'dark';
    const candles = $('#candles').getAttribute('aria-pressed') === 'true';
    list.push(
      { group: 'Actions', label: 'Copy a link to this backtest', keywords: 'share url', icon: 'link', hint: 'S', run: actions.s },
      { group: 'Actions', label: `Switch to the ${dark ? 'light' : 'dark'} theme`, keywords: 'theme appearance mode', icon: dark ? 'sun' : 'moon', hint: 'T', run: toggleTheme },
    );
    if (themeChoice() !== 'system') {
      list.push({ group: 'Actions', label: 'Follow the system\'s theme', keywords: 'theme appearance mode auto', icon: 'monitor', run: () => setTheme('system') });
    }
    list.push(
      { group: 'Actions', label: this.backtest.log ? 'Linear price scale' : 'Logarithmic price scale', keywords: 'log chart', icon: 'chart', hint: 'L', run: actions.l },
      { group: 'Actions', label: candles ? 'Lines instead of candles' : 'Candles when zoomed in', keywords: 'chart ohlc', icon: 'chart', hint: 'C', run: actions.c },
      { group: 'Actions', label: 'Show the whole range', keywords: 'zoom reset chart', icon: 'chart', hint: 'R', run: actions.r },
      { group: 'Actions', label: 'Download the trades as CSV', keywords: 'export', icon: 'download', run: () => $('#export').click() },
      { group: 'Actions', label: 'The rule language and its functions', keywords: 'reference help', icon: 'function', hint: 'F', run: actions.f },
      { group: 'Actions', label: 'Keyboard shortcuts', keywords: 'keys help', icon: 'keyboard', hint: '?', run: () => $('#shortcuts').showModal() },
      { group: 'Actions', label: 'Source on GitHub', keywords: 'code repository', icon: 'github', run: () => window.open(SOURCE, '_blank', 'noopener') },
    );
    return list;
  }

  // Chooses an option as if from its select, so the change goes the way a choice there goes.
  choose(select, value) {
    if (select.value === value) return;
    select.value = value;
    select.dispatchEvent(new Event('change'));
  }

  applyParams(params) {
    if (state.strategy === 'custom') Object.assign(state.customParams, params);
    else Object.assign(state.params, params);
    this.controls.renderStrategy();
    this.showView('backtest');
    saveQuery();
    this.scheduleBacktest();
    toast('Backtesting the chosen parameters', Object.entries(params).map(([k, v]) => `${k} ${format.number(v, 2)}`).join(', '));
  }

  scheduleBacktest() {
    if (this.scheduled) return;
    this.scheduled = true;
    requestAnimationFrame(() => {
      this.scheduled = false;
      this.runBacktest();
    });
  }

  rangeSpec() {
    const range = {};
    if (state.from) range.from = state.from;
    if (state.to) range.to = state.to;
    return range;
  }

  async loadBars() {
    const key = JSON.stringify([dataSpec(), state.from, state.to]);
    if (key === this.barsKey && this.bars) return this.bars;
    const response = await this.engine.request({ command: 'bars', data: dataSpec(), range: this.rangeSpec() });
    if (!response.ok) throw Object.assign(new Error(response.error.message), { api: response.error });
    this.bars = response.result;
    this.barsKey = key;
    return this.bars;
  }

  async runBacktest() {
    this.pending += 1;
    document.documentElement.classList.add('working');
    try {
      await this.backtestNow();
    } finally {
      this.pending -= 1;
      document.documentElement.classList.toggle('working', this.pending > 0);
    }
  }

  async backtestNow() {
    this.backtest.setLoading(true);
    $('#runtime').classList.add('busy');
    let response;
    try {
      response = await this.engine.backtest(request('backtest'));
    } catch (error) {
      if (error instanceof Superseded) return;
      this.showError({ message: error.message });
      return;
    } finally {
      $('#runtime').classList.remove('busy');
    }
    if (!response.ok) {
      this.showError(response.error);
      return;
    }
    let bars;
    try {
      bars = await this.loadBars();
    } catch (error) {
      this.showError(error.api ?? { message: error.message });
      return;
    }
    const result = response.result;
    if (bars.time.length !== result.series.time.length) {
      this.barsKey = '';
      this.scheduleBacktest();
      return;
    }
    this.controls.showError(null);
    this.backtest.setLoading(false);
    this.backtest.setStale(false);
    this.backtest.render(result, bars, response.meta);
    if (response.meta) $('#runtime-detail').textContent = `${format.number(response.meta.ms, 1)} ms`;
    this.backtests += 1;
    this.backtestOk = true;
    document.body.dataset.backtests = String(this.backtests);
    document.body.dataset.ready = 'true';
    this.syncResearch();
  }

  showError(error) {
    this.backtestOk = false;
    this.syncResearch();
    this.backtest.setLoading(false);
    this.backtest.setStale(true);
    if (!this.backtest.result) {
      $('#kicker').textContent = 'Backtest';
      $('#headline').textContent = 'Nothing to show yet';
      $('#deck').textContent = `The engine couldn't run this backtest: ${error.message}.`;
      $('#dateline').textContent = '';
    }
    if (!this.controls.showError(error)) toast('The backtest failed', error.message, 'error');
    document.body.dataset.error = error.message;
  }

  jobError(error) {
    if (!this.controls.showError(error)) toast('That didn\'t run', error.message, 'error');
  }

  stepDataset(direction) {
    const options = [...$('#dataset').options].map((o) => o.value);
    const index = options.indexOf(state.data);
    state.data = options[(index + direction + options.length) % options.length];
    this.controls.renderAll();
    this.changed('data');
  }

  async share() {
    saveQuery();
    try {
      await navigator.clipboard.writeText(location.href);
      toast('Link copied', 'It opens this backtest, with every setting.');
    } catch {
      toast('Copy this link', location.href);
    }
  }

  async upload(file) {
    if (file.size > 32 * 1024 * 1024) {
      toast('That file is too large', 'Files up to 32 MB can be opened.', 'error');
      return;
    }
    const csv = await file.text();
    const stem = file.name.replace(/\.[^.]+$/, '').replace(/[^\w-]+/g, '_').slice(0, 40) || 'file';
    const response = await this.engine.upload(`file:${stem}`, csv, file.name);
    if (!response.ok) {
      toast(`Couldn't read ${file.name}`, response.error.message, 'error');
      return;
    }
    const dataset = response.result.dataset;
    this.controls.addUpload(dataset);
    const report = dataset.report;
    const notes = [];
    if (report.reversed) notes.push('it was newest first');
    if (report.sorted) notes.push('its rows were sorted');
    if (report.adjusted) notes.push(report.adjusted === 'columns' ? 'adjusted prices used' : 'prices adjusted by Adj Close');
    if (report.date_order) notes.push(`dates read ${report.date_order}`);
    if (report.skipped) notes.push(`${report.skipped} rows without prices skipped`);
    if (report.repaired) notes.push(`${report.repaired} bars repaired`);
    if (report.duplicates) notes.push(`${report.duplicates} repeated dates`);
    toast(`Opened ${file.name}: ${format.integer(dataset.bars)} bars`, notes.length ? `${notes.join(', ')}. It stays in this page.` : 'It stays in this page.');
    this.changed('data');
  }

  listenForDrops() {
    let depth = 0;
    const overlay = $('#drop');
    const hasFiles = (event) => [...(event.dataTransfer?.types ?? [])].includes('Files');
    document.addEventListener('dragenter', (event) => {
      if (!hasFiles(event)) return;
      depth += 1;
      overlay.hidden = false;
    });
    document.addEventListener('dragleave', () => {
      depth = Math.max(0, depth - 1);
      if (!depth) overlay.hidden = true;
    });
    document.addEventListener('dragover', (event) => {
      if (hasFiles(event)) event.preventDefault();
    });
    document.addEventListener('drop', (event) => {
      if (!hasFiles(event)) return;
      event.preventDefault();
      depth = 0;
      overlay.hidden = true;
      const [file] = event.dataTransfer.files;
      if (file) this.upload(file);
    });
  }

  openReference() {
    const body = $('#reference-body');
    if (!body.children.length) {
      const code = (text) => {
        const node = element('code');
        node.innerHTML = highlight(text).trimEnd();
        return node;
      };
      const rows = this.catalog.functions.map((f) => element('tr', {},
        element('td', { class: 'left' }, code(f.signature)), element('td', { class: 'left' }, f.description)));
      const examples = EXAMPLES.map(([name, rules]) => element('li', {},
        element('button', {
          onclick: () => {
            $('#reference').close();
            this.useExample(rules);
          },
        }, rules.long_entry), ` ${name}${rules.long_exit ? ', with its own exit' : ''}`));
      body.append(element('div', { class: 'reference' },
        element('p', {}, 'A rule is a condition read at each bar\'s close; a position it opens or closes trades at the next bar\'s open. Rules combine prices (', code('open'), ', ', code('high'), ', ', code('low'), ', ', code('close'), ', ', code('volume'), ', ', code('hl2'), '), numbers, arithmetic, comparisons, and ', code('and'), ', ', code('or'), ', and ', code('not'), '. ', code('x[n]'), ' is x as it was n bars ago, and ', code('$name'), ' is a parameter, which can be swept.'),
        element('h3', {}, 'Functions'),
        element('table', {}, element('tbody', {}, rows)),
        element('h3', {}, 'Examples to try'),
        element('ul', { class: 'examples' }, examples),
        element('p', { class: 'note' }, 'An indicator is undefined until its window is full, and a condition on an undefined value is false, so a 200-day average starts trading 200 bars into the data: bars before the date range warm it up.')));
    }
    $('#reference').showModal();
  }

  useExample(rules) {
    if (state.strategy !== 'custom') this.controls.editRules();
    state.rules = { long_entry: '', long_exit: '', short_entry: '', short_exit: '', ...rules };
    this.controls.syncCustomParams();
    this.controls.renderStrategy();
    this.changed('rules');
  }
}

async function main() {
  trackMasthead();
  const drawer = new Drawer();
  readPalette();
  fromQuery(location.search);
  if (runtime === 'native') $('#deck').textContent = 'Connecting to the native engine that serves this page.';
  try {
    const manifest = await fetch('data/manifest.json').then((response) => {
      if (!response.ok) throw new Error(`data/manifest.json: ${response.status}`);
      return response.json();
    });
    setEngineStatus('Loading the engine', 'busy');
    const engine = await connect(manifest);
    $('#runtime-label').textContent = engine.runtime === 'wasm' ? 'WebAssembly' : 'Native';
    $('#runtime').title = engine.runtime === 'wasm' ? 'The engine runs in this tab, compiled to WebAssembly' : 'The engine runs on the native server that serves this page';
    setEngineStatus(engine.runtime === 'wasm' ? `NanoTrade ${engine.version}, compiled to WebAssembly` : `NanoTrade ${engine.version}, native`, 'ready');
    const catalog = await engine.request({ command: 'catalog' });
    if (!catalog.ok) throw new Error(catalog.error.message);
    const app = new App(engine, catalog.result, manifest, drawer);
    window.nanotrade = app;  // for the page's checks
    app.start();
  } catch (error) {
    // Emscripten wraps a failed download as "Aborted(the reason). Build with -sASSERTIONS for more info."
    const reason = error.message.replace(/^Aborted\(([\s\S]*)\)\. Build with -sASSERTIONS[\s\S]*$/, '$1').replace(/[.\s]+$/, '');
    document.body.classList.remove('booting');
    $('#chart').parentElement.classList.remove('loading');
    $('#kpis').replaceChildren();
    for (const select of [$('#dataset'), $('#strategy')]) select.replaceChildren(element('option', {}, 'Unavailable'));
    $('#headline').textContent = 'The engine didn\'t start';
    $('#deck').replaceChildren(`${reason}. Reloading usually fixes this; if it doesn't, the browser may be blocking WebAssembly or the page's data. `,
      element('button', { class: 'button', onclick: () => location.reload() }, 'Reload'));
    $('#runtime-label').textContent = 'Not running';
    setEngineStatus(`The engine failed to start: ${reason}`, 'failed');
    document.body.dataset.error = error.message;
    throw error;
  }
}

main();
