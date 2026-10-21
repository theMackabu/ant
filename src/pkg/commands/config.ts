import { describe } from 'upm/src/util.ts';

import {
  CONFIG_KEYS, getConfigValue, isConfigKey, isConfigValue, loadConfig, saveConfig, setConfigValue,
} from '../settings.ts';
import { fail, out, summary, usage } from '../ui/output.ts';

export function config(args: string[]): number {
  const [op, key, value] = args;
  if (op === 'get' && args.length === 2) {
    if (!isConfigKey(key!)) return usage(`unknown config key "${key}"`);
    out(`${getConfigValue(loadConfig(), key)}\n`);
    return 0;
  }

  if (op === 'set' && args.length === 3) {
    if (!isConfigKey(key!)) return usage(`unknown config key "${key}"`);
    if (!isConfigValue(key, value!)) return usage(`${key} takes ${CONFIG_KEYS[key].join(' or ')}`);
    try {
      saveConfig(setConfigValue(loadConfig(), key, value!));
    } catch (error) {
      fail(`could not write the config: ${describe(error)}`);
      return 1;
    }
    summary('saved', [`${key} = ${value}`]);
    return 0;
  }

  return usage('config takes get <key> or set <key> <value>');
}
