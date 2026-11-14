// The research views: a parameter sweep with its overfitting measures, a walk-forward test, and Monte Carlo resampling.
// Each runs on the batch worker (or the server), so the backtest view stays responsive meanwhile.
import { niceStep } from './canvas.js';
import { TimeChart } from './chart.js';
import { $, element, escapeHtml } from './dom.js';
import { Cancelled } from './engine.js';
import { bars, figure, versus } from './figures.js';
import * as format from './format.js';
import { Heatmap, METRIC_LABELS, formatScore } from './heatmap.js';
import { palette } from './palette.js';
import { histogram, scatter } from './plots.js';
import { request, state } from './state.js';

// Parameters a sweep can vary: the strategy's own, or the $names of custom rules.
export function sweepableParams(catalog) {
  if (state.strategy === 'custom') {
    return Object.entries(state.customParams).map(([name, value]) => {
      const integer = Number.isInteger(value);
      const min = integer ? Math.max(1, Math.round(value / 4)) : value / 4;
      const max = integer ? Math.max(min + 5, Math.round(value * 3)) : value * 3;
      return { name, label: `$${name}`, min, max, step: integer ? 1 : 0.1, integer, default: value };
    });
  }
  return catalog.strategies.find((s) => s.id === state.strategy)?.params ?? [];
}

function defaultAxis(param) {
  if (!param) return null;
  const span = param.max - param.min;
  let step = niceStep(span, 20);
  if (param.integer) step = Math.max(1, Math.round(step));
  const from = Math.ceil(param.min / step) * step || param.min;
  return { param: param.name, from, to: param.max, step };
}

// The x and y axes and the metric, shared by the sweep and the walk-forward test.
class AxisFields {
  constructor(container, catalog, onChange) {
    this.container = container;
    this.catalog = catalog;
    this.onChange = onChange;
  }

  ensureAxes() {
    const params = sweepableParams(this.catalog);
    const names = params.map((p) => p.name);
    if (!state.sweep.x || !names.includes(state.sweep.x.param)) state.sweep.x = defaultAxis(params[0]);
    if (state.sweep.y && !names.includes(state.sweep.y.param)) state.sweep.y = null;
    if (state.sweep.y === null && params.length > 1 && !this.touchedY) {
      state.sweep.y = defaultAxis(params.find((p) => p.name !== state.sweep.x?.param));
    }
    return params;
  }

  combinations() {
    const count = (axis) => (axis ? Math.floor((axis.to - axis.from) / axis.step + 1e-9) + 1 : 1);
    return state.sweep.x ? count(state.sweep.x) * count(state.sweep.y) : 0;
  }

  render(extra = []) {
    const params = this.ensureAxes();
    if (!params.length) {
      this.container.replaceChildren(element('p', { class: 'note' }, 'This strategy has no parameters to vary. Pick another, or give custom rules a $parameter.'));
      return;
    }
    const axisField = (key, label) => {
      const axis = state.sweep[key];
      const select = element('select', { 'aria-label': `${label} parameter` },
        key === 'y' ? element('option', { value: '' }, 'None') : null,
        params.map((p) => element('option', { value: p.name }, p.label)));
      select.value = axis?.param ?? '';
      const number = (name, value) => {
        const input = element('input', { type: 'number', step: 'any', value: value ?? '', 'aria-label': `${label} ${name}` });
        input.addEventListener('change', () => {
          const v = Number(input.value);
          if (Number.isFinite(v) && state.sweep[key]) {
            state.sweep[key][name] = v;
            this.onChange();
          }
        });
        return input;
      };
      select.addEventListener('change', () => {
        if (key === 'y') this.touchedY = true;
        const param = params.find((p) => p.name === select.value);
        state.sweep[key] = param ? defaultAxis(param) : null;
        if (key === 'x' && state.sweep.y?.param === state.sweep.x?.param) state.sweep.y = null;
        this.render(extra);
        this.onChange();
      });
      return element('label', { class: 'run-field axis' }, element('span', {}, `${label}: from, to, step`),
        element('div', { class: 'axis-row' }, select, number('from', axis?.from), number('to', axis?.to), number('step', axis?.step)));
    };
    const metric = element('select', { 'aria-label': 'Metric' }, Object.entries(METRIC_LABELS).map(([value, label]) => element('option', { value }, label)));
    metric.value = state.sweep.metric;
    metric.addEventListener('change', () => {
      state.sweep.metric = metric.value;
      this.onChange();
    });
    this.container.replaceChildren(axisField('x', 'X'), axisField('y', 'Y'),
      element('label', { class: 'run-field' }, element('span', {}, 'Rank by'), metric), ...extra);
  }

