import { isHelp } from './args.ts';
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
import { openSession, upm } from './session.ts';
import type { Session } from './session.ts';
import { help, out } from './ui/output.ts';
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

export async function main(argv: string[]): Promise<number> {
  process.env.npm_config_user_agent = `ant/${process.versions.ant}`;
  installUsage();

  const [command = '', ...args] = argv;
  const session = openSession(args);

  if (!UPM_HELP.has(command) && isHelp(args)) {
    out(`${help('stdout')}\n`);
    return 0;
  }

  const run = Object.hasOwn(COMMANDS, command) ? COMMANDS[command] : undefined;
  return run ? await run(args, session) : await upm(argv, session);
}
