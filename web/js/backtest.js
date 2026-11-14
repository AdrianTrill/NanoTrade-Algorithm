// The backtest view: the result told in a headline and a paragraph, the headline figures against buy and hold, the
// chart, and the tables.
import { TimeChart } from './chart.js';
import { $, $$, element, escapeHtml, yieldToBrowser } from './dom.js';
import { bars, figure, versus } from './figures.js';
import * as format from './format.js';
import { overlayColor, palette } from './palette.js';
import { drawdownsTable, downloadTrades, metricsTable, monthlyTable, tradesTable } from './tables.js';

const KICKERS = {
  Benchmark: 'The benchmark',
  Trend: 'Trend following',
  Breakout: 'Breakout',
  'Mean reversion': 'Mean reversion',
  Custom: 'Custom rules',
};

const year = (seconds) => new Date(seconds * 1000).getUTCFullYear();

// What the dataset is called in a sentence.
function subject(dataset) {
  if (dataset.source === 'synthetic') return dataset.title.replace(/^Random market/, 'a random market');
  return dataset.title || dataset.name;
}

export class BacktestView {
  constructor({ catalog }) {
    this.catalog = catalog;
    this.chart = new TimeChart($('#chart'), {
      tooltip: $('#tooltip'),
      onHover: (index) => this.tooltip(index),
    });
    this.result = null;
    this.bars = null;
    this.hidden = new Set();
    this.trades = [];
    this.log = true;
    this.detail = 'trades';
    this.built = new Set();  // the tables built for this result
    $('#candles').addEventListener('click', () => this.toggleCandles());
    $('#log').addEventListener('click', () => this.toggleLog());
    $('#zoom-reset').addEventListener('click', () => this.chart.reset());
    $('#export').addEventListener('click', () => {
      if (this.result) downloadTrades(this.result.trades, this.result.dataset.name.replace(/[^\w-]+/g, '_'));
    });
    for (const tab of $$('.details .tab')) {
      tab.addEventListener('click', () => this.showDetail(tab.dataset.detail));
    }
  }

  showDetail(name) {
    this.detail = name;
    for (const tab of $$('.details .tab')) tab.setAttribute('aria-selected', String(tab.dataset.detail === name));
    for (const panel of $$('.details [data-detail-panel]')) panel.hidden = panel.dataset.detailPanel !== name;
    $('#export').hidden = name !== 'trades';
    this.renderDetail();
  }

  // Builds the open table, if this result hasn't yet: the others wait until they're opened.
  renderDetail() {
    const result = this.result;
    if (!result || this.built.has(this.detail)) return;
    this.built.add(this.detail);
    if (this.detail === 'trades') {
      tradesTable($('#trades'), result.trades, {
        onHover: (i) => this.chart.setHighlight(i === null ? null : this.trades[i]),
        onSelect: (i) => {
          const trade = this.trades[i];
          this.chart.setHighlight(trade);
          this.chart.zoomTo(trade.entry, trade.exit, 0.8);
          $('#chart').scrollIntoView({ block: 'nearest', behavior: 'smooth' });
        },
      });
    } else if (this.detail === 'monthly') {
      monthlyTable($('#monthly'), result.calendar);
    } else if (this.detail === 'metrics') {
      metricsTable($('#metrics'), result.metrics, result.benchmark);
    } else if (this.detail === 'drawdowns') {
      drawdownsTable($('#drawdowns'), result.drawdowns, (d) => {
        const times = result.series.time;
        const index = (day) => times.findIndex((t) => format.date(t) >= day);
        const from = index(d.peak);
        const to = d.recovery ? index(d.recovery) : times.length - 1;
        this.chart.zoomTo(Math.max(0, from), Math.max(from, to), 0.25);
        $('#chart').scrollIntoView({ block: 'nearest', behavior: 'smooth' });
      });
    }
  }