  spec() {
    const axis = (a) => (a ? { param: a.param, from: a.from, to: a.to, step: a.step } : undefined);
    return { x: axis(state.sweep.x), y: axis(state.sweep.y), metric: state.sweep.metric };
  }
}

// A run button that turns into Stop while its job runs, with a progress bar.
class Runner {
  constructor({ button, progress, engine, onResult, onError, onStart = () => {}, onEnd = () => {} }) {
    this.button = button;
    this.progress = progress;
    this.engine = engine;
    this.onResult = onResult;
    this.onError = onError;
    this.onStart = onStart;
    this.onEnd = onEnd;
    this.running = false;
    this.current = Promise.resolve();
    this.label = button.querySelector('span').textContent;
  }

  // Starts the job, or stops the one running.
  run(job) {
    if (this.running) {
      this.engine.cancelJob();
      return this.current;
    }
    this.current = this.execute(job);
    return this.current;
  }

  // Starts the job, stopping any that's running first.
  async restart(job) {
    if (this.running) {
      this.engine.cancelJob();
      await this.current;
    }
    return this.run(job);
  }

  async execute(job) {
    this.running = true;
    this.onStart();
    this.button.classList.add('danger');
    this.button.classList.remove('primary');
    this.button.querySelector('span').textContent = 'Stop';
    this.button.querySelector('use').setAttribute('href', '#i-stop');
    this.progress.hidden = false;
    const bar = this.progress.firstElementChild;
    bar.style.width = '0%';
    this.progress.classList.remove('indeterminate');
    const started = performance.now();
    try {
      const response = await this.engine.job(job, (value) => {
        if (value === null) this.progress.classList.add('indeterminate');
        else bar.style.width = `${Math.round(value * 100)}%`;
      });
      if (response.ok) this.onResult(response, performance.now() - started);
      else this.onError(response.error);
    } catch (error) {
      if (!(error instanceof Cancelled)) this.onError({ message: error.message });
    } finally {
      this.running = false;
      this.button.classList.remove('danger');
      this.button.classList.add('primary');
      this.button.querySelector('span').textContent = this.label;
      this.button.querySelector('use').setAttribute('href', '#i-play');
      this.progress.hidden = true;
      this.onEnd();
    }
  }
}

// What the three research views share. Each runs itself when it opens, and when the settings change while it's open,
// if the work is small enough to take a second or two; otherwise its last result stays, dimmed and marked out of
// date, until it's run again.
class ResearchView {
  constructor({ view, engine, context, onError, button, progress, empty, status, limit, running }) {
    this.view = view;
    this.engine = engine;
    this.context = context;
    this.limit = limit * (engine.runtime === 'wasm' ? 1 : 5);  // the server has every core, and no WebAssembly
    this.empty = empty;
    this.idleText = empty.textContent;
    this.runningText = running;
    this.statusNode = status;
    this.panel = $(`.view-panel[data-panel="${view}"]`);
    this.result = null;
    this.resultKey = null;
    this.pendingKey = null;
    this.timer = 0;
    this.runner = new Runner({
      button,
      progress,
      engine,
      onError,
      onResult: (response, ms) => {
        this.resultKey = this.pendingKey;
        this.show(response.result, ms);
        this.sync();
      },
      onStart: () => {
        if (!this.result) this.empty.textContent = this.runningText;
      },
      onEnd: () => {
        if (!this.result) this.empty.textContent = this.idleText;
      },
    });
    button.addEventListener('click', () => this.run());
  }

