// The sidebar: data, dates, the strategy and its parameters or rules, position sizing, exits, and costs. Every change
// updates the state and asks for a new backtest; sliders do so as they move.
import { $, element } from './dom.js';
import { RuleEditor, ruleBlock } from './editor.js';
import * as format from './format.js';
import { state } from './state.js';

const RULE_LABELS = {
  long_entry: 'Go long when',
  long_exit: 'Exit the long when',
  short_entry: 'Go short when',
  short_exit: 'Cover the short when',
};
const RULE_HINTS = {
  long_entry: 'rsi(close, 2) < 10 and close > sma(close, 200)',
  long_exit: 'empty: when the entry rule stops holding',
  short_entry: 'empty: never short',
  short_exit: 'empty: when the short rule stops holding',
};

// A labeled slider with an editable value. With `toggle`, a switch turns it off, which sends 0.
export function slider({ label, min, max, step, value, format: show, parse = Number, onInput, toggle = false, title = '' }) {
  const range = element('input', { type: 'range', min, max, step, 'aria-label': label });
  const box = element('input', { class: 'value', type: 'text', inputmode: 'decimal', 'aria-label': `${label} value` });
  const check = toggle ? element('input', { type: 'checkbox', class: 'toggle', 'aria-label': `Use ${label.toLowerCase()}` }) : null;
  const name = check ? element('label', { class: 'switch-label', title }, check, label) : element('label', { title }, label);
  const node = element('div', { class: 'param' }, name, box, range);
  let current = value;
  let remembered = value || Number(min) + (Number(max) - Number(min)) / 4;

  const paint = () => {
    const on = !check || check.checked;
    const shown = on ? current : remembered;
    range.value = shown;
    range.disabled = !on;
    box.value = on ? show(shown) : 'Off';
    box.disabled = !on;
    node.classList.toggle('off', !on);
    const fill = ((Number(range.value) - Number(min)) / (Number(max) - Number(min))) * 100;
    range.style.setProperty('--fill', `${Math.max(0, Math.min(100, fill))}%`);
  };
  const commit = (next) => {
    current = next;
    if (next) remembered = next;
    paint();
    onInput(next);
  };
  range.addEventListener('input', () => commit(Number(range.value)));
  box.addEventListener('change', () => {
    const parsed = parse(box.value);
    if (Number.isFinite(parsed)) commit(Math.max(Number(min), Math.min(Number(max) * 100, parsed)));
    else paint();
  });
  box.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') box.blur();
  });
  check?.addEventListener('change', () => commit(check.checked ? remembered : 0));
  if (check) check.checked = Boolean(value);
  paint();
  return {
    node,
    set(next) {
      current = next;
      if (check) check.checked = Boolean(next);
      if (next) remembered = next;
      paint();
    },
    invalid(flag) {
      node.classList.toggle('invalid', flag);
    },
  };
}

const percentOf = (digits = 0) => (v) => `${(v * 100).toFixed(digits)}%`;
const parsePercent = (text) => Number(String(text).replace('%', '').trim()) / 100;
const parseNumber = (text) => Number(String(text).replace(/[,$%\s]/g, ''));

export class Controls {
  constructor({ catalog, manifest, onChange, onUpload, onReference }) {
    this.catalog = catalog;
    this.manifest = manifest;
    this.onChange = onChange;
    this.uploads = [];
    this.paramSliders = new Map();
    this.editors = new Map();
    this.bounds = null;

    this.datasetSelect = $('#dataset');
    this.strategySelect = $('#strategy');
    this.fillDatasets();
    this.fillStrategies();

    this.datasetSelect.addEventListener('change', () => {
      state.data = this.datasetSelect.value;
      this.describeDataset();
      this.onChange('data');
    });
    $('#seed').addEventListener('change', () => {
      state.seed = Math.max(0, Math.floor(Number($('#seed').value) || 0));
      this.onChange('data');
    });
    $('#reseed').addEventListener('click', () => this.reseed());
    for (const id of ['from', 'to']) {
      $(`#${id}`).addEventListener('change', () => {
        state[id] = $(`#${id}`).value;
        this.markPreset();
        this.onChange('range');
      });
    }
    this.strategySelect.addEventListener('change', () => {
      this.selectStrategy(this.strategySelect.value);
    });
    $('#edit-rules').addEventListener('click', () => this.editRules());
    $('#reference-open').addEventListener('click', onReference);
    $('#upload').addEventListener('click', () => $('#file').click());
    $('#file').addEventListener('change', () => {
      const [file] = $('#file').files;
      if (file) onUpload(file);
      $('#file').value = '';
    });
    for (const id of ['direction', 'sizing']) {
      for (const button of $(`#${id}`).querySelectorAll('button')) {
        button.addEventListener('click', () => {
          if (id === 'direction') state.short = button.dataset.value === 'both';
          else state.sizing = button.dataset.value;
          this.renderPosition();
          this.renderStrategy();
          this.onChange(id);
        });
      }
    }
    this.renderAll();
  }

