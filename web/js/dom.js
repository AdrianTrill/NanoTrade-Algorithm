// Small DOM helpers.

export const $ = (selector, root = document) => root.querySelector(selector);
export const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

// element('div', {class: 'row', onclick: fn}, child, 'text', ...)
export function element(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) {
    if (value === undefined || value === null || value === false) continue;
    if (name.startsWith('on') && typeof value === 'function') {
      node.addEventListener(name.slice(2), value);
    } else if (name === 'class') {
      node.className = value;
    } else if (name === 'dataset') {
      Object.assign(node.dataset, value);
    } else if (name === 'style' && typeof value === 'object') {
      Object.assign(node.style, value);
    } else if (value === true) {
      node.setAttribute(name, '');
    } else {
      node.setAttribute(name, value);
    }
  }
  for (const child of children.flat()) {
    if (child === undefined || child === null || child === false) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

export function icon(name) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('class', 'icon');
  const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
  use.setAttribute('href', `#i-${name}`);
  svg.append(use);
  return svg;
}

export function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

// Calls fn at most once per animation frame, with the latest arguments.
export function throttleFrame(fn) {
  let pending = null;
  return (...args) => {
    const scheduled = pending !== null;
    pending = args;
    if (!scheduled) {
      requestAnimationFrame(() => {
        const latest = pending;
        pending = null;
        fn(...latest);
      });
    }
  };
}

export function debounce(fn, delay) {
  let timer = 0;
  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), delay);
  };
}