  toggleCandles() {
    this.chart.candles = !this.chart.candles;
    $('#candles').setAttribute('aria-pressed', String(this.chart.candles));
    if (this.chart.candles && this.chart.end - this.chart.start > 260) {
      this.chart.setView(this.chart.end - 160, this.chart.end);  // candles need room: show the last 160 bars
    }
    this.chart.draw();
  }

  toggleLog() {
    this.log = !this.log;
    $('#log').setAttribute('aria-pressed', String(this.log));
    if (this.result) this.renderChart(true);
  }

  setLoading(flag) {
    $('#chart').parentElement.classList.toggle('loading', flag && !this.result);
  }

  setStale(flag) {
    $('#chart').parentElement.classList.toggle('stale', flag);
  }

  // Draws a result: what's in view first, then, after the browser has had a chance to paint it, the open table.
  async render(result, bars, meta) {
    const sameData = this.result && this.bars === bars;
    this.result = result;
    this.bars = bars;
    this.meta = meta;
    this.trades = result.trades.map((t) => ({
      entry: t.entry_index,
      exit: t.exit_index,
      entryPrice: t.entry_price,
      exitPrice: t.exit_price,
      direction: t.direction,
      win: t.pnl > 0,
      open: t.open,
    }));
    this.renderLede();
    this.renderFigures();
    this.renderLegend();
    this.renderChart(sameData);
    $('#trade-count').textContent = format.integer(result.trades.length);
    this.built = new Set();
    await yieldToBrowser();
    this.renderDetail();
  }

  // The headline, a paragraph that says what happened, and a dateline with the particulars.
  renderLede() {
    const r = this.result;
    const m = r.metrics;
    const b = r.benchmark;
    const info = this.catalog.strategies.find((s) => s.id === r.strategy.id);
    $('#kicker').textContent = `Backtest · ${KICKERS[info?.family] ?? 'Strategy'}`;
    const from = year(r.series.time[0]);
    const to = year(r.series.time.at(-1));
    const years = from === to ? `${from}` : `${from}–${to}`;
    $('#headline').textContent = `${r.strategy.name} on ${subject(r.dataset)}, ${years}`;

    const deck = $('#deck');
    deck.replaceChildren();
    const bold = (text) => element('b', {}, text);
    const initial = m.final_equity / (1 + m.total_return);
    deck.append('It turned ', format.moneyWords(initial), ' into ', bold(format.moneyWords(m.final_equity)),
      '; buying and holding ended at ', format.moneyWords(b.final_equity), '. ');
    deck.append(`Its Sharpe ratio was ${format.ratio(m.sharpe)}, against ${format.ratio(b.sharpe)}, and its worst drawdown `,
      `${format.percent(m.max_drawdown, 0)}, against ${format.percent(b.max_drawdown, 0)}. `);
    if (r.strategy.id !== 'buy_hold') {
      deck.append(`It held a position ${format.percent(m.exposure, 0)} of the time, over ${format.integer(m.trades)} ${m.trades === 1 ? 'trade' : 'trades'}`);
      deck.append(m.trades > 0 ? `, and won ${format.percent(m.win_rate, 0)} of them.` : '.');
    }
    if (r.stats.ruined) deck.append(' Its equity reached zero, and it stopped trading.');

    const params = Object.entries(r.strategy.params).map(([k, v]) => `${k} ${format.number(v, 2)}`).join(', ');
    const parts = [
      r.dataset.source === 'synthetic' ? 'Random market' : r.dataset.name.replace(/^file:/, ''),
      params ? `${r.strategy.name}: ${params}` : r.strategy.name,
      `${format.integer(r.range.bars)} bars, ${format.longDate(r.series.time[0])} to ${format.longDate(r.series.time.at(-1))}`,
    ];
    if (this.meta) {
      parts.push(`computed in ${format.number(this.meta.ms, 1)} ms ${this.meta.runtime === 'wasm' ? 'in this tab' : 'by the native server'}`);
    }
    $('#dateline').textContent = parts.join(' · ');
  }