  renderAll() {
    this.datasetSelect.value = state.data;
    if (this.datasetSelect.value !== state.data) {
      state.data = this.manifest.datasets[0].symbol;
      this.datasetSelect.value = state.data;
    }
    $('#seed').value = state.seed;
    this.describeDataset();
    this.strategySelect.value = state.strategy;
    if (this.strategySelect.value !== state.strategy) {
      state.strategy = 'sma_cross';
      this.strategySelect.value = state.strategy;
    }
    this.renderStrategy();
    this.renderPosition();
    this.renderRisk();
  }

  fillDatasets() {
    const select = this.datasetSelect;
    select.replaceChildren();
    const real = element('optgroup', { label: 'Daily bars, adjusted (public domain)' });
    for (const d of this.manifest.datasets) {
      real.append(element('option', { value: d.symbol }, `${d.symbol} · ${d.name}`));
    }
    select.append(real);
    if (this.uploads.length) {
      const yours = element('optgroup', { label: 'Opened in this page' });
      for (const u of this.uploads) yours.append(element('option', { value: u.name }, `${u.title} · ${u.bars.toLocaleString('en-US')} bars`));
      select.append(yours);
    }
    select.append(element('optgroup', { label: 'Control' }, element('option', { value: 'random' }, 'Random market (no edge to find)')));
  }

  addUpload(dataset) {
    this.uploads = this.uploads.filter((u) => u.name !== dataset.name);
    this.uploads.push({ name: dataset.name, title: dataset.title, bars: dataset.bars, first: dataset.first, last: dataset.last, notes: '' });
    this.fillDatasets();
    state.data = dataset.name;
    state.from = '';
    state.to = '';
    this.renderAll();
  }

  datasetInfo() {
    const fromManifest = this.manifest.datasets.find((d) => d.symbol === state.data);
    if (fromManifest) return { ...fromManifest, title: fromManifest.name };
    return this.uploads.find((u) => u.name === state.data) ?? null;
  }

  describeDataset() {
    const info = this.datasetInfo();
    const random = state.data === 'random';
    $('#seed-field').hidden = !random;
    let note = '';
    if (random) {
      note = 'Ten years of a random walk with clustered volatility and jumps: there is nothing to predict, so any edge a backtest finds here is luck.';
      this.bounds = { first: '2000-01-03', last: '2009-08-28' };
    } else if (info) {
      note = `${format.integer(info.bars)} daily bars, ${format.longDate(Date.parse(info.first) / 1000)} to ${format.longDate(Date.parse(info.last) / 1000)}.${info.notes ? ` ${info.notes}` : ''}`;
      this.bounds = { first: info.first, last: info.last };
    }
    $('#dataset-note').textContent = note;
    for (const id of ['from', 'to']) {
      $(`#${id}`).min = this.bounds?.first ?? '';
      $(`#${id}`).max = this.bounds?.last ?? '';
    }
    $('#from').value = state.from;
    $('#to').value = state.to;
    this.renderPresets();
  }

  renderPresets() {
    const last = this.bounds?.last ?? '2013-12-31';
    const lastYear = Number(last.slice(0, 4));
    const presets = [
      ['All', '', ''],
      ['Since 1990', '1990-01-01', ''],
      ['Since 2000', '2000-01-01', ''],
      ['2007–2009', '2007-01-01', '2009-12-31'],
      ['Last 5 years', `${lastYear - 5}${last.slice(4)}`, ''],
    ].filter(([, from]) => !from || !this.bounds || from >= this.bounds.first || from === '');
    const container = $('#presets');
    container.replaceChildren(...presets.map(([label, from, to]) => element('button', {
      class: 'chip',
      dataset: { from, to },
      onclick: () => {
        state.from = from;
        state.to = to;
        $('#from').value = from;
        $('#to').value = to;
        this.markPreset();
        this.onChange('range');
      },
    }, label)));
    this.markPreset();
  }

  markPreset() {
    for (const chip of $('#presets').children) {
      chip.setAttribute('aria-pressed', String(chip.dataset.from === state.from && chip.dataset.to === state.to));
    }
  }

  reseed() {
    state.seed = Math.floor(Math.random() * 100000);
    $('#seed').value = state.seed;
    this.onChange('data');
  }

