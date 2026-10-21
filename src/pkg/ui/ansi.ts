const CODES = {
  reset: '\x1b[0m',
  bold: '\x1b[1m',
  dim: '\x1b[2m',
  ul: '\x1b[4m',
  ulOff: '\x1b[24m',
  red: '\x1b[31m',
  green: '\x1b[32m',
  yellow: '\x1b[33m',
  blue: '\x1b[34m',
  magenta: '\x1b[35m',
  cyan: '\x1b[36m',
};

export type Palette = typeof CODES;

const PLAIN = Object.fromEntries(Object.keys(CODES).map(key => [key, ''])) as Palette;

function colors(stream: NodeJS.WriteStream): boolean {
  const force = process.env.FORCE_COLOR;
  if (force !== undefined) return force !== '0' && force !== 'false';
  if (process.env.NO_COLOR) return false;
  return stream.isTTY === true && process.env.TERM !== 'dumb';
}

export const c: Palette = colors(process.stdout) ? CODES : PLAIN;

export function formatSize(bytes: number): string {
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GB`;
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(2)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(2)} KB`;
  return `${bytes} B`;
}
