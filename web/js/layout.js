// The page's layout at its sizes. Below 1000px the report comes first and the settings are a drawer over it: a panel
// from the left on a tablet, a sheet from the bottom on a phone. While it's open the rest of the page is inert, so
// focus stays in it, but the report shows through, and every change reruns the backtest at once.
import { $, $$ } from './dom.js';
import { state } from './state.js';

const narrow = matchMedia('(max-width: 1000px)');
const phone = matchMedia('(max-width: 640px)');

// Keeps --masthead-height current, so a jump to part of the page stops below the sticky masthead.
export function trackMasthead() {
  const masthead = $('.masthead');
  const update = () => document.documentElement.style.setProperty('--masthead-height', `${masthead.offsetHeight}px`);
  new ResizeObserver(update).observe(masthead);
  update();
}

export class Drawer {
  constructor() {
    this.toggle = $('#spec-toggle');
    this.scrim = $('#scrim');
    this.toggle.addEventListener('click', () => (this.isOpen ? this.close() : this.open()));
    $('#spec-close').addEventListener('click', () => this.close());
    this.scrim.addEventListener('click', () => this.close());
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && this.isOpen && !document.querySelector('dialog[open]')) this.close();
    });
    narrow.addEventListener('change', () => {
      if (!narrow.matches) this.close({ restoreFocus: false });
    });
  }

  get isOpen() {
    return document.documentElement.classList.contains('spec-open');
  }

  // Shows the settings: a no-op on a wide screen, where they're always in view.
  open({ focus = true } = {}) {
    if (!narrow.matches || this.isOpen) return;
    document.documentElement.classList.add('spec-open');
    this.toggle.setAttribute('aria-expanded', 'true');
    this.scrim.hidden = false;
    for (const node of $$('.masthead, #main, .colophon')) node.inert = true;
    // The sheet covers the lower part of a phone's screen: keep what a change changes in view above it.
    if (phone.matches) (state.view === 'backtest' ? $('#kpis') : $('#main')).scrollIntoView({ block: 'start' });
    if (focus) $('#spec-close').focus({ preventScroll: true });
  }

  close({ restoreFocus = true } = {}) {
    if (!this.isOpen) return;
    const hadFocus = $('#spec').contains(document.activeElement);
    document.documentElement.classList.remove('spec-open');
    this.toggle.setAttribute('aria-expanded', 'false');
    this.scrim.hidden = true;
    for (const node of $$('.masthead, #main, .colophon')) node.inert = false;
    if (restoreFocus && hadFocus) this.toggle.focus({ preventScroll: true });
  }
}
