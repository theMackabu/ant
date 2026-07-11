#!/usr/bin/env node

import * as fs from 'node:fs';
import * as path from 'node:path';
import { deploy, destroy, list, init } from './commands';
import { login, logout, whoami } from './auth';
import { prettyTime, styleText } from './utils';

const args = process.argv.slice(2);

function row(rows: [string, string][]): string {
  const max = Math.max(...rows.map(r => r[0].length));
  return rows.map(r => `  ${styleText('green', r[0].padEnd(max))}  ${r[1]}`).join('\n');
}

function printHelp(): void {
  console.log(`colony

Usage:
${row([
  ['colony init [name]', 'Create a colony.toml in the current directory.'],
  ['colony deploy', 'Build locally and deploy to <name>.ants.page.'],
  ['colony delete [name]', 'Delete a project (defaults to colony.toml).'],
  ['colony list', 'List your projects.']
])}

Commands:
${row([
  ['login', 'Authorize this device (saves a deploy token).'],
  ['logout', 'Remove the saved token.'],
  ['whoami', 'Show the logged-in account.'],
  ['init', 'Scaffold a colony.toml.'],
  ['deploy', 'Build + upload the current project.'],
  ['delete, rm', 'Delete a project.'],
  ['list, ls', 'List your projects.']
])}

Environment:
${row([['COLONY_TOKEN', 'Deploy token for non-interactive use (CI).']])}
`);
}

async function run(fn: () => Promise<void> | void): Promise<void> {
  const start = Date.now();
  try {
    await fn();
    console.log();
    console.log(`${styleText('green', 'Done')} in ${prettyTime(Date.now() - start)}`);
  } catch (err) {
    console.error(styleText('red', err instanceof Error ? err.message : String(err)));
    process.exitCode = 1;
  }
}

const cmd = args[0];

if (!cmd || args.some(a => a === '-h' || a === '--help')) {
  printHelp();
  process.exit(0);
} else if (args.some(a => a === '-v' || a === '--version')) {
  const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'package.json'), 'utf-8')) as { version: string };
  console.log(pkg.version);
  process.exit(0);
} else if (cmd === 'login') {
  run(() => login());
} else if (cmd === 'logout') {
  run(() => logout());
} else if (cmd === 'whoami') {
  run(() => whoami());
} else if (cmd === 'init') {
  run(() => init(args[1]));
} else if (cmd === 'deploy') {
  run(() => deploy());
} else if (cmd === 'delete' || cmd === 'rm') {
  run(() => destroy(args[1]));
} else if (cmd === 'list' || cmd === 'ls') {
  run(() => list());
} else {
  console.error(styleText('red', `Unknown command: ${cmd}`));
  console.log();
  printHelp();
  process.exit(1);
}
