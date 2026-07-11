import { createHash } from 'node:crypto';

const COLORS: Record<string, string> = {
  red: '31',
  green: '32',
  yellow: '33',
  blue: '34',
  magenta: '35',
  cyan: '36',
  dim: '2',
  bold: '1'
};

const useColor = process.stdout.isTTY && process.env.NO_COLOR === undefined;

export function styleText(color: keyof typeof COLORS | string, text: string): string {
  const code = COLORS[color];
  if (!useColor || !code) return text;
  return `\x1b[${code}m${text}\x1b[0m`;
}

export function prettyTime(ms: number): string {
  if (ms < 1000) return `${Math.round(ms)}ms`;
  return `${(ms / 1000).toFixed(2)}s`;
}

export function sha256hex(data: Uint8Array | string): string {
  return createHash('sha256').update(data).digest('hex');
}
