import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

import { nodeGypShim, nodeShim } from '../tools.ts';
import { fail, write } from '../ui/output.ts';
import { MARKER, markerText } from './discover.ts';
import type { Pending } from './discover.ts';

const OUTPUT_LIMIT = 1 << 20;

interface Run {
  code: number;
  output: string;
}

function shell(): [string, string[]] {
  if (process.platform === 'win32') return [process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c']];
  return [process.env.ANT_SCRIPT_SHELL || '/bin/sh', ['-c']];
}

function unshare(dir: string): void {
  const stack = [dir];
  while (stack.length > 0) {
    const current = stack.pop()!;
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const file = path.join(current, entry.name);
      if (entry.isDirectory()) stack.push(file);
      if (!entry.isFile()) continue;
      const info = fs.statSync(file);
      if (info.nlink < 2) continue;
      const temp = `${file}.ant-unshare`;
      fs.copyFileSync(file, temp);
      fs.chmodSync(temp, info.mode | 0o200);
      fs.renameSync(temp, file);
    }
  }
}

function runScript(pkg: Pending, root: string, script: [string, string], tools: string[]): Promise<Run> {
  const [command, args] = shell();
  const bins = [path.join(pkg.modules, '.bin'), path.join(root, 'node_modules', '.bin'), ...tools];
  const env = {
    ...process.env,
    PATH: [...bins, process.env.PATH ?? ''].join(path.delimiter),
    npm_lifecycle_event: script[0],
    npm_lifecycle_script: script[1],
    npm_package_name: pkg.name,
    npm_package_version: pkg.version,
  };

  return new Promise(resolve => {
    let output = '';
    const keep = (chunk: Buffer, stream: string) => {
      output += `[${stream}] ${chunk.toString()}`;
      if (output.length > OUTPUT_LIMIT) output = output.slice(-OUTPUT_LIMIT);
    };
    const child = spawn(command, [...args, script[1]], { cwd: pkg.dir, env, stdio: ['ignore', 'pipe', 'pipe'] });
    child.stdout?.on('data', chunk => keep(chunk, 'stdout'));
    child.stderr?.on('data', chunk => keep(chunk, 'stderr'));
    child.on('error', error => resolve({ code: 1, output: `${output}${error.message}\n` }));
    child.on('close', (code, signal) => resolve({ code: code ?? (signal ? 128 : 1), output }));
  });
}

export async function runLifecycle(root: string, pending: Pending[], verbose: boolean): Promise<{ built: string[]; failed: number }> {
  const tools = pending.length > 0 ? [nodeGypShim(), nodeShim()] : [];
  const built: string[] = [];
  let failed = 0;

  for (const pkg of pending) {
    unshare(pkg.dir);
    let ok = true;
    for (const script of pkg.scripts) {
      const { code, output } = await runScript(pkg, root, script, tools);
      if (verbose || code !== 0) write('stderr', output);
      if (code === 0) continue;
      fail(`${pkg.name}: ${script[0]} script exited with code ${code}`);
      ok = false;
      failed++;
      break;
    }
    if (!ok) continue;
    try {
      fs.writeFileSync(path.join(pkg.dir, MARKER), markerText(pkg.scripts));
    } catch {}
    built.push(pkg.name);
  }

  return { built, failed };
}