  // The request to run, or null when there's nothing to run.
  job() {
    return null;
  }

  // What identifies a result: its request, less anything that doesn't change it.
  key(job) {
    return JSON.stringify(job);
  }

  // The work a run would take, as the number of bars it backtests, or null when that isn't known yet.
  workload() {
    return null;
  }

  // What a run would do, before it has run.
  summary() {
    return '';
  }

  // The result still holds; anything it marks, such as the backtest's own parameters, may have moved.
  fresh() {}

  run() {
    clearTimeout(this.timer);
    const job = this.job();
    if (!job) return Promise.resolve();
    this.pendingKey = this.key(job);
    return this.runner.run(job);
  }

  // Called when the view opens and whenever the settings change.
  sync({ immediate = false } = {}) {
    clearTimeout(this.timer);
    const job = this.job();
    const key = job && this.key(job);
    const current = Boolean(this.result) && key === this.resultKey;
    this.panel.classList.toggle('stale', Boolean(this.result) && !current);
    if (current) {
      this.fresh();
      return;
    }
    if (this.runner.running && key === this.pendingKey) return;
    const work = job && state.view === this.view && this.context.canRun() ? this.workload(job) : null;
    if (work === null || work > this.limit) {
      this.status(this.result ? `Out of date · ${this.summary()}` : this.summary());
      return;
    }
    this.status(this.result ? 'The settings changed: running it again' : this.summary());
    this.timer = setTimeout(() => {
      this.pendingKey = key;
      this.runner.restart(job);
    }, immediate || !this.result ? 0 : 450);
  }

  status(text) {
    this.statusNode.textContent = text;
  }

  timing(count, ms) {
    this.status(`${count} in ${format.number(ms / 1000, 2)} s, ${this.engine.runtime === 'wasm' ? 'in this tab' : 'on the server'}`);
  }
}

// The parameters a sweep varies, which a result doesn't depend on the backtest's own values of.
function withoutSwept(job, spec) {
  const copy = structuredClone(job);
  for (const axis of [spec.x, spec.y]) {
    if (axis) delete copy.strategy.params[axis.param];
  }
  return JSON.stringify(copy);
}

export class OptimizeView extends ResearchView {
  constructor({ engine, catalog, context, onApply, onError }) {
    super({
      view: 'optimize',
      engine,
      context,
      onError,
      button: $('#sweep-run'),
      progress: $('#sweep-progress'),
      empty: $('#heatmap-empty'),
      status: $('#sweep-estimate'),
      limit: 1.2e7,
      running: 'Backtesting every combination…',
    });
    this.catalog = catalog;
    this.onApply = onApply;
    this.fields = new AxisFields($('#sweep-fields'), catalog, () => this.sync());
    this.heatmap = new Heatmap($('#heatmap'), $('#heatmap-tip'), { onSelect: (x, y) => this.apply(x, y) });
    new ResizeObserver(() => this.redraw()).observe($('#degradation'));
  }

  refresh() {
    this.fields.render();
    this.sync();
  }

  job() {
    return state.sweep.x ? request('sweep', { sweep: this.fields.spec() }) : null;
  }

  key(job) {
    return withoutSwept(job, job.sweep);
  }

  workload() {
    const bars = this.context.bars();
    return bars ? Math.min(this.fields.combinations(), 10000) * bars : null;
  }

  summary() {
    const n = this.fields.combinations();
    const text = n ? `${format.integer(n)} combinations` : '';
    return n > 10000 ? `${format.integer(n)} combinations: 10,000 at most` : text;
  }

  fresh() {
    this.heatmap.set(this.result, this.current());
  }

  apply(x, y) {
    this.onApply({ [this.result.x.param]: x, ...(this.result.y ? { [this.result.y.param]: y } : {}) });
  }

