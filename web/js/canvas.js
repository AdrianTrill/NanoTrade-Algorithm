// Canvas helpers shared by the charts.
import { palette } from './palette.js';

// A canvas sized for the display's pixel density and cleared, with its size in CSS pixels.
export function surface(canvas) {
  const ratio = window.devicePixelRatio || 1;
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
  }
  const context = canvas.getContext('2d');
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  context.font = font(11);
  context.lineWidth = 1;
  context.textBaseline = 'alphabetic';
  return { context, width, height };
}

// The page's three voices: the interface's sans, a monospace for numbers, and a serif for titles.
export const font = (size, weight = 500) => `${weight} ${size}px ${palette.fontUi || 'system-ui, sans-serif'}`;
export const mono = (size, weight = 400) => `${weight} ${size}px ${palette.fontMono || 'ui-monospace, monospace'}`;
export const serif = (size, weight = 700) => `${weight} ${size}px ${palette.fontSerif || 'Georgia, serif'}`;

// A pixel-aligned coordinate for crisp one-pixel lines.
export const crisp = (value) => Math.round(value) + 0.5;

// Round steps for an axis covering `range` with about `count` labels: 1, 2, 2.5, or 5 times a power of ten.
export function niceStep(range, count) {
  const rough = Math.max(range, 1e-12) / Math.max(count, 1);
  const power = 10 ** Math.floor(Math.log10(rough));
  for (const factor of [1, 2, 2.5, 5, 10]) {
    if (factor * power >= rough) return factor * power;
  }
  return 10 * power;
}

export function linearTicks(low, high, count) {
  const step = niceStep(high - low, count);
  const ticks = [];
  for (let v = Math.ceil(low / step) * step; v <= high + step * 1e-9; v += step) {
    ticks.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  }
  return ticks;
}

// Ticks for a logarithmic axis: 1, 2, 5 times powers of ten within [low, high], thinned to about `count`.
export function logTicks(low, high, count) {
  const ticks = [];
  for (let power = Math.floor(Math.log10(low)); power <= Math.ceil(Math.log10(high)); power += 1) {
    for (const factor of [1, 2, 5]) {
      const value = factor * 10 ** power;
      if (value >= low && value <= high) ticks.push(value);
    }
  }
  if (ticks.length <= count) return ticks;
  const stride = Math.ceil(ticks.length / count);
  return ticks.filter((_, i) => i % stride === 0);
}

// A label set in reverse, ink on paper turned paper on ink, such as the crosshair's readouts on an axis.
export function tag(context, text, x, y, align = 'left') {
  context.font = mono(11, 500);
  const width = Math.ceil(context.measureText(text).width) + 10;
  const left = align === 'right' ? x - width : align === 'center' ? x - width / 2 : x;
  context.fillStyle = palette.text;
  context.fillRect(Math.round(left), Math.round(y - 9), width, 18);
  context.fillStyle = palette.surface;
  context.textAlign = 'left';
  context.fillText(text, Math.round(left) + 5, Math.round(y + 4));
  return width;
}

function parseHex(hex) {
  const value = hex.replace('#', '');
  const full = value.length === 3 ? [...value].map((c) => c + c).join('') : value;
  return [0, 2, 4].map((i) => parseInt(full.slice(i, i + 2), 16));
}

export function mix(a, b, t) {
  const [x, y] = [parseHex(a), parseHex(b)];
  const channel = (i) => Math.round(x[i] + (y[i] - x[i]) * t);
  return `rgb(${channel(0)}, ${channel(1)}, ${channel(2)})`;
}

// A diverging scale centered on zero: the down color below it, the up color above, `base` at zero. `strength` caps
// how far a cell goes toward the full color, so text drawn over it stays legible.
export function diverging(value, scale, { base = palette.surfaceSunken, strength = 1 } = {}) {
  if (!Number.isFinite(value)) return null;
  const t = Math.max(-1, Math.min(1, value / (scale || 1)));
  const from = base?.startsWith('#') ? base : '#ebe4d6';
  const amount = (0.08 + 0.92 * Math.abs(t)) * strength;
  return mix(from, t >= 0 ? palette.up : palette.down, amount);
}
