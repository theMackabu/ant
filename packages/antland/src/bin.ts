#!/usr/bin/env node

import * as fs from 'node:fs';
import * as path from 'node:path';
import { parseArgs } from 'node:util';
import { execSnippet, execPackage, install, publish, remove, runScript, showPackageInfo, uploadSnippetFile } from './commands';
import { login, logout } from './login';
import { AntPackage, ExecError, prettyTime, setDebug, styleText } from './utils';
import type { PkgManagerName } from './pkg_manager';

const args = process.argv.slice(2);

function row(rows: [string, string][]): string {
  const max = Math.max(...rows.map(r => r[0].length));
  return rows.map(r => `  ${styleText('green', r[0].padEnd(max))}  ${r[1]}`).join('\n');
}

function printHelp() {
  console.log(`ants.land cli

Usage:
${row([
  ['antland add @you/thing', 'Install "@you/thing" from ants.land.'],
  ['antland remove @you/thing', 'Remove "@you/thing" from the project.'],
  ['antland publish', 'Publish the current package to ants.land.']
])}

Commands:
${row([
  ['<script>', 'Run a script from package.json'],
  ['run <script>', 'Run a script from package.json'],
  ['i, install, add', 'Install one or more ants.land packages'],
  ['r, uninstall, remove', 'Remove one or more packages'],
  ['publish', 'Publish the current package to ants.land'],
  ['npx, exec, x', 'Run a package binary (or snippet:<id>) after a safety check'],
  ['snippet <file>', 'Upload a single file (--private optional); run with npx snippet:<id>'],
  ['login', 'Authorize this device (saves a publish token).'],
  ['logout', 'Remove the saved ants.land token.'],
  ['info, show, view', 'Show package information']
])}

Options:
${row([
  ['-P, --save-prod', 'Add to dependencies (default).'],
  ['-D, --save-dev', 'Add to devDependencies.'],
  ['-O, --save-optional', 'Add to optionalDependencies.'],
  ['-y, --yes', 'Skip the npx safety prompt.'],
  ['--npm', 'Use npm.'],
  ['--yarn', 'Use yarn.'],
  ['--pnpm', 'Use pnpm.'],
  ['--bun', 'Use bun.'],
  ['--debug', 'Show additional debugging information.'],
  ['-h, --help', 'Show this help text.'],
  ['-v, --version', 'Print the version number.']
])}

Environment:
${row([['ANTS_REGISTRY', 'Override the registry URL (default https://npm.ants.land).']])}
`);
}

function getPackages(positionals: string[], allowEmpty: boolean): AntPackage[] {
  const pkgArgs = positionals.slice(1);
  if (!allowEmpty && pkgArgs.length === 0) {
    console.error(styleText('red', 'Missing package argument.'));
    console.log();
    printHelp();
    process.exit(1);
  }
  return pkgArgs.map(p => AntPackage.from(p));
}

async function run(fn: () => Promise<void>) {
  const start = Date.now();
  try {
    await fn();
    console.log();
    console.log(`${styleText('green', 'Completed')} in ${prettyTime(Date.now() - start)}`);
  } catch (err) {
    if (err instanceof ExecError) process.exit(err.code);
    console.error(styleText('red', err instanceof Error ? err.message : String(err)));
    process.exit(1);
  }
}

function unknown(cmd: string): never {
  console.error(styleText('red', `Unknown command: ${cmd}`));
  console.log();
  printHelp();
  process.exit(1);
}

const isExecCmd = args.length > 0 && (args[0] === 'npx' || args[0] === 'exec' || args[0] === 'x');