  current() {
    const params = state.strategy === 'custom' ? state.customParams : state.params;
    const info = sweepableParams(this.catalog);
    const value = (name) => params[name] ?? info.find((p) => p.name === name)?.default;
    return this.result ? { x: value(this.result.x.param), y: this.result.y ? value(this.result.y.param) : null } : null;
  }

  show(result, ms) {
    this.result = result;
    $('#heatmap-empty').hidden = true;
    $('#heatmap-title').textContent = `${METRIC_LABELS[result.metric]} across ${result.x.param}${result.y ? ` and ${result.y.param}` : ''}, on ${format.integer(result.valid)} backtests`;
    this.heatmap.set(result, this.current());
    this.renderTop();
    this.renderOverfit();
    this.redraw();
    this.timing(`${format.integer(result.valid)} backtests`, ms);
    document.body.dataset.sweep = String(Number(document.body.dataset.sweep ?? 0) + 1);
  }

  redraw() {
    if (!this.result) return;
    this.heatmap.draw();
    const points = this.result.overfitting.degradation;
    if (points.length) scatter($('#degradation'), points, { format: (v) => v.toFixed(2), xLabel: 'Sharpe in sample, per bar', yLabel: 'Out of sample' });
    const logits = $('#logits');
    if (logits) histogram(logits, this.result.overfitting.logits, { split: 0, format: (v) => v.toFixed(1), bins: 30 });
  }

  renderTop() {
    const r = this.result;
    const head = element('tr', {}, element('th', { scope: 'col' }, r.x.param), r.y ? element('th', { scope: 'col' }, r.y.param) : null,
      [METRIC_LABELS[r.metric], 'Sharpe', 'Annual return', 'Drawdown', 'Trades'].map((name) => element('th', { scope: 'col' }, name)));
    const body = element('tbody');
    for (const row of r.top) {
      const tr = element('tr', { class: 'clickable', title: 'Backtest these parameters' },
        element('td', {}, format.number(row.x, 2)), r.y ? element('td', {}, format.number(row.y, 2)) : null,
        element('td', {}, formatScore(r.metric, r.metric === 'max_drawdown' ? -row.score : row.score)),
        element('td', {}, format.ratio(row.sharpe)), element('td', {}, format.percent(row.cagr, 1)),
        element('td', {}, format.percent(-row.max_drawdown, 1)), element('td', {}, format.integer(row.trades)));
      tr.addEventListener('click', () => this.apply(row.x, row.y));
      body.append(tr);
    }
    const notes = r.errors.length ? element('p', { class: 'table-note' }, `${format.integer(r.trials - r.valid)} combinations couldn't run: ${r.errors[0]}`) : null;
    $('#top').replaceChildren(element('table', {}, element('thead', {}, head), body), notes ?? '');
  }

