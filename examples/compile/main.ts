import config from './config.json';
import antArt from './ant.txt';
import { bubble } from './lib/bubble.ts';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const colors = require('./lib/colors.cjs');

const [command = 'say', ...rest] = process.argv.slice(2);

switch (command) {
  case 'say': {
    const message = rest.join(' ') || config.defaultMessage;
    console.log(bubble(message, { width: config.bubbleWidth }));
    console.log(colors.green(antArt));
    break;
  }

  case 'sysinfo': {
    const { sysinfo } = await import('./commands/sysinfo.ts');
    console.log(colors.bold(`${config.name} sysinfo`));
    console.log(sysinfo());
    break;
  }

  case 'march': {
    const { fork } = await import('node:child_process');
    process.env.MARCH_COUNT = rest[0] ?? '5';
    const child = fork(new URL('./commands/count.ts', import.meta.url).pathname);
    child.stdout?.on('data', (chunk: Uint8Array) => process.stdout.write(chunk));
    child.on('exit', (code: number) => {
      console.log(colors.dim(`march finished with code ${code}`));
    });
    break;
  }

  default:
    console.error(`unknown command: ${command}`);
    console.error('usage: antsay [say <message>] [sysinfo] [march <n>]');
    process.exit(1);
}
