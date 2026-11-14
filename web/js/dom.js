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

// Marks the edges of a scrolling element that have more beyond them, as data-more="top bottom left right", for the
// stylesheet to fade. It reads the element's size only once the browser reports it laid out, never forcing a layout.
export function scrollEdges(node) {
  const update = () => {
    const more = [];
    if (node.scrollTop > 1) more.push('top');
    if (node.scrollTop + node.clientHeight < node.scrollHeight - 1) more.push('bottom');
    if (node.scrollLeft > 1) more.push('left');
    if (node.scrollLeft + node.clientWidth < node.scrollWidth - 1) more.push('right');
    const value = more.join(' ');
    if (node.dataset.more !== value) node.dataset.more = value;
  };
  const sizes = new ResizeObserver(update);
  const watch = () => {
    sizes.disconnect();
    sizes.observe(node);
    for (const child of node.children) sizes.observe(child);
  };
  new MutationObserver(watch).observe(node, { childList: true });
  node.addEventListener('scroll', update, { passive: true });
  watch();
}

// Lets the browser paint, and handle input, before the work that follows: a task of its own, without the clamping that
// setTimeout gets.
export function yieldToBrowser() {
  if (globalThis.scheduler?.yield) return globalThis.scheduler.yield();
  return new Promise((resolve) => {
    const channel = new MessageChannel();
    channel.port1.onmessage = () => resolve();
    channel.port2.postMessage(null);
  });
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

// A tablist as the keyboard expects one: only the selected tab is in the tab order, and the arrow keys, Home, and
// End move between the tabs, choosing each as they go.
// Rows that do something when clicked, reachable from the keyboard as well: Tab enters the table at one row, the arrow
// keys, Home, and End move between its rows, and Enter or Space does what a click does.
export function clickableRows(body) {
  const rows = [...body.rows];
  rows.forEach((row, index) => (row.tabIndex = index ? -1 : 0));
  body.addEventListener('keydown', (event) => {
    const row = event.target.closest('tr');
    const index = rows.indexOf(row);
    if (index < 0) return;
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      row.click();
      return;
    }
    const next = { ArrowDown: index + 1, ArrowUp: index - 1, Home: 0, End: rows.length - 1 }[event.key];
    if (next === undefined || next < 0 || next >= rows.length) return;
    event.preventDefault();
    row.tabIndex = -1;
    rows[next].tabIndex = 0;
    rows[next].focus();
  });
}

export function rovingTabs(list) {
  const tabs = () => [...list.querySelectorAll('[role="tab"]')];
  const sync = () => {
    for (const tab of tabs()) tab.tabIndex = tab.getAttribute('aria-selected') === 'true' ? 0 : -1;
  };
  list.addEventListener('keydown', (event) => {
    const all = tabs();
    const index = all.indexOf(document.activeElement);
    const next = { ArrowRight: index + 1, ArrowLeft: index - 1, Home: 0, End: all.length - 1 }[event.key];
    if (index < 0 || next === undefined || event.altKey || event.ctrlKey || event.metaKey) return;
    event.preventDefault();
    const tab = all[(next + all.length) % all.length];
    tab.focus();
    tab.click();
  });
  new MutationObserver(sync).observe(list, { subtree: true, attributeFilter: ['aria-selected'] });
  sync();
}
