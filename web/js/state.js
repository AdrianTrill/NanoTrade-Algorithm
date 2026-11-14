// What the user has chosen, kept in the address bar so that any backtest can be shared as a link, and turned into the
// engine's JSON requests.

export const DEFAULTS = {
  view: 'backtest',
  data: 'AAPL',
  seed: 7,
  from: '1990-01-01',
  to: '',
  strategy: 'sma_cross',
  params: {},  // overrides of the strategy's defaults
  rules: { long_entry: '', long_exit: '', short_entry: '', short_exit: '' },
  customParams: {},
  short: false,
  sizing: 'percent',
  size: 1,
  volTarget: 0.15,
  leverage: 1,
  stop: 0,
  takeProfit: 0,
  trailing: 0,
  commission: 1,
  slippage: 2,
  borrow: 0.01,
  capital: 100000,
  sweep: { x: null, y: null, metric: 'sharpe' },
  walk: { isYears: 4, oosYears: 1, anchored: false },
  monte: { paths: 1000, block: 20, seed: 1 },
  portfolio: { markets: ['AAPL', 'BA', 'CAT', 'F', 'GE'], weighting: 'equal', weights: {}, rebalance: 'monthly' },
};

const WEIGHTINGS = ['equal', 'inverse_volatility', 'fixed'];
const SCHEDULES = ['monthly', 'quarterly', 'yearly', 'never'];

export const state = structuredClone(DEFAULTS);

const NUMBER_KEYS = {
  seed: 'seed', size: 'size', vt: 'volTarget', lev: 'leverage', sl: 'stop', tp: 'takeProfit', ts: 'trailing',
  cb: 'commission', sb: 'slippage', br: 'borrow', cap: 'capital',
};

// The settings as a query string: only what differs from the defaults.
export function toQuery() {
  const q = new URLSearchParams();
  const put = (key, value, fallback) => {
    if (value !== fallback && value !== undefined && value !== null && value !== '') q.set(key, String(value));
  };
  put('view', state.view, DEFAULTS.view);
  put('d', state.data, DEFAULTS.data);
  put('from', state.from, DEFAULTS.from);
  put('to', state.to, DEFAULTS.to);
  put('s', state.strategy, DEFAULTS.strategy);
  const params = state.strategy === 'custom' ? state.customParams : state.params;
  const pairs = Object.entries(params).map(([k, v]) => `${k}:${v}`).join(',');
  if (pairs) q.set('p', pairs);
  if (state.strategy === 'custom') {
    put('le', state.rules.long_entry, '');
    put('lx', state.rules.long_exit, '');
    put('se', state.rules.short_entry, '');
    put('sx', state.rules.short_exit, '');
  }
  if (state.short) q.set('short', '1');
  put('sz', state.sizing, DEFAULTS.sizing);
  for (const [key, name] of Object.entries(NUMBER_KEYS)) put(key, state[name], DEFAULTS[name]);
  // The sweep's axes only matter, and only travel in a link, on the views that sweep.
  if (state.view === 'optimize' || state.view === 'walkforward') {
    if (state.sweep.x) q.set('sx_', `${state.sweep.x.param}:${state.sweep.x.from}:${state.sweep.x.to}:${state.sweep.x.step}`);
    if (state.sweep.y) q.set('sy_', `${state.sweep.y.param}:${state.sweep.y.from}:${state.sweep.y.to}:${state.sweep.y.step}`);
    put('m', state.sweep.metric, DEFAULTS.sweep.metric);
  }
  // As do the portfolio's markets and weights, on its view.
  if (state.view === 'portfolio') {
    put('pm', state.portfolio.markets.join(','), DEFAULTS.portfolio.markets.join(','));
    put('pw', state.portfolio.weighting, DEFAULTS.portfolio.weighting);
    put('pr', state.portfolio.rebalance, DEFAULTS.portfolio.rebalance);
    if (state.portfolio.weighting === 'fixed') {
      const weights = state.portfolio.markets.map((name) => `${name}:${state.portfolio.weights[name] ?? 1}`).join(',');
      if (weights) q.set('pf', weights);
    }
  }
  // Settings of the page rather than of the backtest, such as ?runtime=wasm, stay as they were.
  const current = new URLSearchParams(location.search);
  for (const key of ['runtime']) {
    if (current.has(key)) q.set(key, current.get(key));
  }
  return q.toString();
}