  renderOverfit() {
    const o = this.result.overfitting;
    const measure = (value, kind, title, text) => element('div', { class: `measure ${kind}` },
      element('span', { class: 'big' }, format.probability(value)), element('h3', {}, title), element('p', {}, text));
    const dsrKind = !Number.isFinite(o.deflated_sharpe) ? '' : o.deflated_sharpe >= 0.95 ? 'good' : o.deflated_sharpe >= 0.5 ? 'middling' : 'bad';
    const pboKind = !Number.isFinite(o.pbo) ? '' : o.pbo <= 0.2 ? 'good' : o.pbo <= 0.5 ? 'middling' : 'bad';
    const children = [
      measure(o.deflated_sharpe, dsrKind, 'Deflated Sharpe ratio',
        `The chance the best Sharpe ratio, ${format.ratio(o.best_sharpe)}, beats the ${format.ratio(o.expected_max_sharpe)} that the best of ${format.integer(o.trials)} strategies with no edge would show by luck.`),
    ];
    if (o.splits > 0) {
      children.push(measure(o.pbo, pboKind, 'Probability of backtest overfitting',
        `In ${format.integer(o.splits)} splits of the bars into ${o.blocks} blocks, how often the in-sample winner finished in the bottom half out of sample. It lost money out of sample in ${format.percent(o.oos_loss_probability, 0)} of them.`));
      children.push(element('div', {}, element('p', { class: 'note' }, 'The winner\'s rank out of sample in each split, as a logit: left of zero, it did worse than the median combination.'),
        element('div', { class: 'chart-wrap logits' }, element('canvas', { id: 'logits' }))));
    }
    let verdict;
    if (dsrKind === 'good' && pboKind === 'good') verdict = 'The best combination beats luck and keeps its rank out of sample: the edge looks real, though the data is one market\'s history.';
    else if (dsrKind === 'good') verdict = 'The strategy family beats luck, but which parameters win doesn\'t persist: prefer a robust region of the map to its single best cell.';
    else if (pboKind === 'good') verdict = 'The ranking holds out of sample, but the best Sharpe ratio isn\'t clearly above what luck gives this many tries.';
    else verdict = 'Most of the best combination\'s edge is what this many tries would find by luck: expect less out of sample.';
    children.push(element('p', { class: 'verdict' }, verdict));
    $('#overfit').replaceChildren(...children);
  }
}

export class WalkForwardView extends ResearchView {
  constructor({ engine, catalog, context, onError }) {
    super({
      view: 'walkforward',
      engine,
      context,
      onError,
      button: $('#walk-run'),
      progress: $('#walk-progress'),
      empty: $('#walk-empty'),
      status: $('#walk-estimate'),
      limit: 1.2e7,
      running: 'Optimizing each window and trading the next…',
    });
    this.catalog = catalog;
    this.fields = new AxisFields($('#walk-fields'), catalog, () => this.sync());
    this.chart = new TimeChart($('#walk-chart'), { tooltip: $('#walk-tip'), onHover: (i) => this.tooltip(i) });
  }

  refresh() {
    const number = (key, label, min, max, step) => {
      const input = element('input', { type: 'number', min, max, step, value: state.walk[key], 'aria-label': label });
      input.addEventListener('change', () => {
        const v = Number(input.value);
        if (Number.isFinite(v)) state.walk[key] = v;
        this.sync();
      });
      return element('label', { class: 'run-field' }, element('span', {}, label), input);
    };
    const anchored = element('input', { type: 'checkbox', class: 'toggle', 'aria-label': 'Anchored: keep the first year in every in-sample window' });
    anchored.checked = state.walk.anchored;
    anchored.addEventListener('change', () => {
      state.walk.anchored = anchored.checked;
      this.sync();
    });
    this.fields.render([
      number('isYears', 'Years in sample', 0.5, 30, 0.5),
      number('oosYears', 'Years out of sample', 0.25, 10, 0.25),
      element('label', { class: 'run-field', title: 'Keep the first year in every in-sample window, so it grows' }, element('span', {}, 'Anchored'), anchored),
    ]);
    this.sync();
  }

  job() {
    if (!state.sweep.x) return null;
    const spec = this.fields.spec();
    return request('walkforward', {
      walkforward: { ...spec, in_sample_years: state.walk.isYears, out_of_sample_years: state.walk.oosYears, anchored: state.walk.anchored },
    });
  }

  key(job) {
    return withoutSwept(job, job.walkforward);
  }

  // Each window searches its in-sample years, and there's a window for each out-of-sample stretch after the first.
  workload() {
    const bars = this.context.bars();
    const perYear = this.context.barsPerYear() || 252;
    const inSample = state.walk.isYears * perYear;
    const outOfSample = state.walk.oosYears * perYear;
    if (!bars || bars <= inSample || outOfSample <= 0) return null;
    const searched = state.walk.anchored ? (inSample + bars) / 2 : inSample;
    return Math.min(this.fields.combinations(), 10000) * searched * ((bars - inSample) / outOfSample);
  }

