// The light and dark themes, and the choice between them: the system's, until light or dark is chosen. A choice is
// remembered, and index.html applies it before the page first paints. The canvases re-read their colors on a change.
import { $, $$ } from './dom.js';
import { readPalette } from './palette.js';

const KEY = 'nanotrade-theme';
const PAGE_COLOR = { light: '#ffffff', dark: '#0a0a0a' };

let redrawCanvases = () => {};
const system = matchMedia('(prefers-color-scheme: dark)');

function stored() {
  try {
    const value = localStorage.getItem(KEY);
    return value === 'light' || value === 'dark' ? value : null;
  } catch {
    return null;
  }
}

// 'system', 'light', or 'dark'. It's kept here as well as in storage, which can be off, in a private window for one:
// then a choice lasts for this visit.
let choice = stored() ?? 'system';

export const currentTheme = () => (document.documentElement.dataset.theme === 'dark' ? 'dark' : 'light');
export const themeChoice = () => choice;
const resolve = (value) => (value === 'system' ? (system.matches ? 'dark' : 'light') : value);

function apply(name) {
  document.documentElement.dataset.theme = name;
  $('meta[name="theme-color"]').content = PAGE_COLOR[name];
  readPalette();
  const next = name === 'dark' ? 'light' : 'dark';
  const button = $('#theme');
  button.innerHTML = `<svg class="icon"><use href="#i-${next === 'dark' ? 'moon' : 'sun'}"/></svg>`;
  button.title = `${next === 'dark' ? 'Dark' : 'Light'} theme (T)`;
  button.setAttribute('aria-label', `Switch to the ${next} theme`);
  for (const radio of $$('[data-theme-choice]')) {
    const on = radio.dataset.themeChoice === choice;
    radio.setAttribute('aria-checked', String(on));
    radio.tabIndex = on ? 0 : -1;
  }
}

// Shows a theme, cross-fading to it where the browser can.
function show(name) {
  if (name === currentTheme()) {
    apply(name);
    return;
  }
  const change = () => {
    apply(name);
    redrawCanvases();
  };
  if (document.startViewTransition && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
    document.startViewTransition(change).ready.catch(() => {});
  } else {
    change();
  }
}

export function setTheme(value) {
  choice = value;
  try {
    if (value === 'system') localStorage.removeItem(KEY);
    else localStorage.setItem(KEY, value);
  } catch {
    // Remembered for this visit only.
  }
  show(resolve(value));
}

export const toggleTheme = () => setTheme(currentTheme() === 'dark' ? 'light' : 'dark');

export function startTheme(redraw) {
  redrawCanvases = redraw;
  apply(currentTheme());
  $('#theme').addEventListener('click', toggleTheme);
  // The footer's switch: three radio buttons, the arrow keys moving between them as a radio group's do.
  const radios = $$('[data-theme-choice]');
  radios.forEach((radio, index) => {
    radio.addEventListener('click', () => setTheme(radio.dataset.themeChoice));
    radio.addEventListener('keydown', (event) => {
      const step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[event.key];
      if (!step) return;
      event.preventDefault();
      const next = radios[(index + step + radios.length) % radios.length];
      next.focus();
      setTheme(next.dataset.themeChoice);
    });
  });
  system.addEventListener('change', () => {
    if (choice === 'system') show(resolve('system'));
  });
}
