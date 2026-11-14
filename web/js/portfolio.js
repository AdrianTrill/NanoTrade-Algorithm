// The portfolio view: the strategy on several markets at once, each in a sleeve of the capital as it would trade alone,
// the sleeves set back to their weights on a schedule, against the same markets bought and held.
import { mix } from './canvas.js';
import { TimeChart } from './chart.js';
import { $, clickableRows, element, escapeHtml } from './dom.js';
import { bars, figure, versus } from './figures.js';
import * as format from './format.js';
import { overlayColor, palette } from './palette.js';
import { ResearchView } from './research.js';
import { request, saveQuery, state } from './state.js';

const WEIGHTINGS = [['equal', 'Equal'], ['inverse_volatility', 'Inverse volatility'], ['fixed', 'Fixed']];
const SCHEDULES = [['monthly', 'Monthly'], ['quarterly', 'Quarterly'], ['yearly', 'Yearly'], ['never', 'Never']];

export class PortfolioView extends ResearchView {
  // `markets` lists what can be held: [{ name, label, title }]. `onOpen` backtests one market alone, and `onChange`
  // follows a change of the portfolio's markets or weights.
  constructor({ engine, context, markets, onOpen, onChange, onError }) {
    super({
      view: 'portfolio',
      engine,
      context,
      onError,
      button: $('#port-run'),
      progress: $('#port-progress'),
      empty: $('#port-empty'),
      status: $('#port-estimate'),
      limit: 1e8,
      running: 'Backtesting the strategy on every market…',
    });
    this.markets = markets;
    this.onOpen = onOpen;
    this.onChange = onChange;
    this.chart = new TimeChart($('#port-chart'), { tooltip: $('#port-tip'), onHover: (i) => this.tooltip(i) });
  }

  // The markets chosen that are still there to hold, in the order they're listed: a file opened in the page is gone
  // after a reload.
  chosen() {
    const wanted = new Set(state.portfolio.markets);
    return this.markets().map((m) => m.name).filter((name) => wanted.has(name));
  }

  changed() {
    saveQuery();
    this.onChange();
    this.sync();
  }

  refresh() {
    const chosen = new Set(this.chosen());
    const chips = element('div', { class: 'chips', role: 'group', 'aria-labelledby': 'port-markets-label' }, this.markets().map((m) => {
      const chip = element('button', { class: 'chip', type: 'button', 'aria-pressed': String(chosen.has(m.name)), title: m.title, dataset: { market: m.name } }, m.label);
      chip.addEventListener('click', () => {
        const on = chip.getAttribute('aria-pressed') !== 'true';
        chip.setAttribute('aria-pressed', String(on));
        state.portfolio.markets = on ? [...state.portfolio.markets, m.name] : state.portfolio.markets.filter((name) => name !== m.name);
        this.renderWeights();
        this.changed();
      });
      return chip;
    }));
    const select = (text, pairs, key) => {
      const node = element('select', { 'aria-label': text }, pairs.map(([value, name]) => element('option', { value }, name)));
      node.value = state.portfolio[key];
      node.addEventListener('change', () => {
        state.portfolio[key] = node.value;
        this.renderWeights();
        this.changed();
      });
      return element('label', { class: 'run-field' }, element('span', {}, text), node);
    };
    $('#port-fields').replaceChildren(
      element('div', { class: 'run-field markets-field' }, element('span', { id: 'port-markets-label' }, 'Markets'), chips),
      select('Weights', WEIGHTINGS, 'weighting'),
      select('Rebalance', SCHEDULES, 'rebalance'),
      element('div', { class: 'run-field weights-field', id: 'port-weights' }),
    );
    this.renderWeights();
    this.sync();
  }

  // The weight of each market chosen, when the weights are fixed.
  renderWeights() {
    const container = $('#port-weights');
    container.hidden = state.portfolio.weighting !== 'fixed';
    if (container.hidden) return;
    const inputs = this.chosen().map((name) => {
      const input = element('input', { type: 'number', min: 0, step: 'any', value: state.portfolio.weights[name] ?? 1, 'aria-label': `The weight of ${name}` });
      input.addEventListener('change', () => {
        const value = Number(input.value);
        if (Number.isFinite(value) && value >= 0) state.portfolio.weights[name] = value;
        this.changed();
      });
      return element('label', { class: 'custom-param' }, element('span', {}, name), input);
    });
    container.replaceChildren(element('span', {}, 'Fixed weights, scaled to add up to 100%'), element('div', { class: 'custom-params' }, inputs));
  }

  job() {
    const markets = this.chosen();
    if (markets.length < 2) return null;
    const portfolio = { markets, weighting: state.portfolio.weighting, rebalance: state.portfolio.rebalance };
    if (state.portfolio.weighting === 'fixed') portfolio.weights = markets.map((name) => state.portfolio.weights[name] ?? 1);
    const { data, ...job } = request('portfolio', { portfolio });
    return job;
  }

