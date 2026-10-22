import { color, write } from './output.ts';

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

const PLAIN = Object.fromEntries(Object.keys(CODES).map(key => [key, ''])) as typeof CODES;

export const c = color('bold', 'x') === 'x' ? PLAIN : CODES;

export function error(message: string): void {
  write('stderr', `Error: ${message}\n`);
}
