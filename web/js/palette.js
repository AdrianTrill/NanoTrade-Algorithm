// The theme's colors and fonts for the canvases, read from the page's CSS custom properties so the charts follow it.

const NAMES = [
  'text', 'text-2', 'muted', 'faint', 'grid', 'hover', 'border', 'border-strong', 'surface', 'surface-raised',
  'surface-sunken', 'accent', 'accent-soft', 'strategy', 'strategy-soft', 'benchmark', 'up', 'up-soft', 'up-strong',
  'down', 'down-soft', 'down-strong', 'warn', 'warn-soft', 'crosshair', 'o1', 'o2', 'o3', 'o4', 'o5', 'o6', 'o7', 'o8',
  'font-ui', 'font-mono',
];

// Keyed in camel case: --text-2 is palette.text2, --up-soft is palette.upSoft.
export const palette = {};

export function readPalette() {
  const style = getComputedStyle(document.documentElement);
  for (const name of NAMES) {
    palette[name.replace(/-(\w)/g, (_, letter) => letter.toUpperCase())] = style.getPropertyValue(`--${name}`).trim();
  }
}

// Colors for indicator overlays, in the order they are assigned.
export const overlayColor = (index) => palette[`o${(index % 8) + 1}`];