  renderFigures() {
    const m = this.result.metrics;
    const b = this.result.benchmark;
    const sign = (v) => (v < 0 ? 'down' : '');
    const holding = (text) => versus('Buy and hold', text);
    $('#kpis').replaceChildren(
      figure('Total return', format.bigPercent(m.total_return), holding(format.bigPercent(b.total_return)),
        { compare: bars(m.total_return, b.total_return), valueClass: sign(m.total_return) }),
      figure('Annual return', format.percent(m.cagr, 1), holding(format.percent(b.cagr, 1)),
        { compare: bars(m.cagr, b.cagr), valueClass: sign(m.cagr), title: 'Compound annual growth rate' }),
      figure('Sharpe ratio', format.ratio(m.sharpe), holding(format.ratio(b.sharpe)), { compare: bars(m.sharpe, b.sharpe), valueClass: sign(m.sharpe) }),
      figure('Worst drawdown', format.percent(-m.max_drawdown, 1), holding(format.percent(-b.max_drawdown, 1)),
        { compare: bars(m.max_drawdown, b.max_drawdown) }),
      figure('Trades', format.integer(m.trades), `${format.percent(m.win_rate, 0)} of them won`, { title: `Profit factor ${format.ratio(m.profit_factor)}` }),
      figure('In the market', format.percent(m.exposure, 0), `${format.number(m.turnover, 1)}× turnover a year`),
    );
  }

  renderLegend() {
    const items = [
      element('span', { class: 'legend-item', style: { color: palette.text } }, element('span', { class: 'swatch' }), element('span', {}, 'Close')),
    ];
    this.result.overlays.forEach((overlay, index) => {
      const pressed = !this.hidden.has(overlay.key);
      const button = element('button', { class: 'legend-item', 'aria-pressed': String(pressed), style: { color: overlayColor(index) }, title: `${overlay.key}: click to ${pressed ? 'hide' : 'show'}` },
        element('span', { class: 'swatch thick' }), element('span', { style: { color: 'var(--text-2)' } }, overlay.label));
      button.addEventListener('click', () => {
        if (this.hidden.has(overlay.key)) this.hidden.delete(overlay.key);
        else this.hidden.add(overlay.key);
        this.renderLegend();
        this.renderChart(true);
      });
      items.push(button);
    });
    if (this.result.trades.length) {
      const title = 'Drawn once they are far enough apart to read: zoom in';
      items.push(element('span', { class: 'legend-item', style: { color: palette.up }, title }, element('span', { class: 'swatch triangle' }), element('span', { style: { color: 'var(--text-2)' } }, 'Entry')));
      items.push(element('span', { class: 'legend-item', style: { color: palette.text2 }, title }, element('span', { class: 'swatch ring' }), 'Exit'));
    }
    items.push(element('span', { class: 'legend-item', style: { color: palette.strategy } }, element('span', { class: 'swatch thick' }), element('span', { style: { color: 'var(--text-2)' } }, 'Strategy')));
    items.push(element('span', { class: 'legend-item', style: { color: palette.benchmark } }, element('span', { class: 'swatch thick' }), element('span', { style: { color: 'var(--text-2)' } }, 'Buy and hold')));
    $('#legend').replaceChildren(...items);
  }

