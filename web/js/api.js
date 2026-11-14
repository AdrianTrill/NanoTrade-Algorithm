// The open view's run as a request to the JSON API, ready to copy: the JSON the page sends to the engine, the command
// line that answers it, and curl for the native server. All three get the same bytes back.
import { $, $$, element } from './dom.js';
import { runtime } from './runtime.js';
import { toast } from './toast.js';

const FORMATS = ['json', 'cli', 'curl'];

// JSON with its keys, strings, numbers, and literals marked for the stylesheet.
function highlightJson(text) {
  const fragment = document.createDocumentFragment();
  const pattern = /("(?:[^"\\]|\\.)*")(\s*:)?|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|\b(true|false|null)\b/g;
  let last = 0;
  for (const match of text.matchAll(pattern)) {
    fragment.append(text.slice(last, match.index));
    if (match[1]) {
      fragment.append(element('span', { class: match[2] ? 'json-key' : 'json-string' }, match[1]));
      if (match[2]) fragment.append(match[2]);
    } else if (match[3]) {
      fragment.append(element('span', { class: 'json-number' }, match[3]));
    } else {
      fragment.append(element('span', { class: 'json-literal' }, match[4]));
    }
    last = match.index + match[0].length;
  }
  fragment.append(text.slice(last));
  return fragment;
}

export class ApiDialog {
  constructor() {
    this.dialog = $('#api');
    this.code = $('#api-code');
    this.note = $('#api-note');
    this.copyButton = $('#api-copy');
    this.format = 'json';
    this.request = null;
    this.radios = $$('#api [data-format]');
    this.radios.forEach((radio, index) => {
      radio.addEventListener('click', () => this.show(radio.dataset.format));
      radio.addEventListener('keydown', (event) => {
        const step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[event.key];
        if (!step) return;
        event.preventDefault();
        const next = this.radios[(index + step + this.radios.length) % this.radios.length];
        next.focus();
        this.show(next.dataset.format);
      });
    });
    this.copyButton.addEventListener('click', () => this.copy());
  }

  // The server that answers: this page's, if it's served natively, or else nanotrade serve on its default port.
  get endpoint() {
    return runtime === 'native' ? `${location.origin}/api` : 'http://127.0.0.1:8080/api';
  }

  open(request) {
    this.request = request;
    this.show(this.format);
    this.dialog.showModal();
  }

  text(format = this.format) {
    const json = JSON.stringify(this.request, null, 2);
    if (format === 'cli') return `nanotrade api <<'EOF'\n${json}\nEOF`;
    if (format === 'curl') return `curl -s ${this.endpoint} --data-binary @- <<'EOF'\n${json}\nEOF`;
    return json;
  }

  show(format) {
    this.format = FORMATS.includes(format) ? format : 'json';
    for (const radio of this.radios) {
      const on = radio.dataset.format === this.format;
      radio.setAttribute('aria-checked', String(on));
      radio.tabIndex = on ? 0 : -1;
    }
    const json = JSON.stringify(this.request, null, 2);
    const command = { cli: "nanotrade api <<'EOF'", curl: `curl -s ${this.endpoint} --data-binary @- <<'EOF'` }[this.format];
    this.code.replaceChildren(...(command
      ? [element('span', { class: 'json-command' }, command), '\n', highlightJson(json), '\n', element('span', { class: 'json-command' }, 'EOF')]
      : [highlightJson(json)]));
    this.code.scrollTop = 0;
    const notes = {
      json: 'What the page sends to the engine. POST it to /api on nanotrade serve, or give it to nanotrade api, and the same bytes come back.',
      cli: 'nanotrade api reads a request from a file, or from standard input as here, and prints the response.',
      curl: runtime === 'native'
        ? 'The server that serves this page answers it.'
        : 'Start the server first with nanotrade serve; it answers on port 8080.',
    };
    const data = this.request?.data;
    const uploaded = typeof data === 'string' && data.startsWith('file:');
    this.note.textContent = uploaded
      ? `${notes[this.format]} This run uses a file opened in this page: send its CSV first with a load request, or give the command line --data FILE.csv.`
      : notes[this.format];
  }

  async copy() {
    try {
      await navigator.clipboard.writeText(this.text());
    } catch {
      toast('Copy it by hand', 'The browser didn\'t allow the clipboard here: select the request and copy it.', 'error');
      return;
    }
    const label = this.copyButton.querySelector('span');
    label.textContent = 'Copied';
    this.copyButton.classList.add('done');
    clearTimeout(this.copied);
    this.copied = setTimeout(() => {
      label.textContent = 'Copy';
      this.copyButton.classList.remove('done');
    }, 1600);
  }
}
