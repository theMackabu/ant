import * as os from 'node:os';
import * as path from 'node:path';
import * as fs from 'node:fs';
import { spawn } from 'node:child_process';
import { styleText } from './utils';
import { REGISTRY_URL } from './api';

const SITE_URL = process.env.ANTS_URL ?? 'https://ants.land';
const REGISTRY_HOST = new URL(REGISTRY_URL).host;

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

async function postJson<T>(url: string, body?: unknown): Promise<T> {
  const res = await fetch(url, {
    method: 'POST',
    headers: body !== undefined ? { 'content-type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined
  });
  if (!res.ok) throw new Error(`Received ${res.status} from ${url}`);
  return res.json() as Promise<T>;
}

function openBrowser(url: string) {
  const cmd = process.platform === 'darwin' ? 'open' : process.platform === 'win32' ? 'start' : 'xdg-open';
  try {
    spawn(cmd, [url], { stdio: 'ignore', detached: true, shell: process.platform === 'win32' }).unref();
  } catch {}
}

export async function login() {
  const start = await postJson<StartResp>(`${SITE_URL}/api/cli/start`);
  console.log('To authorize this device, visit:');
  console.log(`  ${styleText('cyan', start.verifyUrl)}`);
  console.log();
  console.log(styleText('dim', 'Opening your browser… waiting for approval.'));
  openBrowser(start.verifyUrl);

  const deadline = Date.now() + start.expiresIn * 1000;
  while (Date.now() < deadline) {
    await sleep(start.interval * 1000);
    const poll = await postJson<PollResp>(`${SITE_URL}/api/cli/poll`, { code: start.code });
    if (poll.status === 'done' && poll.token) {
      await saveToken(poll.token);
      console.log(`${styleText('green', 'Logged in')}${poll.email ? ` as ${poll.email}` : ''}. Token saved to ~/.npmrc`);
      return;
    }
    if (poll.status === 'expired') throw new Error('Login request expired. Run antland login again.');
  }
  throw new Error('Login timed out.');
}

export async function logout() {
  const npmrc = path.join(os.homedir(), '.npmrc');
  const prefix = `//${REGISTRY_HOST}/:_authToken=`;
  let content: string;
  try {
    content = await fs.promises.readFile(npmrc, 'utf-8');
  } catch {
    console.log('Not logged in.');
    return;
  }
  const lines = content.split('\n');
  const next = lines.filter(l => !l.startsWith(prefix));
  if (next.length === lines.length) {
    console.log('Not logged in.');
    return;
  }
  await fs.promises.writeFile(npmrc, next.join('\n'));
  console.log(`${styleText('green', 'Logged out')}. Removed the ants.land token from ~/.npmrc`);
}

async function saveToken(token: string) {
  const npmrc = path.join(os.homedir(), '.npmrc');
  const prefix = `//${REGISTRY_HOST}/:_authToken=`;
  let content = '';
  try {
    content = await fs.promises.readFile(npmrc, 'utf-8');
  } catch {}
  const kept = content.split('\n').filter(l => l.trim() !== '' && !l.startsWith(prefix));
  kept.push(prefix + token);
  await fs.promises.writeFile(npmrc, kept.join('\n') + '\n');
}