export function fromQuery(search) {
  const q = new URLSearchParams(search);
  const text = (key, fallback) => (q.has(key) ? q.get(key) : fallback);
  state.view = text('view', DEFAULTS.view);
  state.data = text('d', DEFAULTS.data);
  state.from = text('from', DEFAULTS.from);
  state.to = text('to', DEFAULTS.to);
  state.strategy = text('s', DEFAULTS.strategy);
  const params = {};
  for (const pair of (q.get('p') ?? '').split(',')) {
    const [key, value] = pair.split(':');
    if (key && value !== undefined && Number.isFinite(Number(value))) params[key] = Number(value);
  }
  if (state.strategy === 'custom') {
    state.customParams = params;
    state.rules = { long_entry: text('le', ''), long_exit: text('lx', ''), short_entry: text('se', ''), short_exit: text('sx', '') };
  } else {
    state.params = params;
  }
  state.short = q.get('short') === '1';
  state.sizing = text('sz', DEFAULTS.sizing) === 'volatility' ? 'volatility' : 'percent';
  for (const [key, name] of Object.entries(NUMBER_KEYS)) {
    const value = Number(q.get(key));
    if (q.has(key) && Number.isFinite(value)) state[name] = value;
  }
  const axis = (key) => {
    const parts = (q.get(key) ?? '').split(':');
    if (parts.length !== 4 || parts.slice(1).some((p) => !Number.isFinite(Number(p)))) return null;
    return { param: parts[0], from: Number(parts[1]), to: Number(parts[2]), step: Number(parts[3]) };
  };
  state.sweep.x = axis('sx_');
  state.sweep.y = axis('sy_');
  state.sweep.metric = text('m', DEFAULTS.sweep.metric);
  state.portfolio = structuredClone(DEFAULTS.portfolio);
  if (q.has('pm')) state.portfolio.markets = q.get('pm').split(',').filter(Boolean);
  if (WEIGHTINGS.includes(q.get('pw'))) state.portfolio.weighting = q.get('pw');
  if (SCHEDULES.includes(q.get('pr'))) state.portfolio.rebalance = q.get('pr');
  for (const pair of (q.get('pf') ?? '').split(',')) {
    const at = pair.lastIndexOf(':');
    const value = Number(pair.slice(at + 1));
    if (at > 0 && Number.isFinite(value) && value >= 0) state.portfolio.weights[pair.slice(0, at)] = value;
  }
}

export function saveQuery() {
  const query = toQuery();
  const url = `${location.pathname}${query ? `?${query}` : ''}${location.hash}`;
  if (url !== `${location.pathname}${location.search}${location.hash}`) history.replaceState(null, '', url);
}

export function dataSpec() {
  if (state.data === 'random') return { synthetic: { seed: state.seed, bars: 2520 } };
  return state.data;
}

export function strategySpec() {
  if (state.strategy === 'custom') {
    const rules = Object.fromEntries(Object.entries(state.rules).filter(([, rule]) => rule.trim() !== ''));
    return { id: 'custom', params: { ...state.customParams }, ...rules };
  }
  return { id: state.strategy, params: { ...state.params } };
}

export function configSpec() {
  return {
    initial_capital: state.capital,
    allow_short: state.short,
    sizing: state.sizing,
    position_size: state.size,
    target_volatility: state.volTarget,
    max_leverage: Math.max(state.leverage, state.sizing === 'percent' ? state.size : 0.1),
    stop_loss: state.stop,
    take_profit: state.takeProfit,
    trailing_stop: state.trailing,
    commission_bps: state.commission,
    slippage_bps: state.slippage,
    borrow_rate: state.borrow,
  };
}

export function request(command, extra = {}) {
  const range = {};
  if (state.from) range.from = state.from;
  if (state.to) range.to = state.to;
  return { command, data: dataSpec(), range, strategy: strategySpec(), config: configSpec(), ...extra };
}
