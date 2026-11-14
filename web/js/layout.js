// The page's layout at its sizes. Below 1000px the report comes first and the settings are a drawer over it: a panel
// from the left on a tablet, a sheet from the bottom on a phone. While it's open the rest of the page is inert, so
// focus stays in it, but the report shows through, and every change reruns the backtest at once.
import { $, $$ } from './dom.js';
import { state } from './state.js';

const narrow = matchMedia('(max-width: 1000px)');
const phone = matchMedia('(max-width: 640px)');

// Keeps --masthead-height at what the masthead keeps in view as the page scrolls (on a wide screen its top row scrolls
// away and the views stay), so a jump to part of the page stops below it. Once that row is out of sight the page is
// `stuck`, which slides the mark into the row of views, as a dashboard's does.
export function trackMasthead() {
  const masthead = $('.masthead');
  const root = document.documentElement;
  let lift = 0;  // how far the masthead rises before it sticks: its top row's height, or nothing on a narrow screen
  const stick = () => root.classList.toggle('stuck', lift > 0 && window.scrollY >= lift);
  const update = () => {
    lift = -Math.min(0, parseFloat(getComputedStyle(masthead).top) || 0);
    root.style.setProperty('--masthead-height', `${masthead.offsetHeight - lift}px`);
    stick();
  };
  new ResizeObserver(update).observe(masthead);
  narrow.addEventListener('change', update);
  addEventListener('scroll', stick, { passive: true });
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
      this.hideWhenClosed();
    });
    this.hideWhenClosed();
  }

  // A closed drawer is out of sight, and out of reach of the keyboard and assistive technology too.
  hideWhenClosed() {
    $('#spec').inert = narrow.matches && !this.isOpen;
  }

  get isOpen() {
    return document.documentElement.classList.contains('spec-open');
  }

  // Shows the settings: a no-op on a wide screen, where they're always in view.
  open({ focus = true } = {}) {
    if (!narrow.matches || this.isOpen) return;
    document.documentElement.classList.add('spec-open');
    this.hideWhenClosed();
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
    this.hideWhenClosed();
    this.toggle.setAttribute('aria-expanded', 'false');
    this.scrim.hidden = true;
    for (const node of $$('.masthead, #main, .colophon')) node.inert = false;
    if (restoreFocus && hadFocus) this.toggle.focus({ preventScroll: true });
  }
}
