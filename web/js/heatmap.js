// The parameter map: one cell per combination, shaded by its score, with the best outlined and the backtest's own
// parameters marked. A one-parameter sweep is drawn as bars instead.
import { crisp, diverging, font, mono, surface } from './canvas.js';
import { escapeHtml } from './dom.js';
import * as format from './format.js';
import { palette } from './palette.js';

const CENTERED = new Set(['sharpe', 'sortino', 'cagr', 'total_return', 'calmar']);
const finite = (v) => typeof v === 'number' && Number.isFinite(v);

export const METRIC_LABELS = {
  sharpe: 'Sharpe ratio',
  sortino: 'Sortino ratio',
  cagr: 'Annual return',
  total_return: 'Total return',
  calmar: 'Calmar ratio',
  max_drawdown: 'Worst drawdown',
  profit_factor: 'Profit factor',
  win_rate: 'Win rate',
};

export function formatScore(metric, value) {
  if (!finite(value)) return '–';
  if (metric === 'cagr' || metric === 'total_return' || metric === 'win_rate' || metric === 'max_drawdown') {
    return format.percent(value, 1);
  }
  return format.ratio(value);
}

export class Heatmap {
  constructor(canvas, tooltip, { onSelect }) {
    this.canvas = canvas;
    this.tooltip = tooltip;
    this.onSelect = onSelect;
    this.result = null;
    this.current = null;
    this.hover = null;
    canvas.addEventListener('pointermove', (event) => this.move(event));
    canvas.addEventListener('pointerleave', () => {
      this.hover = null;
      tooltip.hidden = true;
      this.draw();
    });
    canvas.addEventListener('click', () => {
      if (this.hover !== null && this.result.cells.score[this.hover] !== null) {
        const { x, y } = this.coordinates(this.hover);
        this.onSelect(x, y);
      }
    });
    new ResizeObserver(() => this.draw()).observe(canvas);
  }

  set(result, current) {
    this.result = result;
    this.current = current;
    this.draw();
  }

  coordinates(index) {
    const xs = this.result.x.values;
    return { x: xs[index % xs.length], y: this.result.y ? this.result.y.values[Math.floor(index / xs.length)] : null };
  }

  geometry() {
    const width = this.canvas.clientWidth;
    const height = this.canvas.clientHeight;
    const left = this.result.y ? 62 : 58;
    const right = width - 78;
    const top = 8;
    const bottom = height - 46;
    const nx = this.result.x.values.length;
    const ny = this.result.y ? this.result.y.values.length : 1;
    return { width, height, left, right, top, bottom, nx, ny, cw: (right - left) / nx, ch: (bottom - top) / ny };
  }

  scale() {
    const scores = this.result.cells.score.filter(finite);
    if (!scores.length) return { center: 0, spread: 1 };
    if (CENTERED.has(this.result.metric)) return { center: 0, spread: Math.max(...scores.map(Math.abs)) || 1 };
    const sorted = [...scores].sort((a, b) => a - b);
    const center = sorted[Math.floor(sorted.length / 2)];
    return { center, spread: Math.max(sorted.at(-1) - center, center - sorted[0]) || 1 };
  }

  // A score as it reads: drawdowns are ranked by their negative, but shown as the drawdown.
  shown(value) {
    return this.result.metric === 'max_drawdown' && finite(value) ? -value : value;
  }