  fillStrategies() {
    const families = new Map();
    for (const s of this.catalog.strategies) {
      if (!families.has(s.family)) families.set(s.family, element('optgroup', { label: s.family }));
      families.get(s.family).append(element('option', { value: s.id }, s.name));
    }
    this.strategySelect.replaceChildren(...families.values());
  }

  strategyInfo(id = state.strategy) {
    return this.catalog.strategies.find((s) => s.id === id);
  }

  selectStrategy(id) {
    state.strategy = id;
    state.params = {};
    if (id === 'custom' && !state.rules.long_entry && !state.rules.short_entry) {
      const example = this.strategyInfo('custom');
      state.rules = { ...example.rules };
      state.customParams = Object.fromEntries(example.params.map((p) => [p.name, p.default]));
    }
    this.renderStrategy();
    this.onChange('strategy');
  }

  // The current built-in's rules, as a custom strategy to edit.
  editRules() {
    if (state.strategy === 'custom') {
      this.editors.get('long_entry')?.textarea.focus();
      return;
    }
    const info = this.strategyInfo();
    state.rules = { ...info.rules };
    state.customParams = Object.fromEntries(info.params.map((p) => [p.name, state.params[p.name] ?? p.default]));
    state.strategy = 'custom';
    this.strategySelect.value = 'custom';
    this.renderStrategy();
    this.onChange('strategy');
    this.editors.get('long_entry')?.textarea.focus();
  }

  insertRule(text) {
    if (state.strategy !== 'custom') this.editRules();
    state.rules.long_entry = text;
    state.rules.long_exit = '';
    this.syncCustomParams();
    this.renderStrategy();
    this.onChange('strategy');
  }

  renderStrategy() {
    const info = this.strategyInfo();
    $('#strategy-summary').textContent = info?.summary ?? '';
    const params = $('#params');
    const rules = $('#rules');
    this.paramSliders.clear();
    this.editors.clear();
    params.replaceChildren();
    rules.replaceChildren();
    $('#edit-rules').hidden = state.strategy === 'custom' || state.strategy === 'buy_hold';
    if (!info) return;

    if (state.strategy !== 'custom') {
      for (const p of info.params) {
        const value = state.params[p.name] ?? p.default;
        const control = slider({
          label: p.label,
          min: p.min,
          max: p.max,
          step: p.step,
          value,
          format: (v) => (p.integer ? String(Math.round(v)) : format.number(v, 1)),
          onInput: (v) => {
            state.params[p.name] = p.integer ? Math.round(v) : Math.round(v * 100) / 100;
            this.onChange('params');
          },
        });
        this.paramSliders.set(p.name, control);
        params.append(control.node);
      }
      for (const [key, label] of Object.entries(RULE_LABELS)) {
        const source = info.rules[key];
        if (!source) continue;
        if ((key === 'short_entry' || key === 'short_exit') && !state.short) continue;
        rules.append(ruleBlock(label, source));
      }
      return;
    }

    for (const [key, label] of Object.entries(RULE_LABELS)) {
      const optional = key !== 'long_entry';
      const editor = new RuleEditor({
        label: optional ? `${label}` : label,
        placeholder: RULE_HINTS[key],
        value: state.rules[key] ?? '',
        onInput: (text) => {
          state.rules[key] = text;
          this.scheduleCustom();
        },
      });
      this.editors.set(key, editor);
      if ((key === 'short_entry' || key === 'short_exit') && !state.short && !state.rules[key]) continue;
      rules.append(editor.node);
    }
    this.renderCustomParams();
  }

  scheduleCustom() {
    clearTimeout(this.customTimer);
    this.customTimer = setTimeout(() => {
      if (this.syncCustomParams()) this.renderCustomParams();
      this.onChange('rules');
    }, 220);
  }

  // Parameters are whatever $names the rules mention; new ones start at 20.
  syncCustomParams() {
    const names = new Set();
    for (const rule of Object.values(state.rules)) {
      for (const match of rule.matchAll(/\$([A-Za-z_]\w*)/g)) names.add(match[1].toLowerCase());
    }
    const next = {};
    for (const name of names) next[name] = state.customParams[name] ?? 20;
    const changed = JSON.stringify(Object.keys(next)) !== JSON.stringify(Object.keys(state.customParams));
    state.customParams = next;
    return changed;
  }

  renderCustomParams() {
    const params = $('#params');
    params.replaceChildren();
    this.syncCustomParams();
    const names = Object.keys(state.customParams);
    if (!names.length) return;
    const grid = element('div', { class: 'custom-params' });
    for (const name of names) {
      const input = element('input', { type: 'number', step: 'any', value: state.customParams[name], 'aria-label': `$${name}` });
      input.addEventListener('input', () => {
        const value = Number(input.value);
        if (Number.isFinite(value)) {
          state.customParams[name] = value;
          this.onChange('params');
        }
      });
      grid.append(element('label', { class: 'custom-param' }, element('span', {}, `$${name}`), input));
    }
    params.append(grid);
  }

