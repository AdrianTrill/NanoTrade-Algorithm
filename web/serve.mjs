// A static file server for the site's checks, serving as GitHub Pages does: no API, so the page runs its engine in
// WebAssembly. node web/serve.mjs SITE_DIR [PORT], or import { serve } for a server on a free port.
import { createReadStream, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.csv': 'text/csv; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.wasm': 'application/wasm',
};

export function serve(directory, port = 0) {
  const root = resolve(directory);
  const server = createServer((request, response) => {
    const path = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
    let file = normalize(join(root, path));
    if (file !== root && !file.startsWith(root + sep)) {
      response.writeHead(403).end();
      return;
    }
    try {
      if (statSync(file).isDirectory()) file = join(file, 'index.html');
      const size = statSync(file).size;
      response.writeHead(200, { 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream', 'Content-Length': size });
      createReadStream(file).pipe(response);
    } catch {
      response.writeHead(404, { 'Content-Type': 'text/plain' }).end('not found\n');
    }
  });
  return new Promise((done) => {
    server.listen(port, '127.0.0.1', () => done({ server, url: `http://127.0.0.1:${server.address().port}/` }));
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [directory = '.', port = '8080'] = process.argv.slice(2);
  const { url } = await serve(directory, Number(port));
  console.log(`serving ${directory} at ${url}`);
}