  draw() {
    const { context: ctx } = surface(this.canvas);
    if (!this.result) return;
    const g = this.geometry();
    const { center, spread } = this.scale();
    const score = this.result.cells.score;
    const metric = this.result.metric;

    if (!this.result.y) {
      const values = score.filter(finite);
      const low = Math.min(0, ...values);
      const high = Math.max(0, ...values);
      const y = (v) => g.bottom - ((v - low) / (high - low || 1)) * (g.bottom - g.top);
      for (let i = 0; i < g.nx; i += 1) {
        const v = score[i];
        const x0 = g.left + i * g.cw;
        const gap = g.cw > 6 ? 1 : 0;
        if (!finite(v)) continue;
        ctx.fillStyle = diverging(v - center, spread);
        const top = Math.min(y(0), y(v));
        const h = Math.max(1, Math.abs(y(v) - y(0)));
        ctx.fillRect(x0 + gap, top, Math.max(1, g.cw - 2 * gap), h);
        if (i === this.result.best?.index || i === this.hover) {
          ctx.strokeStyle = i === this.hover ? palette.strategy : palette.text;
          ctx.lineWidth = 2;
          ctx.strokeRect(x0 + gap + 1, top + 1, Math.max(1, g.cw - 2 * gap - 2), Math.max(1, h - 2));
          ctx.lineWidth = 1;
        }
      }
      ctx.strokeStyle = palette.text;
      ctx.beginPath();
      ctx.moveTo(g.left, crisp(y(0)));
      ctx.lineTo(g.right, crisp(y(0)));
      ctx.stroke();
      ctx.font = mono(10.5);
      ctx.fillStyle = palette.muted;
      ctx.textAlign = 'right';
      ctx.fillText(formatScore(metric, this.shown(high)), g.left - 8, y(high) + 4);
      ctx.fillText(formatScore(metric, this.shown(low)), g.left - 8, y(low));
    } else {
      const gap = g.cw > 9 && g.ch > 9 ? 1 : 0;
      for (let j = 0; j < g.ny; j += 1) {
        for (let i = 0; i < g.nx; i += 1) {
          const index = j * g.nx + i;
          const v = score[index];
          const x0 = g.left + i * g.cw;
          const y0 = g.bottom - (j + 1) * g.ch;
          if (!finite(v)) {
            // Combinations that can't run, such as a fast window longer than the slow one: hatched.
            ctx.strokeStyle = palette.border;
            ctx.beginPath();
            ctx.moveTo(x0 + gap, y0 + g.ch - gap);
            ctx.lineTo(x0 + g.cw - gap, y0 + gap);
            ctx.stroke();
            continue;
          }
          ctx.fillStyle = diverging(v - center, spread);
          ctx.fillRect(x0 + gap / 2, y0 + gap / 2, g.cw - gap + (gap ? 0 : 0.5), g.ch - gap + (gap ? 0 : 0.5));
        }
      }
      const outline = (index, color, widthPx) => {
        const i = index % g.nx;
        const j = Math.floor(index / g.nx);
        ctx.strokeStyle = color;
        ctx.lineWidth = widthPx;
        ctx.strokeRect(g.left + i * g.cw + widthPx / 2, g.bottom - (j + 1) * g.ch + widthPx / 2, g.cw - widthPx, g.ch - widthPx);
        ctx.lineWidth = 1;
      };
      if (this.result.best) outline(this.result.best.index, palette.text, 2);
      if (this.hover !== null) outline(this.hover, palette.strategy, 2);
      const current = this.currentIndex();
      if (current !== null) {
        const i = current % g.nx;
        const j = Math.floor(current / g.nx);
        ctx.fillStyle = palette.text;
        ctx.strokeStyle = palette.surface;
        ctx.beginPath();
        ctx.arc(g.left + (i + 0.5) * g.cw, g.bottom - (j + 0.5) * g.ch, Math.max(2.5, Math.min(4.5, g.cw / 4, g.ch / 4)), 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();
      }
      ctx.font = mono(10.5);
      ctx.fillStyle = palette.muted;
      ctx.textAlign = 'right';
      const yStride = Math.max(1, Math.ceil(17 / g.ch));
      this.result.y.values.forEach((v, j) => {
        if (j % yStride === 0) ctx.fillText(format.number(v, 2), g.left - 8, g.bottom - (j + 0.5) * g.ch + 4);
      });
      ctx.save();
      ctx.translate(13, (g.top + g.bottom) / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.textAlign = 'center';
      ctx.font = font(12, 500);
      ctx.fillStyle = palette.text2;
      ctx.fillText(this.result.y.param, 0, 0);
      ctx.restore();
    }
    ctx.strokeStyle = palette.text;
    ctx.beginPath();
    ctx.moveTo(g.left, crisp(g.bottom + 2));
    ctx.lineTo(g.right, crisp(g.bottom + 2));
    ctx.stroke();
    ctx.font = mono(10.5);
    ctx.fillStyle = palette.muted;
    ctx.textAlign = 'center';
    const xStride = Math.max(1, Math.ceil(38 / g.cw));
    this.result.x.values.forEach((v, i) => {
      if (i % xStride === 0) ctx.fillText(format.number(v, 2), g.left + (i + 0.5) * g.cw, g.bottom + 17);
    });
    ctx.font = font(12, 500);
    ctx.fillStyle = palette.text2;
    ctx.fillText(this.result.x.param, (g.left + g.right) / 2, g.bottom + 37);
    this.drawScale(ctx, g, center, spread);
  }

  drawScale(ctx, g, center, spread) {
    const x0 = g.right + 20;
    const steps = 48;
    const h = (g.bottom - g.top) / steps;
    for (let s = 0; s < steps; s += 1) {
      const t = 1 - (s + 0.5) / steps;
      ctx.fillStyle = diverging((t * 2 - 1) * spread, spread);
      ctx.fillRect(x0, g.top + s * h, 9, h + 0.5);
    }
    const metric = this.result.metric;
    const show = (v) => formatScore(metric, this.shown(v));
    ctx.font = mono(10.5);
    ctx.fillStyle = palette.muted;
    ctx.textAlign = 'left';
    ctx.fillText(show(center + spread), x0 + 14, g.top + 9);
    ctx.fillText(show(center), x0 + 14, (g.top + g.bottom) / 2 + 4);
    ctx.fillText(show(center - spread), x0 + 14, g.bottom);
  }

  currentIndex() {
    if (!this.current || !this.result) return null;
    const i = this.result.x.values.findIndex((v) => Math.abs(v - this.current.x) < 1e-9);
    const j = this.result.y ? this.result.y.values.findIndex((v) => Math.abs(v - this.current.y) < 1e-9) : 0;
    return i >= 0 && j >= 0 ? j * this.result.x.values.length + i : null;
  }

  move(event) {
    if (!this.result) return;
    const rect = this.canvas.getBoundingClientRect();
    const px = event.clientX - rect.left;
    const py = event.clientY - rect.top;
    const g = this.geometry();
    const i = Math.floor((px - g.left) / g.cw);
    const j = this.result.y ? Math.floor((g.bottom - py) / g.ch) : 0;
    if (i < 0 || i >= g.nx || j < 0 || j >= g.ny || py < g.top || py > g.bottom) {
      this.hover = null;
      this.tooltip.hidden = true;
      this.canvas.style.cursor = '';
      this.draw();
      return;
    }
    const index = j * g.nx + i;
    this.hover = index;
    this.canvas.style.cursor = this.result.cells.score[index] === null ? 'not-allowed' : 'pointer';
    this.draw();
    const { x, y } = this.coordinates(index);
    const c = this.result.cells;
    const row = (label, value) => `<div class="row"><span>${escapeHtml(label)}</span><b>${value}</b></div>`;
    let html = `<h4>${escapeHtml(this.result.x.param)} ${format.number(x, 2)}${this.result.y ? `, ${escapeHtml(this.result.y.param)} ${format.number(y, 2)}` : ''}</h4>`;
    if (c.score[index] === null) {
      html += '<div class="row"><span>These values can\'t run together</span></div>';
    } else {
      html += row('Sharpe ratio', format.ratio(c.sharpe[index]));
      html += row('Annual return', format.percent(c.cagr[index], 1));
      html += row('Total return', format.bigPercent(c.total_return[index]));
      html += row('Worst drawdown', format.percent(-c.max_drawdown[index], 1));
      html += row('Trades', format.integer(c.trades[index]));
      if (index === this.result.best?.index) html += '<div class="row"><span>The best of the sweep</span></div>';
    }
    this.tooltip.innerHTML = html;
    this.tooltip.hidden = false;
    const left = px + 18 + this.tooltip.offsetWidth > g.width ? px - 18 - this.tooltip.offsetWidth : px + 18;
    this.tooltip.style.left = `${Math.max(4, left)}px`;
    this.tooltip.style.top = `${Math.max(4, Math.min(py - 10, g.height - this.tooltip.offsetHeight - 4))}px`;
  }
}
