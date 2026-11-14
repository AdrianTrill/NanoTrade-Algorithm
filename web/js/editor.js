// The rule editor: a textarea over a highlighted copy of its text, so the strategy language reads in color and an
// error is underlined where the engine found it.
import { element, escapeHtml } from './dom.js';

const COLUMNS = new Set(['open', 'high', 'low', 'close', 'volume', 'hl2']);
const KEYWORDS = new Set(['and', 'or', 'not', 'true', 'false']);
const TOKEN = /(\$[A-Za-z_]\w*)|([A-Za-z_]\w*)|(\d+\.?\d*(?:[eE][+-]?\d+)?|\.\d+)|(<=|>=|==|!=|&&|\|\||[()[\],+\-*/<>!=])|(\s+)|(.)/g;

// The engine reports positions in bytes of UTF-8; the page's strings count UTF-16 units.
export function byteToIndex(text, bytes) {
  const encoder = new TextEncoder();
  let total = 0;
  for (let i = 0; i < text.length; i += 1) {
    if (total >= bytes) return i;
    total += encoder.encode(text[i]).length;
  }
  return text.length;
}

export function highlight(source, error = null) {
  const pieces = [];
  TOKEN.lastIndex = 0;
  let match;
  while ((match = TOKEN.exec(source)) !== null) {
    const [text, param, name, num, op] = match;
    let kind = '';
    if (param) kind = 'tok-param';
    else if (name) {
      const lower = name.toLowerCase();
      const call = source.slice(TOKEN.lastIndex).trimStart().startsWith('(');
      kind = KEYWORDS.has(lower) ? 'tok-kw' : call ? 'tok-fn' : COLUMNS.has(lower) ? 'tok-col' : '';
    } else if (num) kind = 'tok-num';
    else if (op) kind = 'tok-op';
    pieces.push({ start: match.index, end: match.index + text.length, kind });
  }
  let errorStart = -1;
  let errorEnd = -1;
  if (error) {
    errorStart = byteToIndex(source, error.position);
    errorEnd = byteToIndex(source, error.position + Math.max(1, error.length));
    if (errorStart >= source.length && source.length > 0) {
      errorStart = source.length - 1;
      errorEnd = source.length;
    }
  }
  let html = '';
  for (const piece of pieces) {
    const cuts = [piece.start, piece.end];
    for (const edge of [errorStart, errorEnd]) if (edge > piece.start && edge < piece.end) cuts.push(edge);
    cuts.sort((a, b) => a - b);
    for (let k = 0; k + 1 < cuts.length; k += 1) {
      const [a, b] = [cuts[k], cuts[k + 1]];
      const classes = [piece.kind, a >= errorStart && b <= errorEnd && errorStart >= 0 ? 'tok-error' : ''].filter(Boolean).join(' ');
      const text = escapeHtml(source.slice(a, b));
      html += classes ? `<span class="${classes}">${text}</span>` : text;
    }
  }
  return `${html}\n`;  // the trailing newline keeps the mirror as tall as the textarea
}

export class RuleEditor {
  constructor({ label, placeholder = '', value = '', onInput }) {
    this.pre = element('pre', { 'aria-hidden': 'true' });
    this.textarea = element('textarea', { spellcheck: 'false', autocapitalize: 'off', autocomplete: 'off', placeholder, rows: 1, 'aria-label': label });
    this.box = element('div', { class: 'editor' }, this.pre, this.textarea);
    this.node = element('div', { class: 'rule' }, element('span', { class: 'rule-label' }, label), this.box);
    this.error = null;
    this.textarea.value = value;
    this.textarea.addEventListener('input', () => {
      this.error = null;
      this.render();
      onInput?.(this.textarea.value);
    });
    this.textarea.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' && !event.shiftKey) event.preventDefault();  // rules are one line
    });
    this.render();
  }

  get value() {
    return this.textarea.value;
  }

  set value(text) {
    this.textarea.value = text;
    this.render();
  }

  setError(error) {
    this.error = error;
    this.render();
  }

  render() {
    this.pre.innerHTML = this.textarea.value ? highlight(this.textarea.value, this.error) : `<span class="tok-op">${escapeHtml(this.textarea.placeholder)}</span>\n`;
    this.box.classList.toggle('invalid', Boolean(this.error));
  }
}

// A read-only rule, highlighted.
export function ruleBlock(label, source) {
  const code = element('div', { class: 'rule-code' });
  code.innerHTML = highlight(source).trimEnd();
  return element('div', { class: 'rule' }, element('span', { class: 'rule-label' }, label), code);
}