  renderChart(keepView) {
    const r = this.result;
    const bars = this.bars;
    const times = r.series.time;
    const priceOverlays = [];
    const lowerOverlays = [];
    r.overlays.forEach((overlay, index) => {
      const series = { kind: 'line', values: overlay.values, color: overlayColor(index), width: 1.4, hidden: this.hidden.has(overlay.key), bounded: overlay.bounded, label: overlay.label };
      (overlay.pane === 'price' ? priceOverlays : lowerOverlays).push(series);
    });
    const name = r.dataset.source === 'synthetic' ? 'Random market' : r.dataset.name.replace(/^file:/, '');
    const panes = [{
      id: 'price',
      title: `${name}, adjusted close`,
      note: this.log ? 'log scale' : '',
      weight: 3,
      log: this.log,
      format: format.price,
      series: [{ kind: 'candles', open: bars.open, high: bars.high, low: bars.low, close: bars.close }, ...priceOverlays],
      trades: this.trades,
    }, {
      id: 'position',
      kind: 'ribbon',
      title: 'Position',
      fixed: 9,
      values: r.series.exposure,
    }];
    if (lowerOverlays.length) {
      const bounded = lowerOverlays.every((o) => o.bounded);
      const guides = r.levels.map((value) => ({ value, color: palette.faint }));
      panes.push({
        id: 'lower',
        title: lowerOverlays.map((o) => o.label).join(' and '),
        weight: 0.95,
        format: (v) => format.number(v, Math.abs(v) < 1 ? 3 : 1),
        range: bounded ? [0, 100] : null,
        guides,
        series: lowerOverlays,
      });
    }
    const initial = r.metrics.final_equity / (1 + r.metrics.total_return);
    panes.push({
      id: 'equity',
      title: 'Equity',
      note: `${this.log ? 'log scale, ' : ''}started at ${format.moneyCompact(initial)}`,
      weight: 1.7,
      log: this.log,
      format: format.moneyCompact,
      tagFormat: (v) => format.money(v),
      endLabels: true,
      series: [
        { kind: 'line', values: r.series.benchmark, color: palette.benchmark, width: 1.4, endLabel: format.moneyCompact },
        { kind: 'line', values: r.series.equity, color: palette.strategy, width: 2, endLabel: format.moneyCompact },
      ],
      guides: [{ value: initial, color: palette.faint, dash: [2, 3] }],
    });
    panes.push({
      id: 'drawdown',
      title: 'Drawdown',
      note: 'from the highest equity so far',
      weight: 0.9,
      format: (v) => format.percent(v, 0),
      tagFormat: (v) => format.percent(v, 1),
      clampTop: 0,
      series: [
        { kind: 'area', values: r.series.drawdown, color: palette.down, width: 1.1, fill: palette.downSoft, baseline: 0 },
        { kind: 'line', values: r.series.benchmark_drawdown, color: palette.benchmark, width: 1.1 },
      ],
    });
    this.chart.setData({ times, panes }, { keepView });
    $('#chart-source').textContent = r.dataset.source === 'synthetic'
      ? 'Data: a synthetic market, generated in the engine from its seed.'
      : r.dataset.source && r.dataset.source !== 'opened in this page'
        ? 'Data: Quandl WIKI prices (public domain) via MarketData.jl, adjusted for splits and dividends.'
        : 'Data: the file you opened, which stays in this page.';
  }

  tooltip(index) {
    const r = this.result;
    const bars = this.bars;
    if (!r || index < 0 || index >= r.series.time.length) return '';
    const row = (label, value, color = null) => `<div class="row"><span>${color ? `<i class="key" style="color:${color}"></i>` : ''}${escapeHtml(label)}</span><b>${value}</b></div>`;
    let html = `<h4>${format.longDate(r.series.time[index])}</h4>`;
    html += row('Open, close', `${format.price(bars.open[index])}, ${format.price(bars.close[index])}`);
    html += row('High, low', `${format.price(bars.high[index])}, ${format.price(bars.low[index])}`);
    r.overlays.forEach((overlay, i) => {
      if (!this.hidden.has(overlay.key)) html += row(overlay.label, format.number(overlay.values[index], 2), overlayColor(i));
    });
    const exposure = r.series.exposure[index];
    html += row('Position', exposure === 0 ? 'flat' : `${exposure > 0 ? 'long' : 'short'}, ${format.percent(Math.abs(exposure), 0)} of equity`);
    html += row('Strategy', format.money(r.series.equity[index]), palette.strategy);
    html += row('Buy and hold', format.money(r.series.benchmark[index]), palette.benchmark);
    html += row('Drawdown', format.percent(r.series.drawdown[index], 1));
    const trade = r.trades.find((t) => t.entry_index <= index && index <= t.exit_index);
    if (trade) html += row('This trade', `${format.percent(trade.return, 1, true)} over ${trade.bars} bars`);
    return html;
  }
}