  summary() {
    const n = this.fields.combinations();
    return n ? `${format.integer(n)} combinations a window` : '';
  }

  show(result, ms) {
    this.result = result;
    $('#walk-empty').hidden = true;
    const m = result.metrics;
    const b = result.benchmark;
    const holding = (text) => versus('Buy and hold', text);
    const sign = (v) => (v < 0 ? 'down' : '');
    const distinct = new Set(result.folds.map((f) => `${f.x}|${f.y}`)).size;
    $('#walk-kpis').replaceChildren(
      figure('Annual return', format.percent(m.cagr, 1), holding(format.percent(b.cagr, 1)), { compare: bars(m.cagr, b.cagr), valueClass: sign(m.cagr), title: 'Out of sample: compound annual growth rate' }),
      figure('Sharpe ratio', format.ratio(m.sharpe), holding(format.ratio(b.sharpe)), { compare: bars(m.sharpe, b.sharpe), valueClass: sign(m.sharpe) }),
      figure('Worst drawdown', format.percent(-m.max_drawdown, 1), holding(format.percent(-b.max_drawdown, 1)), { compare: bars(m.max_drawdown, b.max_drawdown) }),
      figure('Efficiency', format.ratio(result.efficiency), 'out of sample ÷ in', { title: 'The mean out-of-sample annual return over the mean in-sample one' }),
      figure('Windows', format.integer(result.folds.length), `${distinct} different choices`),
      figure('Trades', format.integer(m.trades), `${format.percent(m.win_rate, 0)} won`),
    );
    const times = result.series.time;
    const regions = result.folds.map((fold, i) => ({
      from: fold.oos_index,
      to: (result.folds[i + 1]?.oos_index ?? times.length) - 1,
      fill: i % 2 ? palette.hover : 'rgba(0,0,0,0)',
    }));
    this.chart.setData({
      times,
      panes: [
        {
          id: 'equity', weight: 2.4, log: true, title: 'Equity', note: 'log scale', format: format.moneyCompact, tagFormat: (v) => format.money(v), regions, endLabels: true,
          series: [
            { kind: 'line', values: result.series.benchmark, color: palette.benchmark, width: 1.4, endLabel: format.moneyCompact },
            { kind: 'line', values: result.series.equity, color: palette.strategy, width: 2, endLabel: format.moneyCompact },
          ],
        },
        {
          id: 'drawdown', weight: 1, title: 'Drawdown', format: (v) => format.percent(v, 0), clampTop: 0, regions,
          series: [
            { kind: 'area', values: result.series.drawdown, color: palette.down, width: 1.1, fill: palette.downSoft, baseline: 0 },
            { kind: 'line', values: result.series.benchmark_drawdown, color: palette.benchmark, width: 1.1 },
          ],
        },
      ],
    });
    const head = element('tr', {}, ['Traded', 'Chosen on the years before', 'Sharpe in', 'Sharpe out', 'Annual, out', 'Drawdown, out', 'Trades']
      .map((name, i) => element('th', { scope: 'col', class: i < 2 ? 'left' : '' }, name)));
    const body = element('tbody');
    for (const fold of result.folds) {
      const chosen = `${result.x_param} ${format.number(fold.x, 2)}${result.y_param ? `, ${result.y_param} ${format.number(fold.y, 2)}` : ''}`;
      body.append(element('tr', {},
        element('td', { class: 'left' }, `${fold.oos_from} to ${fold.oos_to}`), element('td', { class: 'left text' }, chosen),
        element('td', {}, format.ratio(fold.in_sample.sharpe)),
        element('td', { class: format.sign(fold.out_of_sample.sharpe) }, format.ratio(fold.out_of_sample.sharpe)),
        element('td', { class: format.sign(fold.out_of_sample.cagr) }, format.percent(fold.out_of_sample.cagr, 1)),
        element('td', {}, format.percent(-fold.out_of_sample.max_drawdown, 1)), element('td', {}, format.integer(fold.out_of_sample.trades))));
    }
    $('#folds').replaceChildren(element('table', {}, element('thead', {}, head), body));
    this.timing(`${result.folds.length} windows`, ms);
    document.body.dataset.walk = String(Number(document.body.dataset.walk ?? 0) + 1);
  }

