// Headline figures: a label, a number, and the benchmark's number beneath it, with a pair of bars comparing the two.
import { element } from './dom.js';

// The two magnitudes as bar lengths, in percent of the larger.
export function bars(mine, theirs) {
  const a = Math.abs(mine);
  const b = Math.abs(theirs);
  if (!Number.isFinite(a) || !Number.isFinite(b) || Math.max(a, b) === 0) return null;
  const top = Math.max(a, b);
  return [Math.max(1.5, (a / top) * 100), Math.max(1.5, (b / top) * 100)];
}

export function versus(prefix, text) {
  const node = element('span');
  node.append(`${prefix} `, element('b', {}, text));
  return node;
}

export function figure(label, value, below, { compare = null, valueClass = '', title = '' } = {}) {
  const children = [element('span', { class: 'label' }, label), element('span', { class: `value ${valueClass}`.trim() }, value)];
  if (compare) {
    children.push(element('span', { class: 'bars', 'aria-hidden': 'true' },
      element('span', { class: 'mine', style: { width: `${compare[0]}%` } }),
      element('span', { class: 'theirs', style: { width: `${compare[1]}%` } })));
  }
  children.push(element('span', { class: 'versus' }, below));
  return element('div', { class: 'figure', title }, ...children);
}
