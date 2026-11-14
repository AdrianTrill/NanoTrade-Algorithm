// The light and dark themes. Until one is chosen the page follows the system's; a choice is remembered, and
// index.html applies it before the page first paints. The canvases re-read their colors when it changes.
import { $ } from './dom.js';
import { readPalette } from './palette.js';

const KEY = 'nanotrade-theme';
const PAGE_COLOR = { light: '#ffffff', dark: '#0a0a0a' };

let redrawCanvases = () => {};
const system = matchMedia('(prefers-color-scheme: dark)');

function chosen() {
  try {
    const value = localStorage.getItem(KEY);
    return value === 'light' || value === 'dark' ? value : null;
  } catch {
    return null;
  }
}

export const currentTheme = () => (document.documentElement.dataset.theme === 'dark' ? 'dark' : 'light');

function apply(name) {
  document.documentElement.dataset.theme = name;
  $('meta[name="theme-color"]').content = PAGE_COLOR[name];
  readPalette();
  const next = name === 'dark' ? 'light' : 'dark';
  const button = $('#theme');
  button.innerHTML = `<svg class="icon"><use href="#i-${next === 'dark' ? 'moon' : 'sun'}"/></svg>`;
  button.title = `${next === 'dark' ? 'Dark' : 'Light'} theme (T)`;
  button.setAttribute('aria-label', `Switch to the ${next} theme`);
}

export function toggleTheme() {
  const next = currentTheme() === 'dark' ? 'light' : 'dark';
  try {
    localStorage.setItem(KEY, next);
  } catch {
    // Storage can be off, in a private window for one: then the theme lasts for this visit.
  }
  const change = () => {
    apply(next);
    redrawCanvases();
  };
  if (document.startViewTransition && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
    document.startViewTransition(change).ready.catch(() => {});
  } else {
    change();
  }
}

export function startTheme(redraw) {
  redrawCanvases = redraw;
  apply(currentTheme());
  $('#theme').addEventListener('click', toggleTheme);
  system.addEventListener('change', () => {
    if (chosen()) return;
    apply(system.matches ? 'dark' : 'light');
    redrawCanvases();
  });
}
