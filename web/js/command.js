// The command menu, as a Vercel dashboard has: ⌘K (Ctrl+K elsewhere), the Search button, or a breadcrumb opens it,
// and a few letters find a market, a strategy, a view, or anything the page's buttons and keys do. A breadcrumb opens
// it narrowed to its own kind, starting on the current choice.
import { $, $$, element, icon } from './dom.js';
import { MOD } from './shortcuts.js';

const PLACEHOLDERS = {
  data: 'Find a market…',
  strategy: 'Find a strategy…',
};

export class CommandMenu {
  // `commands` lists them as the menu opens: { group, label, keywords, scope, icon, hint, current, run }.
  constructor(commands) {
    this.commands = commands;
    this.dialog = $('#command');
    this.input = $('#command-input');
    this.list = $('#command-list');
    this.empty = $('#command-empty');
    this.all = [];
    this.items = [];
    this.active = -1;
    this.input.addEventListener('input', () => this.filter());
    this.input.addEventListener('keydown', (event) => this.key(event));
    this.list.addEventListener('pointermove', (event) => {
      const option = event.target.closest('[role="option"]');
      if (option) this.activate(Number(option.dataset.index), { scroll: false });
    });
    this.list.addEventListener('click', (event) => {
      const option = event.target.closest('[role="option"]');
      if (option) this.choose(Number(option.dataset.index));
    });
    document.addEventListener('keydown', (event) => {
      if ((event.metaKey || event.ctrlKey) && !event.altKey && !event.shiftKey && event.key.toLowerCase() === 'k') {
        event.preventDefault();
        if (this.dialog.open) this.dialog.close();
        else this.open();
      }
    });
    for (const kbd of $$('[data-mod-k]')) kbd.textContent = `${MOD} K`;
  }

  open(scope = null) {
    for (const dialog of $$('dialog[open]')) dialog.close();
    this.scope = scope;
    this.all = this.commands().filter((command) => !scope || command.scope === scope);
    this.input.value = '';
    this.input.placeholder = PLACEHOLDERS[scope] ?? 'Search markets, strategies, views, and actions…';
    this.dialog.dataset.scope = scope ?? 'all';
    this.dialog.showModal();
    this.filter();
    const current = this.items.findIndex((command) => command.current);
    if (scope && current >= 0) this.activate(current);
  }

  // Every word typed must appear in a command's label, its group, or its keywords.
  filter() {
    const words = this.input.value.toLowerCase().split(/\s+/).filter(Boolean);
    this.items = this.all.filter((command) => {
      const text = `${command.label} ${command.group} ${command.keywords ?? ''}`.toLowerCase();
      return words.every((word) => text.includes(word));
    });
    this.render();
    this.activate(this.items.length ? 0 : -1);
  }

  render() {
    this.active = -1;
    this.list.hidden = !this.items.length;
    this.empty.hidden = Boolean(this.items.length);
    this.empty.textContent = this.items.length ? '' : `Nothing matches “${this.input.value.trim()}”.`;
    if (!this.items.length) {
      this.list.replaceChildren();
      return;
    }
    const groups = [];
    this.items.forEach((command, index) => {
      if (groups.at(-1)?.name !== command.group) {
        const id = `command-group-${groups.length}`;
        groups.push({ name: command.group, node: element('div', { class: 'command-group', role: 'group', 'aria-labelledby': id },
          element('div', { class: 'command-group-title', id, role: 'presentation' }, command.group)) });
      }
      groups.at(-1).node.append(element('div', {
        class: 'command-item',
        role: 'option',
        id: `command-${index}`,
        dataset: { index: String(index) },
        'aria-selected': 'false',
      },
      command.avatar ?? (command.icon ? icon(command.icon) : null),
      element('span', { class: 'command-label' }, command.label),
      command.current ? element('span', { class: 'command-current' }, icon('check'), element('span', { class: 'visually-hidden' }, ', the current one')) : null,
      command.hint ? element('kbd', {}, command.hint) : null));
    });
    this.list.replaceChildren(...groups.map((group) => group.node));
  }

  activate(index, { scroll = true } = {}) {
    if (index === this.active && index >= 0) return;
    $(`#command-${this.active}`)?.setAttribute('aria-selected', 'false');
    this.active = index;
    const option = $(`#command-${index}`);
    if (!option) {
      this.input.removeAttribute('aria-activedescendant');
      return;
    }
    option.setAttribute('aria-selected', 'true');
    this.input.setAttribute('aria-activedescendant', option.id);
    if (scroll) option.scrollIntoView({ block: 'nearest' });
  }

  key(event) {
    const count = this.items.length;
    if (!count) return;
    const moves = { ArrowDown: 1, ArrowUp: -1, PageDown: 5, PageUp: -5 };
    if (event.key in moves) {
      event.preventDefault();
      const step = moves[event.key];
      const next = Math.abs(step) === 1 ? (this.active + step + count) % count : Math.max(0, Math.min(count - 1, this.active + step));
      this.activate(next);
    } else if (event.key === 'Enter' && !event.isComposing) {
      event.preventDefault();
      this.choose(this.active);
    }
  }

  // The menu closes first, giving the focus back, so a command that moves the focus keeps it where it put it.
  choose(index) {
    const command = this.items[index];
    if (!command) return;
    this.dialog.close();
    command.run();
  }
}

// A small round gradient for a market or a strategy, from its name, as a dashboard gives a project.
export function avatar(name) {
  let hash = 0;
  for (const char of name) hash = (hash * 31 + char.codePointAt(0)) >>> 0;
  const hue = hash % 360;
  const node = element('span', { class: 'avatar', 'aria-hidden': 'true' });
  node.style.setProperty('--from', `hsl(${hue} 85% 62%)`);
  node.style.setProperty('--to', `hsl(${(hue + 50 + ((hash >>> 9) % 70)) % 360} 80% 48%)`);
  return node;
}
