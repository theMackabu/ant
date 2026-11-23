import fs from 'node:fs';
import path from 'node:path';

import { configPath } from './paths.ts';

const VALUES = {
  defaultRegistry: ['land', 'npm'],
  missingPackageFallback: ['none', 'npm'],
  store: ['global', 'project'],
  sharedLeaves: ['on', 'off'],
} as const;

const PREFIX = 'install.';

export type ConfigField = keyof typeof VALUES;
export type PkgConfig = { [Field in ConfigField]: (typeof VALUES)[Field][number] };
export type Source = PkgConfig['defaultRegistry'];

const DEFAULTS: PkgConfig = { defaultRegistry: 'land', missingPackageFallback: 'npm', store: 'global', sharedLeaves: 'on' };
const FIELDS = Object.keys(VALUES) as ConfigField[];

export const CONFIG_KEYS = FIELDS.map(field => ({ key: `${PREFIX}${field}`, values: VALUES[field] as readonly string[] }));

export function configField(key: string): ConfigField | undefined {
  return FIELDS.find(field => key === `${PREFIX}${field}`);
}

export function configValues(field: ConfigField): readonly string[] {
  return VALUES[field];
}

export function loadConfig(): PkgConfig {
  const config: Record<ConfigField, string> = { ...DEFAULTS };
  let text = '';
  try {
    text = fs.readFileSync(configPath(), 'utf8');
  } catch {}
  for (const line of text.split(/\r?\n/)) {
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const field = configField(line.slice(0, eq).trim());
    const value = line.slice(eq + 1).trim();
    if (field && VALUES[field].includes(value as never)) config[field] = value;
  }
  return config as PkgConfig;
}

export function saveConfig(config: Record<ConfigField, string>): void {
  const file = configPath();
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, FIELDS.map(field => `${PREFIX}${field}=${config[field]}\n`).join(''));
}
