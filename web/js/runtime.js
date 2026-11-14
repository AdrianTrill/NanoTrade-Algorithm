// Where the engine runs. A static host such as GitHub Pages serves this file, and the page runs the engine in
// WebAssembly; `nanotrade serve` answers this path with 'native' instead, and the page sends its requests to the server.
export const runtime = 'wasm';