  workload() {
    const bars = this.context.bars();
    return bars ? bars * this.chosen().length * 2 : null;
  }

  summary() {
    const count = this.chosen().length;
    if (count < 2) return 'Choose two markets or more';
    const weights = { equal: 'equal weights', inverse_volatility: 'inverse-volatility weights', fixed: 'fixed weights' }[state.portfolio.weighting];
    const schedule = state.portfolio.rebalance === 'never' ? 'never rebalanced' : `rebalanced ${state.portfolio.rebalance}`;
    return `${count} markets, ${weights}, ${schedule}`;
  }

  // A market's color: on the chart, its swatch, and its row.
  color(index) {
    const surface = palette.surface?.startsWith('#') ? palette.surface : '#ffffff';
    return mix(overlayColor(index), surface, 0.3);
  }

  show(result, ms) {
    this.result = result;
    $('#port-empty').hidden = true;
    this.renderAll(false);
    this.timing(`${result.markets.length} markets`, ms);
    document.body.dataset.portfolio = String(Number(document.body.dataset.portfolio ?? 0) + 1);
  }

  renderAll(keepView = true) {
    this.renderFigures();
    this.renderLegend();
    this.renderChart(keepView);
    this.renderMarkets();
    this.renderCorrelation();
  }

  redraw() {
    if (this.result) this.renderAll();
    else this.chart.draw();
  }

  averageCorrelation() {
    const c = this.result.correlation;
    let sum = 0;
    let pairs = 0;
    for (let a = 0; a < c.length; a += 1) {
      for (let b = a + 1; b < c.length; b += 1) {
        if (Number.isFinite(c[a][b])) {
          sum += c[a][b];
          pairs += 1;
        }
      }
    }
    return pairs ? sum / pairs : NaN;
  }

  renderFigures() {
    const r = this.result;
    const m = r.metrics;
    const b = r.benchmark;
    const held = (text) => versus('Held', text);
    const sign = (v) => (v < 0 ? 'down' : '');
    const initial = m.final_equity / (1 + m.total_return);
    const rebalances = r.portfolio.rebalances;
    $('#port-kpis').replaceChildren(
      figure('Annual return', format.percent(m.cagr, 1), held(format.percent(b.cagr, 1)),
        { compare: bars(m.cagr, b.cagr), valueClass: sign(m.cagr), title: 'Compound annual growth rate' }),
      figure('Sharpe ratio', format.ratio(m.sharpe), held(format.ratio(b.sharpe)), { compare: bars(m.sharpe, b.sharpe), valueClass: sign(m.sharpe) }),
      figure('Worst drawdown', format.percent(-m.max_drawdown, 1), held(format.percent(-b.max_drawdown, 1)), { compare: bars(m.max_drawdown, b.max_drawdown) }),
      figure('Volatility', format.percent(m.volatility, 1), held(format.percent(b.volatility, 1)),
        { compare: bars(m.volatility, b.volatility), title: 'The annualized standard deviation of daily returns' }),
      figure('Correlation', format.ratio(this.averageCorrelation()), `on average, between the ${r.markets.length} markets`,
        { title: 'The average correlation of the strategy\'s daily returns in each pair of markets: the lower, the more they diversify' }),
      figure('Rebalances', format.integer(rebalances), rebalances ? `costing ${format.percent(r.portfolio.rebalance_cost / initial, 2)} of the capital` : 'none: the weights drift'),
    );
  }

  renderLegend() {
    const item = (color, swatch, text, title = '') => element('span', { class: 'legend-item', style: { color }, title },
      element('span', { class: `swatch ${swatch}` }), element('span', { style: { color: 'var(--text-2)' } }, text));
    $('#port-legend').replaceChildren(
      item(palette.strategy, 'thick', 'Portfolio'),
      item(palette.benchmark, 'thick', 'Held', 'The same markets bought and held, weighted and rebalanced the same way'),
      ...this.result.markets.map((market, i) => item(this.color(i), 'block', market.name, market.title)),
    );
  }

