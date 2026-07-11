import * as os from 'node:os';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { spawn } from 'node:child_process';
import { styleText } from './utils';
import { consoleUrl } from './config';

interface Cfg {
  token?: string;
  email?: string;
  console?: string;
}

interface StartResp {
  code: string;
  verifyUrl: string;
  interval: number;
  expiresIn: number;
}
interface PollResp {
  status: 'pending' | 'done' | 'expired';
  token?: string;
  email?: string;
}

const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));

function cfgPath(): string {
  return path.join(os.homedir(), '.colony', 'config.json');
}

export function readConfig(): Cfg {
  try {
    const config = JSON.parse(fs.readFileSync(cfgPath(), 'utf-8')) as unknown;
    if (typeof config !== 'object' || config === null || Array.isArray(config)) throw new Error('expected a JSON object');
    for (const key of ['token', 'email', 'console'] as const) {
      if (key in config && typeof (config as Record<string, unknown>)[key] !== 'string') throw new Error(`\`${key}\` must be a string`);
    }
    return config as Cfg;
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return {};
    throw new Error(`could not read ${cfgPath()}: ${error instanceof Error ? error.message : String(error)}`);
  }
}

function writeConfig(c: Cfg): void {
  const dir = path.dirname(cfgPath());
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(cfgPath(), JSON.stringify(c, null, 2) + '\n', { mode: 0o600 });
}

export function readToken(): string | null {
  return process.env.COLONY_TOKEN ?? readConfig().token ?? null;
}

export function requireToken(): string {
  const t = readToken();
  if (!t) throw new Error('not logged in. Run `colony login` first.');
  return t;
}

async function postJson<T>(url: string, body?: unknown): Promise<T> {
  const res = await fetch(url, {
    method: 'POST',
    headers: body !== undefined ? { 'content-type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined
  });
  const data = (await res.json().catch(() => null)) as (T & { error?: string; message?: string }) | null;
  if (!res.ok) throw new Error(data?.message || data?.error || `Received ${res.status} from ${url}`);
  if (data === null) throw new Error(`Received an invalid response from ${url}`);
  return data;
}

function openBrowser(url: string): void {
  const [cmd, args] =
    process.platform === 'darwin' ? ['open', [url]] : process.platform === 'win32' ? ['cmd', ['/c', 'start', '', url]] : ['xdg-open', [url]];
  const child = spawn(cmd, args as string[], { stdio: 'ignore', detached: true });
  child.once('error', () => {});
  child.unref();
}

export async function login(): Promise<void> {
  const base = consoleUrl();
  const start = await postJson<StartResp>(`${base}/api/cli/start`);
  console.log('To authorize this device, visit:');
  console.log(`  ${styleText('cyan', start.verifyUrl)}`);
  console.log();
  console.log(styleText('dim', 'Opening your browser… waiting for approval.'));
  openBrowser(start.verifyUrl);

  const deadline = Date.now() + start.expiresIn * 1000;
  while (Date.now() < deadline) {
    await sleep(start.interval * 1000);
    const poll = await postJson<PollResp>(`${base}/api/cli/poll`, { code: start.code });
    if (poll.status === 'done' && poll.token) {
      writeConfig({ token: poll.token, email: poll.email, console: base });
      console.log(`${styleText('green', 'Logged in')}${poll.email ? ` as ${poll.email}` : ''}. Saved to ~/.colony/config.json`);
      return;
    }
    if (poll.status === 'expired') throw new Error('Login request expired. Run `colony login` again.');
  }
  throw new Error('Login timed out.');
}

export async function logout(): Promise<void> {
  const c = readConfig();
  if (!c.token) {
    console.log('Not logged in.');
    return;
  }
  writeConfig({ console: c.console });
  console.log(`${styleText('green', 'Logged out')}. Removed the token from ~/.colony/config.json`);
}

export function whoami(): void {
  const c = readConfig();
  if (!readToken()) {
    console.log('Not logged in. Run `colony login`.');
    return;
  }
  console.log(`${styleText('green', c.email ?? 'logged in')} @ ${c.console ?? consoleUrl()}`);
}
