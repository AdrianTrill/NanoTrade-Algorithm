// Small exhibits: histograms with a marked value, and a scatter of in-sample against out-of-sample Sharpe ratios.
import { crisp, font, linearTicks, mono, surface } from './canvas.js';
import { palette } from './palette.js';

const finite = (v) => typeof v === 'number' && Number.isFinite(v);

// Bars counting the values in each bin, those below `split` in the down color, and `marker` drawn as a rule.
export function histogram(canvas, values, { marker = null, markerLabel = '', format = String, split = null, bins = 36 } = {}) {
  const { context: ctx, width, height } = surface(canvas);
  const data = values.filter(finite);
  if (!data.length || width < 40) return;
  const sorted = [...data].sort((a, b) => a - b);
  let low = sorted[Math.floor(sorted.length * 0.005)];
  let high = sorted[Math.ceil(sorted.length * 0.995) - 1];
  if (finite(marker)) {
    low = Math.min(low, marker);
    high = Math.max(high, marker);
  }
  if (low === high) {
    low -= 1;
    high += 1;
  }
  const pad = (high - low) * 0.04;
  low -= pad;
  high += pad;
  const counts = new Array(bins).fill(0);
  for (const v of data) {
    const bin = Math.min(bins - 1, Math.max(0, Math.floor(((v - low) / (high - low)) * bins)));
    counts[bin] += 1;
  }
  const top = Math.max(...counts);
  const left = 2;
  const right = width - 2;
  const bottom = height - 24;
  const plotTop = 20;
  const x = (v) => left + ((v - low) / (high - low)) * (right - left);
  const barWidth = (right - left) / bins;
  for (let b = 0; b < bins; b += 1) {
    const center = low + ((high - low) * (b + 0.5)) / bins;
    ctx.fillStyle = split !== null && center < split ? palette.downStrong : palette.strategy;
    ctx.globalAlpha = split !== null && center < split ? 1 : 0.55;
    const h = (counts[b] / top) * (bottom - plotTop);
    ctx.fillRect(left + b * barWidth + 0.5, bottom - h, Math.max(1, barWidth - 1), h);
  }
  ctx.globalAlpha = 1;
  ctx.strokeStyle = palette.text;
  ctx.beginPath();
  ctx.moveTo(left, crisp(bottom));
  ctx.lineTo(right, crisp(bottom));
  ctx.stroke();
  ctx.font = mono(10.5);
  ctx.fillStyle = palette.muted;
  ctx.textAlign = 'center';
  let last = -Infinity;
  for (const tick of linearTicks(low, high, Math.max(2, Math.floor(width / 90)))) {
    const tx = x(tick);
    if (tx - last < 52 || tx < left + 14 || tx > right - 14) continue;
    last = tx;
    ctx.strokeStyle = palette.text;
    ctx.beginPath();
    ctx.moveTo(crisp(tx), bottom);
    ctx.lineTo(crisp(tx), bottom + 4);
    ctx.stroke();
    ctx.fillText(format(tick), tx, bottom + 16);
  }
  if (finite(marker)) {
    const mx = crisp(x(marker));
    ctx.strokeStyle = palette.accent;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(mx, plotTop - 6);
    ctx.lineTo(mx, bottom);
    ctx.stroke();
    ctx.lineWidth = 1;
    ctx.font = font(11, 650);
    ctx.fillStyle = palette.text;
    const onRight = mx > width / 2;
    ctx.textAlign = onRight ? 'right' : 'left';
    ctx.fillText(`${markerLabel} ${format(marker)}`.trim(), mx + (onRight ? -6 : 6), plotTop + 2);
  }
}

// Points (x, y) with the diagonal y = x, where a winner would sit if it did as well out of sample, and both zero lines.
export function scatter(canvas, points, { format = (v) => v.toFixed(2), xLabel = '', yLabel = '' } = {}) {
  const { context: ctx, width, height } = surface(canvas);
  if (!points.length || width < 60) return;
  const xs = points.map((p) => p[0]);
  const ys = points.map((p) => p[1]);
  let low = Math.min(...xs, ...ys, 0);
  let high = Math.max(...xs, ...ys, 0);
  const pad = (high - low) * 0.08 || 0.1;
  low -= pad;
  high += pad;
  const left = 48;
  const right = width - 8;
  const top = 8;
  const bottom = height - 38;
  const x = (v) => left + ((v - low) / (high - low)) * (right - left);
  const y = (v) => bottom - ((v - low) / (high - low)) * (bottom - top);
  ctx.font = mono(10.5);
  for (const tick of linearTicks(low, high, 5)) {
    ctx.strokeStyle = palette.grid;
    ctx.beginPath();
    ctx.moveTo(left, crisp(y(tick)));
    ctx.lineTo(right, crisp(y(tick)));
    ctx.stroke();
    ctx.fillStyle = palette.muted;
    ctx.textAlign = 'right';
    ctx.fillText(format(tick), left - 8, y(tick) + 4);
    ctx.textAlign = 'center';
    ctx.fillText(format(tick), x(tick), bottom + 16);
  }
  ctx.strokeStyle = palette.faint;
  ctx.setLineDash([3, 3]);
  ctx.beginPath();
  ctx.moveTo(x(low), y(low));
  ctx.lineTo(x(high), y(high));
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.strokeStyle = palette.text;
  ctx.beginPath();
  ctx.moveTo(left, crisp(y(0)));
  ctx.lineTo(right, crisp(y(0)));
  ctx.moveTo(crisp(x(0)), top);
  ctx.lineTo(crisp(x(0)), bottom);
  ctx.stroke();
  for (const [px, py] of points) {
    ctx.fillStyle = py < 0 ? palette.down : palette.strategy;
    ctx.globalAlpha = 0.6;
    ctx.beginPath();
    ctx.arc(x(px), y(py), 2.6, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.globalAlpha = 1;
  ctx.fillStyle = palette.text2;
  ctx.font = font(10.5, 700);
  ctx.textAlign = 'center';
  ctx.fillText(xLabel.toUpperCase(), (left + right) / 2, height - 4);
  ctx.save();
  ctx.translate(11, (top + bottom) / 2);
  ctx.rotate(-Math.PI / 2);
  ctx.fillText(yLabel.toUpperCase(), 0, 0);
  ctx.restore();
}
