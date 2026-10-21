import fs from 'node:fs';
import path from 'node:path';

import { configPath } from './paths.ts';

export type Source = 'land' | 'npm';

export interface PkgConfig {
  defaultRegistry: Source;
  missingPackageFallback: 'none' | 'npm';
  store: 'global' | 'project';
}

export const CONFIG_KEYS = {
  'install.defaultRegistry': ['land', 'npm'],
  'install.missingPackageFallback': ['none', 'npm'],
  'install.store': ['global', 'project'],
} as const;

export type ConfigKey = keyof typeof CONFIG_KEYS;

const FIELDS: Record<ConfigKey, keyof PkgConfig> = {
  'install.defaultRegistry': 'defaultRegistry',
  'install.missingPackageFallback': 'missingPackageFallback',
  'install.store': 'store',
};

const DEFAULTS: PkgConfig = { defaultRegistry: 'land', missingPackageFallback: 'npm', store: 'global' };

export function isConfigKey(key: string): key is ConfigKey {
  return Object.hasOwn(CONFIG_KEYS, key);
}

export function isConfigValue(key: ConfigKey, value: string): boolean {
  return (CONFIG_KEYS[key] as readonly string[]).includes(value);
}

export function loadConfig(): PkgConfig {
  const config = { ...DEFAULTS };
  let text: string;
  try {
    text = fs.readFileSync(configPath(), 'utf8');
  } catch {
    return config;
  }
  for (const line of text.split(/\r?\n/)) {
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    const value = line.slice(eq + 1).trim();
    if (isConfigKey(key) && isConfigValue(key, value)) (config as Record<string, string>)[FIELDS[key]] = value;
  }
  return config;
}

export function saveConfig(config: PkgConfig): void {
  const file = configPath();
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const lines = (Object.keys(FIELDS) as ConfigKey[]).map(key => `${key}=${config[FIELDS[key]]}`);
  fs.writeFileSync(file, `${lines.join('\n')}\n`);
}

export function getConfigValue(config: PkgConfig, key: ConfigKey): string {
  return config[FIELDS[key]];
}

export function setConfigValue(config: PkgConfig, key: ConfigKey, value: string): PkgConfig {
  return { ...config, [FIELDS[key]]: value };
}
