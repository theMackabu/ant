import { setProgramName } from 'upm/src/cli.ts';
import { describe } from 'upm/src/util.ts';

import { isHelp, parseArgs } from './args.ts';
import { cache } from './commands/cache.ts';
import { config } from './commands/config.ts';
import { create } from './commands/create.ts';
import { exec } from './commands/exec.ts';
import { info } from './commands/info.ts';
import { init } from './commands/init.ts';
import { install } from './commands/install.ts';
import { ls } from './commands/ls.ts';
import { trust } from './commands/trust.ts';
import { update } from './commands/update.ts';
import { why } from './commands/why.ts';
import { useAntHome } from './paths.ts';
import { openSession, upm } from './session.ts';
import type { Session } from './session.ts';
import { shareSelfContained } from './share.ts';
import { installRunBanner } from './ui/banner.ts';
import { fail, help, out, usage } from './ui/output.ts';
import { installUsage } from './ui/usage.ts';

type Command = (args: string[], session: Session) => number | Promise<number>;

const COMMANDS: Record<string, Command> = {
  install: (args, session) => install('install', args, session),
  add: (args, session) => install('add', args, session),
  remove: (args, session) => install('remove', args, session),
  update,
  exec,
  run: (args, session) => upm(['run', ...args], session),
  trust,
  why,
  info,
  ls,
  cache,
  config,
  init,
  create,
};

const UPM_HELP = new Set(['install', 'add', 'remove', 'run', 'exec']);
const PASSTHROUGH = new Set(['run', 'exec', 'create']);
const FORCEABLE = new Set(['install', 'add', 'remove', 'update']);

function dirOption(command: string, args: string[]): string | undefined {
  const first = args.findIndex(arg => !arg.startsWith('-'));
  const own = PASSTHROUGH.has(command) && first >= 0 ? args.slice(0, first) : args;
  try {
    return parseArgs(own).dir;
  } catch {
    return undefined;
  }
}

async function dispatch(argv: string[]): Promise<number> {
  const leading: string[] = [];
  while (argv[0]?.startsWith('-')) leading.push(argv.shift()!);
  const [command = '', ...rest] = argv;
  const args = leading.includes('--force') && FORCEABLE.has(command) ? [...rest, '--force'] : rest;

  process.env.npm_config_user_agent = `ant/${process.versions.ant}`;
  setProgramName('ant', 'magenta');
  useAntHome();
  installUsage();
  shareSelfContained();
  installRunBanner();

  if (!UPM_HELP.has(command) && isHelp(args)) {
    out(`${help('stdout')}\n`);
    return 0;
  }

  const run = Object.hasOwn(COMMANDS, command) ? COMMANDS[command] : undefined;
  if (!run) return usage(`unknown command "${command}"`);
  const session = await openSession(dirOption(command, args), leading.includes('--verbose'));
  return await run(args, session);
}

export async function main(argv: string[]): Promise<number> {
  try {
    return await dispatch([...argv]);
  } catch (error) {
    fail(describe(error));
    return 1;
  }
}
