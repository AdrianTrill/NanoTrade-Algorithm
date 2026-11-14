// A chart of stacked panes over one time axis, set like a printed exhibit: each pane titled in small capitals above
// it, horizontal rules only, values in a monospace on the right, and an ink baseline with tick marks under the last.
// Wheel or pinch to zoom, drag to pan, double-click to see everything. Thousands of bars are drawn as one segment per
// pixel column spanning the column's range, so a long history stays crisp and quick at any width.
import { crisp, font, linearTicks, logTicks, mono, surface, tag } from './canvas.js';
import { palette } from './palette.js';

const AXIS = 68;    // the value axis, on the right
const BOTTOM = 26;  // the time axis
const TITLE = 19;   // a pane's title, above it
const GAP = 12;     // between panes
const LEFT = 1;
const DAY = 86400;
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

const finite = (v) => typeof v === 'number' && Number.isFinite(v);

export class TimeChart {
  constructor(canvas, { tooltip = null, onHover = null, minSpan = 15 } = {}) {
    this.canvas = canvas;
    this.tooltip = tooltip;
    this.onHover = onHover;
    this.minSpan = minSpan;
    this.data = null;
    this.start = 0;
    this.end = 0;
    this.hover = null;
    this.highlight = null;
    this.candles = false;
    this.layout = [];
    this.pointers = new Map();
    this.frame = 0;
    this.listen();
    new ResizeObserver(() => this.draw()).observe(canvas);
  }

  get length() {
    return this.data ? this.data.times.length : 0;
  }

  setData(data, { keepView = false } = {}) {
    const previous = this.data;
    this.data = data;
    const n = data.times.length;
    const sameAxis = previous && previous.times.length === n && previous.times[0] === data.times[0];
    if (!keepView || !sameAxis) {
      this.start = 0;
      this.end = n;
    }
    this.draw();
  }

  reset() {
    this.start = 0;
    this.end = this.length;
    this.draw();
  }

  zoomTo(first, last, margin = 0.15) {
    const span = Math.max(this.minSpan, last - first + 1);
    const pad = span * margin;
    this.setView(first - pad, last + 1 + pad);
  }

  setView(start, end) {
    const n = this.length;
    const span = Math.min(n, Math.max(this.minSpan, end - start));
    let s = Math.max(0, Math.min(start, n - span));
    if (!Number.isFinite(s)) s = 0;
    this.start = s;
    this.end = s + span;
    this.draw();
  }

  setHighlight(trade) {
    this.highlight = trade;
    this.draw();
  }

