// Keyboard shortcuts, and the list that ? opens. Typing in a field, or holding Ctrl, Cmd, or Alt, leaves keys alone.
import { $, element } from './dom.js';

// The command menu's modifier, as the keyboard labels it.
export const MOD = /mac|iphone|ipad/i.test(navigator.userAgentData?.platform || navigator.platform || '') ? '⌘' : 'Ctrl';

export const SHORTCUTS = [
  [`${MOD} K`, 'Search markets, strategies, views, and actions'],
  ['1 2 3 4 5', 'Backtest, optimize, walk-forward, Monte Carlo, or portfolio'],
  ['[ ]', 'The previous or next market'],
  ['N', 'Another random market'],
  ['O', 'Open a CSV of daily bars'],
  ['E', 'Edit the rules as a custom strategy'],
  ['F', 'The rule language and its functions'],
  ['Enter', 'Run the open research view again, or stop it'],
  ['L', 'Logarithmic or linear scale'],
  ['C', 'Candles when zoomed in'],
  ['R', 'Show the whole range'],
  ['S', 'Copy a link to this backtest'],
  ['A', 'This run as an API request: JSON, the command line, or curl'],
  ['P', 'Pin this backtest, to compare the next ones with it, or unpin it'],
  ['T', 'Light or dark theme'],
  ['?', 'This list'],
];

const typing = (target) => target instanceof HTMLElement
  && (target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName));
const activates = (event) => (event.key === 'Enter' || event.key === ' ') && event.target instanceof Element
  && event.target.closest('button, a[href], [role="tab"], [role="radio"], [tabindex]:not([tabindex="-1"])');

export function startShortcuts(actions) {
  const list = $('#shortcut-list');
  list.replaceChildren(...SHORTCUTS.flatMap(([keys, text]) => [
    element('dt', {}, keys.split(' ').map((key) => element('kbd', {}, key))),
    element('dd', {}, text),
  ]));
  document.addEventListener('keydown', (event) => {
    if (event.defaultPrevented || event.ctrlKey || event.metaKey || event.altKey || typing(event.target) || activates(event)) return;
    if (document.querySelector('dialog[open]') && event.key !== '?') return;
    const action = actions[event.key] ?? actions[event.key.toLowerCase()];
    if (action) {
      event.preventDefault();
      action();
    }
  });
  for (const dialog of document.querySelectorAll('dialog')) {
    dialog.addEventListener('click', (event) => {
      if (event.target === dialog || event.target.closest('[data-close]')) dialog.close();
    });
  }
}
