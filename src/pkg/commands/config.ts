import { describe } from 'upm/src/util.ts';

import { configField, configValues, loadConfig, saveConfig } from '../settings.ts';
import { fail, out, summary, usage } from '../ui/output.ts';

export function config(args: string[]): number {
  const [op, key = '', value = ''] = args;
  const get = op === 'get' && args.length === 2;
  const set = op === 'set' && args.length === 3;
  if (!get && !set) return usage('config takes get <key> or set <key> <value>');

  const field = configField(key);
  if (!field) return usage(`unknown config key "${key}"`);
  if (get) {
    out(`${loadConfig()[field]}\n`);
    return 0;
  }

  if (!configValues(field).includes(value)) return usage(`${key} takes ${configValues(field).join(' or ')}`);
  try {
    saveConfig({ ...loadConfig(), [field]: value });
  } catch (error) {
    return fail(`could not write the config: ${describe(error)}`);
  }
  summary('saved', [`${key} = ${value}`]);
  return 0;
}