  draw() {
    if (this.frame) return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      this.render();
    });
  }

  plotWidth() {
    return this.canvas.clientWidth - AXIS - LEFT;
  }

  indexAt(px) {
    const scale = this.plotWidth() / Math.max(1e-9, this.end - this.start);
    return Math.round(this.start + (px - LEFT) / scale - 0.5);
  }

  render() {
    const { context: ctx, width, height } = surface(this.canvas);
    this.layout = [];
    if (!this.data || this.length === 0 || width < 80 || height < 60) return;
    const { times, panes } = this.data;
    const plotWidth = width - AXIS - LEFT;
    const scale = plotWidth / (this.end - this.start);
    const x = (i) => LEFT + (i - this.start + 0.5) * scale;
    const i0 = Math.max(0, Math.floor(this.start));
    const i1 = Math.min(this.length - 1, Math.ceil(this.end));

    // Fixed panes (the position ribbon) keep their height; the others share the rest by weight.
    const titles = panes.reduce((sum, pane) => sum + (pane.title ? TITLE : 0), 0);
    const fixed = panes.reduce((sum, pane) => sum + (pane.fixed ?? 0), 0);
    const available = height - BOTTOM - titles - fixed - GAP * (panes.length - 1);
    const totalWeight = panes.reduce((sum, pane) => sum + (pane.fixed ? 0 : pane.weight ?? 1), 0);
    let top = 0;
    for (const pane of panes) {
      if (pane.title) {
        this.renderTitle(ctx, pane, top, plotWidth);
        top += TITLE;
      }
      const paneHeight = pane.fixed ?? Math.max(30, (available * (pane.weight ?? 1)) / totalWeight);
      const box = { pane, top, bottom: top + paneHeight, height: paneHeight };
      this.layout.push(box);
      if (pane.kind === 'ribbon') this.renderRibbon(ctx, box, x, scale, i0, i1, plotWidth);
      else this.renderPane(ctx, box, x, scale, i0, i1, plotWidth);
      top += paneHeight + GAP;
    }
    this.renderTimeAxis(ctx, times, x, i0, i1, height, plotWidth);
    this.renderCrosshair(ctx, x, width, height);
  }

  renderTitle(ctx, pane, top, plotWidth) {
    ctx.font = font(12, 500);
    ctx.fillStyle = palette.text2;
    ctx.textAlign = 'left';
    const title = pane.title;
    ctx.fillText(title, LEFT, top + 12);
    if (pane.note) {
      // The note goes at the right, if it fits beside the title.
      const titleWidth = ctx.measureText(title).width;
      ctx.font = font(11.5, 400);
      const note = pane.note;
      if (titleWidth + ctx.measureText(note).width + 24 < plotWidth) {
        ctx.fillStyle = palette.muted;
        ctx.textAlign = 'right';
        ctx.fillText(note, LEFT + plotWidth, top + 12);
      }
    }
  }

  // The visible range of a pane's values, padded, as a mapping to y.
  scaleFor(box, i0, i1) {
    const { pane } = box;
    let low = Infinity;
    let high = -Infinity;
    const take = (v) => {
      if (finite(v)) {
        if (v < low) low = v;
        if (v > high) high = v;
      }
    };
    if (pane.range) {
      [low, high] = pane.range;
    } else {
      for (const series of pane.series) {
        if (series.hidden) continue;
        const lists = series.kind === 'candles' ? [series.high, series.low]
          : series.kind === 'band' ? [series.upper, series.lower] : [series.values];
        for (const list of lists) {
          for (let i = i0; i <= i1; i += 1) take(list[i]);
        }
        if (series.kind === 'histogram' || series.baseline === 0) take(0);
      }
      for (const guide of pane.guides ?? []) {
        if (guide.include) take(guide.value);
      }
      if (!Number.isFinite(low)) {
        low = 0;
        high = 1;
      }
    }
    const log = pane.log && low > 0;
    const f = log ? Math.log10 : (v) => v;
    let a = f(low);
    let b = f(high);
    if (a === b) {
      a -= log ? 0.05 : Math.abs(a) * 0.05 || 1;
      b += log ? 0.05 : Math.abs(b) * 0.05 || 1;
    }
    if (!pane.range) {
      const pad = (b - a) * 0.06;
      a -= pad;
      b += pad;
      if (pane.clampTop !== undefined && b > f(pane.clampTop)) b = f(pane.clampTop) + (b - a) * 0.015;
    }
    const inner = box.height - 2;
    const y = (v) => box.top + 1 + (1 - (f(v) - a) / (b - a)) * inner;
    const value = (py) => {
      const raw = a + (1 - (py - box.top - 1) / inner) * (b - a);
      return log ? 10 ** raw : raw;
    };
    return { y, value, log, low: log ? 10 ** a : a, high: log ? 10 ** b : b };
  }

  renderPane(ctx, box, x, scale, i0, i1, plotWidth) {
    const { pane } = box;
    const mapping = this.scaleFor(box, i0, i1);
    box.mapping = mapping;
    const { y } = mapping;
    const right = LEFT + plotWidth;

    // Rules and values: horizontal only, values on the right, except where a line's end label sits.
    const endLabels = pane.endLabels ? this.endLabels(pane, y, i1) : [];
    ctx.font = mono(10.5);
    ctx.textAlign = 'left';
    const count = Math.max(2, Math.floor(box.height / 44));
    const ticks = mapping.log ? logTicks(mapping.low, mapping.high, count) : linearTicks(mapping.low, mapping.high, count);
    for (const tick of ticks) {
      const ty = crisp(y(tick));
      if (ty < box.top + 3 || ty > box.bottom - 1) continue;
      ctx.strokeStyle = tick === 0 && !mapping.log ? palette.borderStrong : palette.grid;
      ctx.beginPath();
      ctx.moveTo(LEFT, ty);
      ctx.lineTo(right, ty);
      ctx.stroke();
      if (endLabels.some((label) => Math.abs(label.y - ty) < 14)) continue;
      ctx.fillStyle = palette.muted;
      ctx.fillText((pane.format ?? String)(tick), right + 9, ty + 3.5);
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(LEFT, box.top - 2, plotWidth, box.height + 4);
    ctx.clip();
    for (const region of pane.regions ?? []) {
      if (region.to < i0 || region.from > i1) continue;
      ctx.fillStyle = region.fill;
      const left = x(region.from) - scale / 2;
      ctx.fillRect(left, box.top, x(region.to) + scale / 2 - left, box.height);
    }
    if (pane.trades && this.highlight) {
      const trade = this.highlight;
      ctx.fillStyle = palette.accentSoft;
      const left = x(trade.entry) - scale / 2;
      ctx.fillRect(left, box.top, Math.max(2, x(trade.exit) + scale / 2 - left), box.height);
    }
    for (const guide of pane.guides ?? []) {
      const gy = crisp(y(guide.value));
      ctx.strokeStyle = guide.color ?? palette.faint;
      ctx.setLineDash(guide.dash ?? [3, 3]);
      ctx.beginPath();
      ctx.moveTo(LEFT, gy);
      ctx.lineTo(right, gy);
      ctx.stroke();
      ctx.setLineDash([]);
    }
    for (const series of pane.series) {
      if (series.hidden) continue;
      if (series.kind === 'band') this.renderBand(ctx, series, x, y, scale, i0, i1);
      else if (series.kind === 'area') this.renderArea(ctx, series, box, x, y, scale, i0, i1);
      else if (series.kind === 'histogram') this.renderHistogram(ctx, series, x, y, scale, i0, i1);
    }
    for (const series of pane.series) {
      if (series.hidden) continue;
      if (series.kind === 'line' || series.kind === 'area') {
        ctx.strokeStyle = series.color;
        ctx.lineWidth = series.width ?? 1.4;
        ctx.lineJoin = 'round';
        ctx.setLineDash(series.dash ?? []);
        this.tracePath(ctx, series.values, x, y, scale, i0, i1);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.lineWidth = 1;
      } else if (series.kind === 'candles') {
        this.renderPrice(ctx, series, x, y, scale, i0, i1);
      }
    }
    if (pane.trades) this.renderTradeMarkers(ctx, pane, x, y, scale, i0, i1, plotWidth);
    ctx.restore();

    this.renderEndLabels(ctx, endLabels, right);
  }

  // The last visible value of each line, where its tag goes at the right edge, nudged apart so tags don't overlap.
  endLabels(pane, y, i1) {
    const labels = [];
    for (const series of pane.series) {
      if (series.hidden || !series.endLabel) continue;
      let i = i1;
      while (i > 0 && !finite(series.values[i])) i -= 1;
      if (finite(series.values[i])) labels.push({ y: y(series.values[i]), text: series.endLabel(series.values[i]), color: series.color });
    }
    labels.sort((a, b) => a.y - b.y);
    for (let k = 1; k < labels.length; k += 1) {
      if (labels[k].y - labels[k - 1].y < 17) labels[k].y = labels[k - 1].y + 17;
    }
    return labels;
  }

  // Each line's last value, tagged in its color, as a printed chart labels its lines.
  renderEndLabels(ctx, labels, right) {
    ctx.font = mono(10.5, 600);
    for (const label of labels) {
      const width = Math.ceil(ctx.measureText(label.text).width) + 8;
      ctx.fillStyle = label.color;
      ctx.fillRect(right + 3, Math.round(label.y - 8), width, 16);
      ctx.fillStyle = palette.surface;
      ctx.textAlign = 'left';
      ctx.fillText(label.text, right + 7, Math.round(label.y + 4));
    }
  }

  // A line through the visible values; where several bars share a pixel column, one segment spans their range.
  tracePath(ctx, values, x, y, scale, i0, i1) {
    ctx.beginPath();
    let started = false;
    if (scale >= 0.8) {
      for (let i = i0; i <= i1; i += 1) {
        const v = values[i];
        if (!finite(v)) {
          started = false;
          continue;
        }
        if (started) ctx.lineTo(x(i), y(v));
        else ctx.moveTo(x(i), y(v));
        started = true;
      }
      return;
    }
    let column = null;
    let first = 0;
    let last = 0;
    let low = 0;
    let high = 0;
    const flush = () => {
      if (column === null) return;
      if (started) ctx.lineTo(column, y(first));
      else ctx.moveTo(column, y(first));
      started = true;
      ctx.lineTo(column, y(low));
      ctx.lineTo(column, y(high));
      ctx.lineTo(column, y(last));
      column = null;
    };
    for (let i = i0; i <= i1; i += 1) {
      const v = values[i];
      if (!finite(v)) {
        flush();
        started = false;
        continue;
      }
      const c = Math.round(x(i));
      if (c !== column) {
        flush();
        column = c;
        first = v;
        low = v;
        high = v;
      } else {
        low = Math.min(low, v);
        high = Math.max(high, v);
      }
      last = v;
    }
    flush();
  }

  renderArea(ctx, series, box, x, y, scale, i0, i1) {
    let first = -1;
    let last = -1;
    for (let i = i0; i <= i1; i += 1) {
      if (finite(series.values[i])) {
        if (first < 0) first = i;
        last = i;
      }
    }
    if (first < 0) return;
    const base = series.baseline === 0 ? y(0) : box.bottom;
    this.tracePath(ctx, series.values, x, y, scale, first, last);
    ctx.lineTo(x(last), base);
    ctx.lineTo(x(first), base);
    ctx.closePath();
    ctx.fillStyle = series.fill ?? palette.strategySoft;
    ctx.fill();
  }

  renderBand(ctx, series, x, y, scale, i0, i1) {
    ctx.beginPath();
    let started = false;
    const step = scale >= 0.8 ? 1 : Math.max(1, Math.floor(1 / scale));
    for (let i = i0; i <= i1; i += step) {
      if (!finite(series.upper[i])) continue;
      if (started) ctx.lineTo(x(i), y(series.upper[i]));
      else ctx.moveTo(x(i), y(series.upper[i]));
      started = true;
    }
    for (let i = i1 - ((i1 - i0) % step); i >= i0; i -= step) {
      if (finite(series.lower[i])) ctx.lineTo(x(i), y(series.lower[i]));
    }
    ctx.closePath();
    ctx.fillStyle = series.fill;
    ctx.fill();
  }

  renderHistogram(ctx, series, x, y, scale, i0, i1) {
    const zero = y(0);
    const width = Math.max(1, scale * 0.7);
    for (let i = i0; i <= i1; i += 1) {
      const v = series.values[i];
      if (!finite(v)) continue;
      ctx.fillStyle = v >= 0 ? series.colorUp ?? palette.upStrong : series.colorDown ?? palette.downStrong;
      const top = Math.min(zero, y(v));
      ctx.fillRect(x(i) - width / 2, top, width, Math.max(1, Math.abs(y(v) - zero)));
    }
  }

  // Candles when there is room for them and they are asked for; otherwise the close, as a line in ink.
  renderPrice(ctx, series, x, y, scale, i0, i1) {
    if (this.candles && scale >= 3) {
      const body = Math.max(1, Math.floor(scale * 0.62));
      for (let i = i0; i <= i1; i += 1) {
        const o = series.open[i];
        const c = series.close[i];
        const up = c >= o;
        const cx = crisp(x(i));
        ctx.strokeStyle = palette.text;
        ctx.beginPath();
        ctx.moveTo(cx, y(series.high[i]));
        ctx.lineTo(cx, y(series.low[i]));
        ctx.stroke();
        const top = y(Math.max(o, c));
        const h = Math.max(1, y(Math.min(o, c)) - top);
        const left = Math.round(x(i) - body / 2);
        // Hollow for a rise, filled for a fall, as candles were drawn before color.
        if (up) {
          ctx.fillStyle = palette.surface;
          ctx.fillRect(left, top, body, h);
          ctx.strokeRect(left + 0.5, Math.round(top) + 0.5, Math.max(0, body - 1), Math.max(0, Math.round(h) - 1));
        } else {
          ctx.fillStyle = palette.text;
          ctx.fillRect(left, top, body, h);
        }
      }
      return;
    }
    ctx.strokeStyle = series.color ?? palette.text;
    ctx.lineWidth = 1.2;
    ctx.lineJoin = 'round';
    this.tracePath(ctx, series.close, x, y, scale, i0, i1);
    ctx.stroke();
    ctx.lineWidth = 1;
  }

  // The position held, as a strip: long in the up color, short in the down color, flat left blank.
  renderRibbon(ctx, box, x, scale, i0, i1, plotWidth) {
    const { values } = box.pane;
    ctx.fillStyle = palette.hover;
    ctx.fillRect(LEFT, box.top, plotWidth, box.height);
    let runStart = i0;
    let runSign = Math.sign(values[i0] || 0);
    const flush = (end) => {
      if (runSign === 0) return;
      ctx.fillStyle = runSign > 0 ? palette.up : palette.down;
      const left = Math.max(LEFT, x(runStart) - scale / 2);
      const rightEdge = Math.min(LEFT + plotWidth, x(end) + scale / 2);
      ctx.fillRect(left, box.top, Math.max(1, rightEdge - left), box.height);
    };
    for (let i = i0 + 1; i <= i1; i += 1) {
      const s = Math.sign(values[i] || 0);
      if (s !== runSign) {
        flush(i - 1);
        runStart = i;
        runSign = s;
      }
    }
    flush(i1);
    if (this.highlight) {
      ctx.strokeStyle = palette.text;
      ctx.lineWidth = 1.5;
      const left = x(this.highlight.entry) - scale / 2;
      ctx.strokeRect(left, box.top - 1, Math.max(2, x(this.highlight.exit) + scale / 2 - left), box.height + 2);
      ctx.lineWidth = 1;
    }
  }

  // Entries as triangles and exits as rings, when they are sparse enough to read; the ribbon shows the rest.
  renderTradeMarkers(ctx, pane, x, y, scale, i0, i1, plotWidth) {
    const visible = pane.trades.filter((t) => t.exit >= i0 && t.entry <= i1);
    const sparse = visible.length <= plotWidth / 20;
    const size = scale >= 6 ? 5 : 4;
    const draw = (trade, emphasis) => {
      const s = emphasis ? size + 1.5 : size;
      const ex = x(trade.entry);
      const ey = y(trade.entryPrice);
      ctx.fillStyle = trade.direction > 0 ? palette.up : palette.down;
      ctx.beginPath();
      if (trade.direction > 0) {
        ctx.moveTo(ex, ey + 3);
        ctx.lineTo(ex - s, ey + 3 + s * 1.7);
        ctx.lineTo(ex + s, ey + 3 + s * 1.7);
      } else {
        ctx.moveTo(ex, ey - 3);
        ctx.lineTo(ex - s, ey - 3 - s * 1.7);
        ctx.lineTo(ex + s, ey - 3 - s * 1.7);
      }
      ctx.closePath();
      ctx.fill();
      if (!trade.open) {
        ctx.strokeStyle = trade.win ? palette.up : palette.down;
        ctx.lineWidth = 1.6;
        ctx.fillStyle = palette.surface;
        ctx.beginPath();
        ctx.arc(x(trade.exit), y(trade.exitPrice), s * 0.72, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();
        ctx.lineWidth = 1;
      }
    };
    if (sparse) {
      for (const trade of visible) if (trade !== this.highlight) draw(trade, false);
    }
    const trade = this.highlight;
    if (trade && trade.exit >= i0 && trade.entry <= i1) {
      ctx.strokeStyle = palette.text;
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(x(trade.entry), y(trade.entryPrice));
      ctx.lineTo(x(trade.exit), y(trade.exitPrice));
      ctx.stroke();
      ctx.setLineDash([]);
      draw(trade, true);
    }
  }

  renderTimeAxis(ctx, times, x, i0, i1, height, plotWidth) {
    const axis = crisp(height - BOTTOM);
    ctx.strokeStyle = palette.text;
    ctx.beginPath();
    ctx.moveTo(LEFT, axis);
    ctx.lineTo(LEFT + plotWidth, axis);
    ctx.stroke();
    ctx.font = mono(10.5);
    ctx.fillStyle = palette.muted;
    ctx.textAlign = 'center';
    let lastX = -Infinity;
    for (const tick of timeTicks(times, i0, i1, plotWidth)) {
      const tx = x(tick.index);
      if (tx < LEFT + 16 || tx > LEFT + plotWidth - 16 || tx - lastX < 60) continue;
      lastX = tx;
      ctx.strokeStyle = palette.text;
      ctx.beginPath();
      ctx.moveTo(crisp(tx), axis);
      ctx.lineTo(crisp(tx), axis + 4);
      ctx.stroke();
      ctx.fillText(tick.label, tx, axis + 16);
    }
  }

  renderCrosshair(ctx, x, width, height) {
    if (!this.hover || !this.data) return;
    const { index, py } = this.hover;
    if (index < 0 || index >= this.length) return;
    const cx = crisp(x(index));
    ctx.strokeStyle = palette.crosshair;
    ctx.beginPath();
    ctx.moveTo(cx, 0);
    ctx.lineTo(cx, height - BOTTOM);
    ctx.stroke();
    const box = this.layout.find((b) => py >= b.top && py <= b.bottom);
    if (box?.mapping) {
      ctx.setLineDash([2, 3]);
      ctx.beginPath();
      ctx.moveTo(LEFT, crisp(py));
      ctx.lineTo(width - AXIS, crisp(py));
      ctx.stroke();
      ctx.setLineDash([]);
      const value = box.mapping.value(py);
      tag(ctx, (box.pane.tagFormat ?? box.pane.format ?? String)(value), width - AXIS + 2, py);
    }
    const label = new Date(this.data.times[index] * 1000).toISOString().slice(0, 10);
    tag(ctx, label, cx, height - BOTTOM + 13, 'center');
  }

  listen() {
    const canvas = this.canvas;
    const position = (event) => {
      const rect = canvas.getBoundingClientRect();
      return { px: event.clientX - rect.left, py: event.clientY - rect.top };
    };
    let drag = null;
    let pinch = null;

    canvas.addEventListener('pointerdown', (event) => {
      if (!this.data) return;
      canvas.setPointerCapture(event.pointerId);
      this.pointers.set(event.pointerId, position(event));
      if (this.pointers.size === 2) {
        const [a, b] = [...this.pointers.values()];
        pinch = { distance: Math.hypot(a.px - b.px, a.py - b.py), start: this.start, end: this.end, center: (a.px + b.px) / 2 };
        drag = null;
      } else {
        drag = { px: position(event).px, start: this.start, end: this.end };
      }
    });

    canvas.addEventListener('pointermove', (event) => {
      if (!this.data) return;
      const point = position(event);
      if (this.pointers.has(event.pointerId)) this.pointers.set(event.pointerId, point);
      const width = this.plotWidth();
      if (pinch && this.pointers.size === 2) {
        const [a, b] = [...this.pointers.values()];
        const distance = Math.hypot(a.px - b.px, a.py - b.py);
        const span = (pinch.end - pinch.start) * (pinch.distance / Math.max(10, distance));
        const anchor = pinch.start + ((pinch.center - LEFT) / width) * (pinch.end - pinch.start);
        const ratio = (anchor - pinch.start) / (pinch.end - pinch.start);
        this.setView(anchor - ratio * span, anchor - ratio * span + span);
        return;
      }
      if (drag && event.buttons) {
        const shift = ((point.px - drag.px) / width) * (drag.end - drag.start);
        this.setView(drag.start - shift, drag.end - shift);
      }
      this.moveHover(point);
    });

    const release = (event) => {
      this.pointers.delete(event.pointerId);
      if (this.pointers.size < 2) pinch = null;
      drag = null;
    };
    canvas.addEventListener('pointerup', release);
    canvas.addEventListener('pointercancel', release);
    canvas.addEventListener('pointerleave', () => {
      this.hover = null;
      if (this.tooltip) this.tooltip.hidden = true;
      this.draw();
    });

    canvas.addEventListener('wheel', (event) => {
      if (!this.data) return;
      event.preventDefault();
      const { px } = position(event);
      const width = this.plotWidth();
      const anchor = this.start + ((px - LEFT) / width) * (this.end - this.start);
      if (Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
        const shift = (event.deltaX / width) * (this.end - this.start);
        this.setView(this.start + shift, this.end + shift);
        return;
      }
      const factor = Math.exp(event.deltaY * (event.deltaMode === 1 ? 0.05 : 0.0018));
      const span = Math.min(this.length, Math.max(this.minSpan, (this.end - this.start) * factor));
      const ratio = (anchor - this.start) / (this.end - this.start);
      this.setView(anchor - ratio * span, anchor - ratio * span + span);
    }, { passive: false });

    canvas.addEventListener('dblclick', () => this.reset());
  }

  moveHover({ px, py }) {
    const index = Math.max(0, Math.min(this.length - 1, this.indexAt(px)));
    this.hover = { index, px, py };
    this.draw();
    if (!this.tooltip || !this.onHover) return;
    const html = this.onHover(index);
    if (!html) {
      this.tooltip.hidden = true;
      return;
    }
    this.tooltip.innerHTML = html;
    this.tooltip.hidden = false;
    const width = this.canvas.clientWidth;
    const tipWidth = this.tooltip.offsetWidth;
    const left = px + 20 + tipWidth > width - AXIS ? px - 20 - tipWidth : px + 20;
    this.tooltip.style.left = `${Math.max(4, left)}px`;
    this.tooltip.style.top = `${Math.max(4, Math.min(py - 24, this.canvas.clientHeight - this.tooltip.offsetHeight - 34))}px`;
  }
}

// Labels at calendar boundaries: decades, years, quarters, months, or days, depending on the span in view.
function timeTicks(times, i0, i1, plotWidth) {
  const span = (times[i1] - times[i0]) / DAY;
  const perLabel = (span / Math.max(1, plotWidth)) * 84;  // days covered by about 84 pixels
  const unit = perLabel > 365 * 4 ? 'decade' : perLabel > 300 ? 'year' : perLabel > 80 ? 'quarter' : perLabel > 25 ? 'month' : 'day';
  const yearStep = unit === 'decade' ? (perLabel > 365 * 8 ? 10 : 5) : perLabel > 365 * 1.6 ? 2 : 1;
  const ticks = [];
  let previous = null;
  for (let i = i0; i <= i1; i += 1) {
    const d = new Date(times[i] * 1000);
    const year = d.getUTCFullYear();
    const month = d.getUTCMonth();
    let key;
    if (unit === 'decade' || unit === 'year') key = year;
    else if (unit === 'quarter') key = year * 4 + Math.floor(month / 3);
    else if (unit === 'month') key = year * 12 + month;
    else key = Math.floor(times[i] / DAY / (perLabel > 6 ? 7 : 1));
    if (previous !== null && key !== previous) {
      if ((unit === 'decade' || unit === 'year') && year % yearStep === 0) {
        ticks.push({ index: i, label: String(year) });
      } else if (unit === 'quarter' || unit === 'month') {
        ticks.push({ index: i, label: month === 0 ? String(year) : MONTHS[month] });
      } else if (unit === 'day') {
        ticks.push({ index: i, label: `${MONTHS[month]} ${d.getUTCDate()}` });
      }
    }
    previous = key;
  }
  return ticks;
}
