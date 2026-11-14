// Number and date formatting for the page.

const nothing = '–';
const finite = (value) => typeof value === 'number' && Number.isFinite(value);

// Intl.NumberFormat is slow to build and quick to use, and toLocaleString builds one on every call: one per pattern,
// kept, formats the same text in a fraction of the time.
const formatters = new Map();
function grouped(value, minimum, maximum = minimum) {
  const key = `${minimum}:${maximum}`;
  let formatter = formatters.get(key);
  if (!formatter) {
    formatter = new Intl.NumberFormat('en-US', { minimumFractionDigits: minimum, maximumFractionDigits: maximum });
    formatters.set(key, formatter);
  }
  return formatter.format(value);
}

// Zero, for a value that would print as zero with a minus sign: -0, or -0.00004 at two decimals.
const settle = (value, decimals) => (Math.abs(value) < 0.5 * 10 ** -decimals ? 0 : value);

export function percent(value, digits = 1, signed = false) {
  if (!finite(value)) return nothing;
  value = settle(value * 100, digits) / 100;
  const text = `${grouped(value * 100, digits)}%`;
  return signed && value > 0 ? `+${text}` : text.replace('-', '−');
}

// A difference of two fractions in percentage points: +2.1 pts, −18.2 pts.
export function points(value, digits = 1) {
  if (!finite(value)) return nothing;
  const settled = settle(value * 100, digits);
  return `${settled > 0 ? '+' : settled < 0 ? '−' : '±'}${grouped(Math.abs(settled), digits)} pts`;
}

// A signed difference: +0.12, −3.
export function change(value, digits = 2) {
  if (!finite(value)) return nothing;
  const settled = settle(value, digits);
  return `${settled > 0 ? '+' : settled < 0 ? '−' : '±'}${grouped(Math.abs(settled), digits)}`;
}

export function ratio(value, digits = 2) {
  if (!finite(value)) return nothing;
  return settle(value, digits).toFixed(digits).replace('-', '−');
}

export function integer(value) {
  if (!finite(value)) return nothing;
  return grouped(Math.round(value), 0);
}

export function money(value, digits = 0) {
  if (!finite(value)) return nothing;
  const text = grouped(Math.abs(value), digits);
  return `${value < 0 ? '−' : ''}$${text}`;
}

// A fraction as a percentage with as many decimals as its size deserves: 2,547%, 14.6%, 0.82%.
export function bigPercent(value) {
  if (!finite(value)) return nothing;
  const magnitude = Math.abs(value);
  return percent(value, magnitude >= 1 ? 0 : magnitude >= 0.01 ? 1 : 2);
}

// A probability in whole percent, which never rounds to certainty: >99% and <1% at the ends.
export function probability(value) {
  if (!finite(value)) return nothing;
  if (value >= 0.995) return '>99%';
  if (value <= 0.005) return '<1%';
  return percent(value, 0);
}

// Money in words, for sentences: $86,450, $1.05 million, $2.3 billion.
export function moneyWords(value) {
  if (!finite(value)) return nothing;
  const magnitude = Math.abs(value);
  const sign = value < 0 ? '−' : '';
  if (magnitude >= 1e9) return `${sign}$${trim(magnitude / 1e9)} billion`;
  if (magnitude >= 1e6) return `${sign}$${trim(magnitude / 1e6)} million`;
  return money(value);
}

// $1.05M: for axes.
export function moneyCompact(value) {
  if (!finite(value)) return nothing;
  return `${value < 0 ? '−' : ''}$${compact(Math.abs(value))}`;
}

// 1,234,567 as 1.23M: for axes and compact labels.
export function compact(value) {
  if (!finite(value)) return nothing;
  const magnitude = Math.abs(value);
  const sign = value < 0 ? '−' : '';
  if (magnitude >= 1e9) return `${sign}${trim(magnitude / 1e9)}B`;
  if (magnitude >= 1e6) return `${sign}${trim(magnitude / 1e6)}M`;
  if (magnitude >= 1e3) return `${sign}${trim(magnitude / 1e3)}K`;
  return `${sign}${trim(magnitude)}`;
}

function trim(value) {
  const digits = value >= 100 ? 0 : value >= 10 ? 1 : 2;
  return Number(value.toFixed(digits)).toString();
}

export function price(value) {
  if (!finite(value)) return nothing;
  const digits = Math.abs(value) >= 1000 ? 0 : Math.abs(value) >= 10 ? 2 : Math.abs(value) >= 1 ? 3 : 4;
  return grouped(value, digits);
}

export function number(value, digits = 2) {
  if (!finite(value)) return nothing;
  return grouped(Number(settle(value, digits).toFixed(digits)), 0, digits).replace('-', '−');
}

// Timestamps arrive as seconds since 1970 (UTC).
export function date(seconds) {
  if (!finite(seconds)) return nothing;
  return new Date(seconds * 1000).toISOString().slice(0, 10);
}

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

export function longDate(seconds) {
  if (!finite(seconds)) return nothing;
  const d = new Date(seconds * 1000);
  return `${MONTHS[d.getUTCMonth()]} ${d.getUTCDate()}, ${d.getUTCFullYear()}`;
}

export const monthName = (index) => MONTHS[index];

export function duration(days) {
  if (!finite(days)) return nothing;
  if (days >= 730) return `${(days / 365.25).toFixed(1)} years`;
  if (days >= 60) return `${Math.round(days / 30.44)} months`;
  return `${Math.round(days)} days`;
}

export const sign = (value) => (finite(value) ? (value > 0 ? 'up' : value < 0 ? 'down' : '') : '');