  tooltip(index) {
    const r = this.result;
    if (!r) return '';
    const fold = [...r.folds].reverse().find((f) => f.oos_index <= index);
    const row = (label, value) => `<div class="row"><span>${escapeHtml(label)}</span><b>${value}</b></div>`;
    let html = `<h4>${format.longDate(r.series.time[index])}</h4>`;
    html += row('Strategy', format.money(r.series.equity[index]));
    html += row('Buy and hold', format.money(r.series.benchmark[index]));
    if (fold) html += row('Parameters', `${format.number(fold.x, 2)}${r.y_param ? ` · ${format.number(fold.y, 2)}` : ''}`);
    return html;
  }
}

const ordinal = (n) => {
  if (!Number.isFinite(n)) return '–';
  const tens = n % 100;
  const suffix = tens >= 11 && tens <= 13 ? 'th' : ({ 1: 'st', 2: 'nd', 3: 'rd' })[n % 10] ?? 'th';
  return `${n}${suffix}`;
};

export class MonteCarloView extends ResearchView {
  constructor({ engine, context, onError }) {
    super({
      view: 'montecarlo',
      engine,
      context,
      onError,
      button: $('#monte-run'),
      progress: $('#monte-progress'),
      empty: $('#fan-empty'),
      status: $('#monte-estimate'),
      limit: 4e7,
      running: 'Resampling the backtest\'s returns…',
    });
    this.chart = new TimeChart($('#fan'), { tooltip: $('#fan-tip'), onHover: (i) => this.tooltip(i), minSpan: 3 });
    new ResizeObserver(() => this.redraw()).observe($('#histograms'));
  }

  refresh() {
    const number = (key, label, min, max, step) => {
      const input = element('input', { type: 'number', min, max, step, value: state.monte[key], 'aria-label': label });
      input.addEventListener('change', () => {
        const v = Number(input.value);
        if (Number.isFinite(v)) state.monte[key] = Math.round(v);
        this.sync();
      });
      return element('label', { class: 'run-field' }, element('span', {}, label), input);
    };
    $('#monte-fields').replaceChildren(number('paths', 'Paths', 10, 10000, 100), number('block', 'Mean block, in bars', 1, 500, 1), number('seed', 'Seed', 0, 1e9, 1),
      element('p', { class: 'note', style: { maxWidth: '440px', alignSelf: 'center' } }, 'A stationary bootstrap: runs of consecutive days, of random length, drawn from the backtest\'s own returns.'));
    this.sync();
  }

  job() {
    return request('montecarlo', { montecarlo: { paths: state.monte.paths, block: state.monte.block, seed: state.monte.seed } });
  }

  workload() {
    const bars = this.context.bars();
    return bars ? state.monte.paths * bars : null;
  }

  summary() {
    return `${format.integer(state.monte.paths)} paths`;
  }

  quantile(values, q) {
    if (!values.length) return NaN;
    return values[Math.min(values.length - 1, Math.max(0, Math.round(q * (values.length - 1))))];
  }

