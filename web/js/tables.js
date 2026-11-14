// Tables under the chart: trades, monthly returns, every metric, and the worst drawdowns.
import { element } from './dom.js';
import { diverging } from './canvas.js';
import * as format from './format.js';
import { palette } from './palette.js';

const td = (content, className = '') => element('td', { class: className }, content);

export function tradesTable(container, trades, { onHover, onSelect }) {
  const columns = [
    ['#', (t) => t.number, (t) => String(t.number), 'left'],
    ['Side', (t) => t.direction, (t) => element('span', { class: `side ${t.direction > 0 ? 'long' : 'short'}` }, t.direction > 0 ? 'Long' : 'Short'), 'left text'],
    ['Entry', (t) => t.entry_index, (t) => t.entry, 'left'],
    ['Exit', (t) => t.exit_index, (t) => (t.open ? `${t.exit} (open)` : t.exit), 'left'],
    ['Bars', (t) => t.bars, (t) => format.integer(t.bars)],
    ['Entry price', (t) => t.entry_price, (t) => format.price(t.entry_price)],
    ['Exit price', (t) => t.exit_price, (t) => format.price(t.exit_price)],
    ['Return', (t) => t.return, (t) => element('span', { class: format.sign(t.return) }, format.percent(t.return, 2, true))],
    ['P&L', (t) => t.pnl, (t) => element('span', { class: format.sign(t.pnl) }, format.money(t.pnl))],
    ['Worst', (t) => -t.mae, (t) => format.percent(-t.mae, 1)],
    ['Best', (t) => t.mfe, (t) => format.percent(t.mfe, 1)],
    ['Exit by', (t) => t.reason, (t) => element('span', { class: 'reason' }, t.reason.replace('_', ' ')), 'left text'],
  ];
  let sort = { column: 0, descending: true };
  const rows = trades.map((t, i) => ({ ...t, number: i + 1 }));
  const render = () => {
    const [, key] = columns[sort.column];
    const sorted = [...rows].sort((a, b) => {
      const x = key(a);
      const y = key(b);
      const order = typeof x === 'string' ? x.localeCompare(y) : x - y;
      return sort.descending ? -order : order;
    });
    const head = element('tr', {}, columns.map(([name, , , align], index) => {
      const th = element('th', { class: `sortable ${(align ?? '').replace('text', '')}`.trim(), scope: 'col' }, name);
      if (index === sort.column) th.setAttribute('aria-sort', sort.descending ? 'descending' : 'ascending');
      th.addEventListener('click', () => {
        sort = { column: index, descending: index === sort.column ? !sort.descending : index !== 0 };
        render();
      });
      return th;
    }));
    const body = element('tbody');
    for (const trade of sorted.slice(0, 2000)) {
      const row = element('tr', { class: 'clickable' }, columns.map(([, , show, align]) => td(show(trade), align ?? '')));
      row.addEventListener('mouseenter', () => onHover(trade.number - 1));
      row.addEventListener('mouseleave', () => onHover(null));
      row.addEventListener('click', () => onSelect(trade.number - 1));
      body.append(row);
    }
    const table = element('table', {}, element('thead', {}, head), body);
    const note = rows.length > 2000 ? element('p', { class: 'table-note' }, `Showing 2,000 of ${format.integer(rows.length)} trades; the CSV has them all.`) : null;
    container.replaceChildren(rows.length ? table : element('p', { class: 'table-note' }, 'No trades: the rules never fired in this range.'), note ?? '');
  };
  render();
}

export function monthlyTable(container, calendar) {
  const head = element('tr', {}, element('th', { scope: 'col' }, 'Year'),
    Array.from({ length: 12 }, (_, m) => element('th', { scope: 'col' }, format.monthName(m))), element('th', { scope: 'col' }, 'Year'));
  const body = element('tbody');
  for (const year of [...calendar].reverse()) {
    const cells = year.months.map((value) => {
      const cell = element('td', { class: 'heat' }, value === null ? '' : format.percent(value, 1));
      const color = value === null ? null : diverging(value, 0.12, { base: palette.surface, strength: 0.55 });
      if (color) cell.style.background = color;
      return cell;
    });
    const total = element('td', { class: `heat ${format.sign(year.total)}` }, format.percent(year.total, 1));
    body.append(element('tr', {}, td(String(year.year), 'left'), cells, total));
  }
  container.replaceChildren(element('table', {}, element('thead', {}, head), body));
}

