import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline/promises';

import { writeJson } from '../project.ts';
import { color, fail, summary } from '../ui/output.ts';

export async function init(): Promise<number> {
  const cwd = process.cwd();
  const file = path.join(cwd, 'package.json');
  if (fs.existsSync(file)) {
    fail(`${file} already exists`);
    return 1;
  }

  let name = path.basename(cwd) || 'my-project';
  let version = '1.0.0';
  let main = 'index.js';

  if (process.stdin.isTTY) {
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
    const ask = async (prompt: string, value: string) => {
      const answer = await rl.question(`${color('cyan', prompt)} ${color('gray', `(${value})`)} `);
      return answer.trim() || value;
    };
    name = await ask('package name', name);
    version = await ask('version', version);
    main = await ask('entry point', main);
    rl.close();
  }

  writeJson(file, {
    name, version, type: 'module', main,
    scripts: { start: `ant ${main}` },
    dependencies: {},
    devDependencies: {},
  });
  summary('created', ['package.json', `${name}@${version}`]);
  return 0;
}