  show(result, ms) {
    this.result = result;
    $('#fan-empty').hidden = true;
    const d = result.distributions;
    const q = (name, p) => this.quantile(d[name], p);
    const sign = (v) => (v < 0 ? 'down' : '');
    $('#monte-kpis').replaceChildren(
      figure('Paths that lost', format.percent(result.loss_probability, 1), `of ${format.integer(result.paths)} paths`, { valueClass: result.loss_probability > 0.2 ? 'down' : '' }),
      figure('Median return', format.percent(q('cagr', 0.5), 1), `a year; 90% of paths ${format.percent(q('cagr', 0.05), 1)} to ${format.percent(q('cagr', 0.95), 1)}`, { valueClass: sign(q('cagr', 0.5)) }),
      figure('Median drawdown', format.percent(-q('max_drawdown', 0.5), 0), `1 path in 20: ${format.percent(-q('max_drawdown', 0.95), 0)} or worse`),
      figure('Median Sharpe', format.ratio(q('sharpe', 0.5)), `90% of paths ${format.ratio(q('sharpe', 0.05))} to ${format.ratio(q('sharpe', 0.95))}`),
      figure('The backtest', format.percent(result.metrics.cagr, 1), `a year, ${ordinal(Math.round(this.rank(d.cagr, result.metrics.cagr) * 100))} percentile`, { valueClass: sign(result.metrics.cagr) }),
      figure('Mean block', `${format.integer(result.block)} bars`, `seed ${result.seed}`),
    );
    const b = result.bands;
    this.chart.setData({
      times: result.time,
      panes: [{
        id: 'fan', weight: 1, log: true, title: 'Equity', note: 'log scale', format: format.moneyCompact, tagFormat: (v) => format.money(v),
        series: [
          { kind: 'band', upper: b.p95, lower: b.p5, fill: palette.strategySoft },
          { kind: 'band', upper: b.p75, lower: b.p25, fill: palette.strategySoft },
          { kind: 'line', values: b.p50, color: palette.strategy, width: 1.4, dash: [5, 4] },
          { kind: 'line', values: result.actual, color: palette.text, width: 1.8, endLabel: format.moneyCompact },
        ],
        endLabels: true,
      }],
    });
    this.redraw();
    this.timing(`${format.integer(result.paths)} paths`, ms);
    document.body.dataset.monte = String(Number(document.body.dataset.monte ?? 0) + 1);
  }

  rank(sorted, value) {
    if (!sorted.length || !Number.isFinite(value)) return NaN;
    let below = 0;
    for (const v of sorted) if (v < value) below += 1;
    return below / sorted.length;
  }

  redraw() {
    const r = this.result;
    const container = $('#histograms');
    if (!r) {
      container.replaceChildren();
      return;
    }
    if (!container.children.length) {
      const exhibit = (number, title, id) => element('figure', { class: 'exhibit' },
        element('figcaption', { class: 'exhibit-head' }, element('div', { class: 'exhibit-text' },
          element('span', { class: 'exhibit-label' }, `Figure ${number}`), element('h2', { class: 'exhibit-title' }, title))),
        element('div', { class: 'chart-wrap' }, element('canvas', { id })));
      container.append(exhibit('6a', 'Annual return of the paths', 'hist-cagr'), exhibit('6b', 'Their worst drawdowns', 'hist-dd'), exhibit('6c', 'Their Sharpe ratios', 'hist-sharpe'));
    }
    const d = r.distributions;
    histogram($('#hist-cagr'), d.cagr, { marker: r.metrics.cagr, markerLabel: 'backtest', format: (v) => format.percent(v, 0), split: 0 });
    histogram($('#hist-dd'), d.max_drawdown.map((v) => -v), { marker: -r.metrics.max_drawdown, markerLabel: 'backtest', format: (v) => format.percent(v, 0) });
    histogram($('#hist-sharpe'), d.sharpe, { marker: r.metrics.sharpe, markerLabel: 'backtest', format: (v) => v.toFixed(1), split: 0 });
  }

  tooltip(index) {
    const r = this.result;
    if (!r) return '';
    const row = (label, value) => `<div class="row"><span>${escapeHtml(label)}</span><b>${value}</b></div>`;
    const b = r.bands;
    return `<h4>${format.longDate(r.time[index])}</h4>${row('95th percentile', format.money(b.p95[index]))}${row('75th', format.money(b.p75[index]))}${row('Median', format.money(b.p50[index]))}${row('25th', format.money(b.p25[index]))}${row('5th', format.money(b.p5[index]))}${row('The backtest', format.money(r.actual[index]))}`;
  }
}