const METRIC_GROUPS = [
  ['Returns', [
    ['Final equity', 'final_equity', (v) => format.money(v)],
    ['Total return', 'total_return', (v) => format.percent(v, 1)],
    ['CAGR', 'cagr', (v) => format.percent(v, 2)],
    ['Years', 'years', (v) => format.number(v, 1)],
  ]],
  ['Risk', [
    ['Volatility (annual)', 'volatility', (v) => format.percent(v, 1)],
    ['Max drawdown', 'max_drawdown', (v) => format.percent(-v, 1)],
    ['Longest drawdown', 'longest_drawdown_days', (v) => format.duration(v)],
    ['Value at risk, 95%, one bar', 'var_95', (v) => format.percent(v, 2)],
    ['Expected shortfall, 95%', 'cvar_95', (v) => format.percent(v, 2)],
    ['Skewness', 'skewness', (v) => format.ratio(v)],
    ['Excess kurtosis', 'kurtosis', (v) => format.ratio(v, 1)],
  ]],
  ['Risk-adjusted', [
    ['Sharpe ratio', 'sharpe', (v) => format.ratio(v)],
    ['Sortino ratio', 'sortino', (v) => format.ratio(v)],
    ['Calmar ratio', 'calmar', (v) => format.ratio(v)],
    ['P(true Sharpe > 0)', 'psr', (v) => format.percent(v, 1)],
  ]],
  ['Trading', [
    ['Trades', 'trades', (v) => format.integer(v)],
    ['Win rate', 'win_rate', (v) => format.percent(v, 1)],
    ['Profit factor', 'profit_factor', (v) => format.ratio(v)],
    ['Average trade', 'avg_trade', (v) => format.percent(v, 2, true)],
    ['Average win', 'avg_win', (v) => format.percent(v, 2, true)],
    ['Average loss', 'avg_loss', (v) => format.percent(v, 2)],
    ['Best trade', 'best_trade', (v) => format.percent(v, 1, true)],
    ['Worst trade', 'worst_trade', (v) => format.percent(v, 1)],
    ['Average bars held', 'avg_bars_held', (v) => format.number(v, 1)],
    ['Longest losing streak', 'max_losing_streak', (v) => format.integer(v)],
    ['Time in the market', 'exposure', (v) => format.percent(v, 1)],
    ['Turnover (per year)', 'turnover', (v) => `${format.number(v, 1)}×`],
    ['Costs paid', 'costs', (v) => format.money(v)],
  ]],
  ['Against buy and hold', [
    ['Beta', 'beta', (v) => format.ratio(v)],
    ['Alpha (annual)', 'alpha', (v) => format.percent(v, 2, true)],
    ['Correlation', 'correlation', (v) => format.ratio(v)],
  ]],
];

export function metricsTable(container, metrics, benchmark) {
  const body = element('tbody');
  for (const [group, rows] of METRIC_GROUPS) {
    body.append(element('tr', {}, element('td', { class: 'group', colspan: 3 }, group)));
    for (const [label, key, show] of rows) {
      const own = group === 'Against buy and hold';
      body.append(element('tr', {}, td(label, 'left text'), td(show(metrics[key])), td(own ? '' : show(benchmark[key]), 'muted')));
    }
  }
  const head = element('tr', {}, element('th', { scope: 'col' }, 'Metric'), element('th', { scope: 'col' }, 'Strategy'), element('th', { scope: 'col' }, 'Buy and hold'));
  container.replaceChildren(element('table', {}, element('thead', {}, head), body));
}

export function drawdownsTable(container, drawdowns, onSelect) {
  const head = element('tr', {}, ['Depth', 'Peak', 'Trough', 'Recovered', 'Bars'].map((name, i) => element('th', { scope: 'col', class: i ? 'left' : '' }, name)));
  const body = element('tbody');
  for (const d of drawdowns) {
    const row = element('tr', { class: 'clickable' },
      td(element('span', { class: 'down' }, format.percent(-d.depth, 1))),
      td(d.peak, 'left'), td(d.trough, 'left'), td(d.recovery ?? 'not yet', 'left'), td(format.integer(d.bars), 'left'));
    row.addEventListener('click', () => onSelect(d));
    body.append(row);
  }
  container.replaceChildren(drawdowns.length ? element('table', {}, element('thead', {}, head), body)
    : element('p', { class: 'table-note' }, 'No drawdowns in this range.'));
}

export function downloadTrades(trades, name) {
  const header = 'direction,entry,exit,bars,entry_price,exit_price,shares,return,pnl,mae,mfe,reason,open';
  const lines = trades.map((t) => [t.direction > 0 ? 'long' : 'short', t.entry, t.exit, t.bars, t.entry_price, t.exit_price, t.shares,
    t.return, t.pnl, t.mae, t.mfe, t.reason, t.open].join(','));
  const blob = new Blob([`${header}\n${lines.join('\n')}\n`], { type: 'text/csv' });
  const link = element('a', { href: URL.createObjectURL(blob), download: `${name}-trades.csv` });
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(link.href), 1000);
}