  renderPosition() {
    for (const button of $('#direction').querySelectorAll('button')) {
      button.setAttribute('aria-pressed', String((button.dataset.value === 'both') === state.short));
    }
    for (const button of $('#sizing').querySelectorAll('button')) {
      button.setAttribute('aria-pressed', String(button.dataset.value === state.sizing));
    }
    const box = $('#position-params');
    box.replaceChildren();
    if (state.sizing === 'percent') {
      box.append(slider({
        label: 'Share of equity', min: 0.1, max: 1, step: 0.05, value: state.size, format: percentOf(), parse: parsePercent,
        title: 'The part of the account each position takes',
        onInput: (v) => {
          state.size = Math.round(v * 100) / 100;
          this.onChange('config');
        },
      }).node);
    } else {
      box.append(slider({
        label: 'Target volatility', min: 0.05, max: 0.4, step: 0.01, value: state.volTarget, format: percentOf(), parse: parsePercent,
        title: 'Positions are scaled so their annualized volatility, over the last 20 bars, is about this',
        onInput: (v) => {
          state.volTarget = Math.round(v * 100) / 100;
          this.onChange('config');
        },
      }).node, slider({
        label: 'Leverage cap', min: 0.5, max: 3, step: 0.1, value: state.leverage, format: (v) => `${v.toFixed(1)}×`, parse: parseNumber,
        title: 'The largest position, as a multiple of equity',
        onInput: (v) => {
          state.leverage = Math.round(v * 10) / 10;
          this.onChange('config');
        },
      }).node);
    }
    box.append(slider({
      label: 'Initial capital', min: 1000, max: 1000000, step: 1000, value: state.capital, format: (v) => format.money(v), parse: parseNumber,
      onInput: (v) => {
        state.capital = Math.round(v);
        this.onChange('config');
      },
    }).node);
  }

  renderRisk() {
    const box = $('#risk-params');
    const make = (key, label, min, max, step, title) => slider({
      label, min, max, step, value: state[key], format: percentOf(), parse: parsePercent, toggle: true, title,
      onInput: (v) => {
        state[key] = Math.round(v * 1000) / 1000;
        this.onChange('config');
      },
    }).node;
    const cost = (key, label, max, step, show, parse, title) => slider({
      label, min: 0, max, step, value: state[key], format: show, parse, title,
      onInput: (v) => {
        state[key] = Math.round(v * 1000) / 1000;
        this.onChange('config');
      },
    }).node;
    box.replaceChildren(
      make('stop', 'Stop loss', 0.01, 0.3, 0.01, 'Exit when the price falls this far below the entry (rises above it, for a short)'),
      make('takeProfit', 'Take profit', 0.02, 1, 0.01, 'Exit when the price gains this much from the entry'),
      make('trailing', 'Trailing stop', 0.02, 0.4, 0.01, 'Exit this far below the highest high since the entry'),
      cost('commission', 'Commission', 50, 0.5, (v) => `${v} bps`, parseNumber, 'Per fill, in basis points of its value'),
      cost('slippage', 'Slippage', 50, 0.5, (v) => `${v} bps`, parseNumber, 'Each fill at a price this much worse than the open or the stop'),
      cost('borrow', 'Borrow (shorts)', 0.1, 0.0025, percentOf(2), parsePercent, 'The yearly cost of a short position, as a share of its value'),
    );
  }

  // An error from the engine, shown where it belongs: under a rule, on a slider, or in the rule-error box.
  showError(error) {
    for (const editor of this.editors.values()) editor.setError(null);
    for (const control of this.paramSliders.values()) control.invalid(false);
    let box = $('#rule-error');
    if (!box) {
      box = element('div', { class: 'rule-error', id: 'rule-error' });
      $('#rules').after(box);
    }
    if (!error) {
      box.hidden = true;
      return false;
    }
    const field = error.field ?? '';
    const ruleKey = field.startsWith('strategy.') ? field.slice('strategy.'.length) : '';
    if (this.editors.has(ruleKey) && error.position !== undefined) {
      this.editors.get(ruleKey).setError({ position: error.position, length: error.length ?? 1 });
    }
    if (field.startsWith('strategy.params.')) this.paramSliders.get(field.slice('strategy.params.'.length))?.invalid(true);
    const shown = field.startsWith('strategy') || field.startsWith('config') || field === 'range' || field.startsWith('range');
    box.hidden = !shown;
    box.textContent = error.message;
    return shown;
  }
}
