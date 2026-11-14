// The day and night editions. The choice is remembered (index.html applies it before the page first paints), and
// the canvases re-read their colors when it changes.
import { $ } from './dom.js';
import { readPalette } from './palette.js';

const KEY = 'nanotrade-theme';
const PAGE_COLOR = { light: '#f4efe5', dark: '#14130f' };

let redrawCanvases = () => {};

export const currentTheme = () => (document.documentElement.dataset.theme === 'dark' ? 'dark' : 'light');

function apply(name) {
  document.documentElement.dataset.theme = name;
  $('meta[name="theme-color"]').content = PAGE_COLOR[name];
  readPalette();
  const next = name === 'dark' ? 'light' : 'dark';
  const button = $('#theme');
  const label = next === 'dark' ? 'Night' : 'Day';
  button.innerHTML = `<svg class="icon"><use href="#i-${next === 'dark' ? 'moon' : 'sun'}"/></svg><span>${label}</span>`;
  button.title = `${label} edition (T)`;
  button.setAttribute('aria-label', `Switch to the ${label.toLowerCase()} edition`);
}

export function toggleTheme() {
  const next = currentTheme() === 'dark' ? 'light' : 'dark';
  try {
    localStorage.setItem(KEY, next);
  } catch {
    // Storage can be off, in a private window for one: then the edition lasts for this visit.
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
}
