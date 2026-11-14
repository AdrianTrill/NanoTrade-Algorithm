// Notifications in the corner.
import { $, element } from './dom.js';

export function toast(title, detail = '', kind = 'info', timeout = 5000) {
  const node = element('div', { class: `toast ${kind}`, role: kind === 'error' ? 'alert' : 'status' },
    element('b', {}, title), detail ? element('span', {}, detail) : null);
  $('#toasts').append(node);
  setTimeout(() => node.remove(), timeout);
}