  renderChart(keepView = true) {
    const r = this.result;
    if (!r) {
      this.chart.draw();
      return;
    }
    const s = r.series;
    const bands = [];
    let lower = new Array(s.time.length).fill(0);
    s.weights.forEach((weights, i) => {
      const upper = lower.map((value, k) => value + weights[k]);
      bands.push({ kind: 'band', upper, lower, fill: this.color(i) });
      lower = upper;
    });
    const schedule = r.portfolio.rebalance === 'never' ? 'never rebalanced' : `rebalanced ${r.portfolio.rebalance}`;
    this.chart.setData({
      times: s.time,
      panes: [{
        id: 'equity', title: 'Equity', note: 'log scale', weight: 2.2, log: true, format: format.moneyCompact, tagFormat: (v) => format.money(v), endLabels: true,
        series: [
          { kind: 'line', values: s.benchmark, color: palette.benchmark, width: 1.4, endLabel: format.moneyCompact },
          { kind: 'line', values: s.equity, color: palette.strategy, width: 2, endLabel: format.moneyCompact },
        ],
      }, {
        id: 'allocation', title: 'Allocation', note: schedule, weight: 1, range: [0, 1], format: (v) => format.percent(v, 0), series: bands,
      }, {
        id: 'drawdown', title: 'Drawdown', weight: 0.9, format: (v) => format.percent(v, 0), tagFormat: (v) => format.percent(v, 1), clampTop: 0,
        series: [
          { kind: 'area', values: s.drawdown, color: palette.down, width: 1.1, fill: palette.downSoft, baseline: 0 },
          { kind: 'line', values: s.benchmark_drawdown, color: palette.benchmark, width: 1.1 },
        ],
      }],
    }, { keepView });
  }

  renderMarkets() {
    const r = this.result;
    const head = element('tr', {}, ['Market', 'Weight', 'Final', 'Annual return', 'Sharpe', 'Worst drawdown', 'Trades', 'Contribution']
      .map((name, i) => element('th', { scope: 'col', class: i ? '' : 'left' }, name)));
    const body = element('tbody');
    r.markets.forEach((market, i) => {
      const m = market.metrics;
      const row = element('tr', { class: 'clickable', title: `Backtest the strategy on ${market.title} alone` },
        element('td', { class: 'left text' }, element('span', { class: 'market-cell' },
          element('span', { class: 'swatch block', style: { color: this.color(i) }, 'aria-hidden': 'true' }),
          element('b', {}, market.name), element('span', { class: 'muted' }, market.title))),
        element('td', {}, format.percent(market.target, 1)),
        element('td', {}, format.percent(market.final_weight, 1)),
        element('td', { class: format.sign(m.cagr) }, format.percent(m.cagr, 1)),
        element('td', {}, format.ratio(m.sharpe)),
        element('td', {}, format.percent(-m.max_drawdown, 1)),
        element('td', {}, format.integer(m.trades)),
        element('td', { class: format.sign(market.contribution) }, format.percent(market.contribution, 1, true)));
      row.addEventListener('click', () => this.onOpen(market.name));
      body.append(row);
    });
    clickableRows(body);
    const initial = r.metrics.final_equity / (1 + r.metrics.total_return);
    const note = element('p', { class: 'table-note' }, `Contributions are what each market's own trading added, as a share of the starting capital; `
      + `rebalancing cost ${format.percent(r.portfolio.rebalance_cost / initial, 2)} more, for a total return of ${format.percent(r.metrics.total_return, 1)}.`);
    $('#port-markets').replaceChildren(element('table', {}, element('thead', {}, head), body), note);
  }

  renderCorrelation() {
    const r = this.result;
    const surface = palette.surface?.startsWith('#') ? palette.surface : '#ffffff';
    const head = element('tr', {}, element('th', { scope: 'col', class: 'left' }, element('span', { class: 'visually-hidden' }, 'Market')),
      r.markets.map((market) => element('th', { scope: 'col' }, market.name)));
    const body = element('tbody');
    r.correlation.forEach((row, a) => {
      body.append(element('tr', {}, element('th', { scope: 'row', class: 'left' }, r.markets[a].name), row.map((value, b) => {
        if (a === b || !Number.isFinite(value)) return element('td', { class: 'muted' }, a === b ? '–' : format.ratio(value));
        const tint = mix(surface, value >= 0 ? palette.strategy : palette.o1, Math.min(1, Math.abs(value)) * 0.55);
        return element('td', { class: 'heat', style: { background: tint } }, format.ratio(value));
      })));
    });
    $('#port-correlation').replaceChildren(element('table', { class: 'correlation' }, element('thead', {}, head), body));
  }

  tooltip(index) {
    const r = this.result;
    if (!r) return '';
    const s = r.series;
    const row = (text, value, color = null) => `<div class="row"><span>${color ? `<i class="key" style="color:${color}"></i>` : ''}${escapeHtml(text)}</span><b>${value}</b></div>`;
    let html = `<h4>${format.longDate(s.time[index])}</h4>`;
    html += row('Portfolio', format.money(s.equity[index]), palette.strategy);
    html += row('Held', format.money(s.benchmark[index]), palette.benchmark);
    r.markets.forEach((market, i) => {
      html += row(market.name, format.percent(s.weights[i][index], 1), this.color(i));
    });
    return html;
  }
}