if (!isExecCmd && (args.length === 0 || args.some(a => a === '-h' || a === '--help'))) {
  printHelp();
  process.exit(0);
} else if (!isExecCmd && args.some(a => a === '-v' || a === '--version')) {
  const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'package.json'), 'utf-8')) as { version: string };
  console.log(pkg.version);
  process.exit(0);
} else {
  const cmd = args[0];

  if (cmd === 'publish') {
    run(() => publish({ pkgManagerName: pkgManagerFrom(args), publishArgs: args.slice(1) }));
  } else if (cmd === 'npx' || cmd === 'exec' || cmd === 'x') {
    const rest = args.slice(1);
    const ours: string[] = [];
    let yes = false;
    let i = 0;
    for (; i < rest.length; i++) {
      const a = rest[i];
      if (a === '-y' || a === '--yes') yes = true;
      else if (a === '--debug') setDebug(true);
      else if (a === '--npm' || a === '--yarn' || a === '--pnpm' || a === '--bun') ours.push(a);
      else break;
    }
    const target = rest[i];
    if (!target) {
      console.error(styleText('red', 'Missing package to run. Usage: antland npx <pkg>[@version] [args...] (or snippet:<id>)'));
      process.exit(1);
    }
    const binArgs = rest.slice(i + 1);
    if (target.startsWith('snippet:')) run(() => execSnippet(target.slice('snippet:'.length), { yes, binArgs }));
    else run(() => execPackage(target, { yes, binArgs, pkgManagerName: pkgManagerFrom(ours) }));
  } else if (cmd === 'snippet') {
    const rest = args.slice(1);
    const isPrivate = rest.includes('--private');
    const file = rest.find(a => !a.startsWith('-'));
    if (!file) {
      console.error(styleText('red', 'Missing file. Usage: antland snippet <file> [--private]'));
      process.exit(1);
    }
    run(() => uploadSnippetFile(file, { private: isPrivate }));
  } else if (cmd === 'login') {
    run(() => login());
  } else if (cmd === 'logout') {
    run(() => logout());
  } else if (cmd === 'view' || cmd === 'show' || cmd === 'info') {
    const name = args[1];
    if (!name) {
      console.error(styleText('red', 'Missing package name.'));
      printHelp();
      process.exit(1);
    }
    run(() => showPackageInfo(name));
  } else {
    const { values, positionals } = parseArgs({
      args,
      allowPositionals: true,
      options: {
        'save-prod': { type: 'boolean', default: true, short: 'P' },
        'save-dev': { type: 'boolean', default: false, short: 'D' },
        'save-optional': { type: 'boolean', default: false, short: 'O' },
        npm: { type: 'boolean', default: false },
        yarn: { type: 'boolean', default: false },
        pnpm: { type: 'boolean', default: false },
        bun: { type: 'boolean', default: false },
        debug: { type: 'boolean', default: false },
        help: { type: 'boolean', default: false, short: 'h' },
        version: { type: 'boolean', default: false, short: 'v' }
      }
    });

    if (values.debug || process.env.DEBUG) setDebug(true);
    const pkgManagerName = pmFromValues(values);

    if (positionals.length === 0) {
      printHelp();
      process.exit(0);
    }

    if (cmd === 'i' || cmd === 'install' || cmd === 'add') {
      run(() =>
        install(getPackages(positionals, true), {
          mode: values['save-dev'] ? 'dev' : values['save-optional'] ? 'optional' : 'prod',
          pkgManagerName
        })
      );
    } else if (cmd === 'r' || cmd === 'uninstall' || cmd === 'remove') {
      run(() => remove(getPackages(positionals, false), { pkgManagerName }));
    } else if (cmd === 'run') {
      const script = positionals[1];
      if (!script) {
        console.error(styleText('red', 'Missing script argument.'));
        process.exit(1);
      }
      run(() => runScript(script, { pkgManagerName }));
    } else {
      const pkgJsonPath = path.join(process.cwd(), 'package.json');
      if (fs.existsSync(pkgJsonPath)) {
        const json = JSON.parse(fs.readFileSync(pkgJsonPath, 'utf-8')) as { scripts?: Record<string, string> };
        if (json.scripts?.[cmd]) run(() => runScript(cmd, { pkgManagerName }));
        else unknown(cmd);
      } else {
        unknown(cmd);
      }
    }
  }
}

function pmFromValues(values: Record<string, unknown>): PkgManagerName | null {
  return values.pnpm ? 'pnpm' : values.yarn ? 'yarn' : values.bun ? 'bun' : values.npm ? 'npm' : null;
}
function pkgManagerFrom(argv: string[]): PkgManagerName | null {
  return argv.includes('--pnpm') ? 'pnpm' : argv.includes('--yarn') ? 'yarn' : argv.includes('--bun') ? 'bun' : argv.includes('--npm') ? 'npm' : null;
}
