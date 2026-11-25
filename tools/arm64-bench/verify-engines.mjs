#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { executableMetadata, resolveEngine } from './lib.mjs';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(scriptDir, '..', '..');
const engineRootIndex = process.argv.indexOf('--engine-root');
const engineRoot = path.resolve(
  engineRootIndex >= 0 ? process.argv[engineRootIndex + 1] : path.join(repo, '.cache', 'arm64-bench'),
);
const externalOnly = process.argv.includes('--external-only');
const config = JSON.parse(fs.readFileSync(path.join(scriptDir, 'engines.json'), 'utf8'));
const statePath = path.join(engineRoot, 'engine-state.json');
const state = fs.existsSync(statePath)
  ? JSON.parse(fs.readFileSync(statePath, 'utf8')).engines || {}
  : {};
let failed = false;

function report(engine, message) {
  if (engine.id === 'ant') {
    console.error(`${engine.id}: ${message}`);
    failed = true;
  } else if (process.env.GITHUB_ACTIONS) console.log(`::warning::${engine.id}: ${message}`);
  else console.error(`warning: ${engine.id}: ${message}`);
}

for (const configured of config.engines) {
  if (externalOnly && configured.id === 'ant') continue;
  const engine = resolveEngine(configured, { repo, engineRoot });
  if (!fs.existsSync(engine.executable)) {
    report(engine, `missing ${engine.executable}`);
    continue;
  }

  const metadata = await executableMetadata(engine);
  const resolution = state[engine.install];
  if (engine.id !== 'ant' && !resolution) {
    report(engine, `missing resolution in ${statePath}`);
    continue;
  }
  if (resolution && resolution.executableSha256 !== metadata.executableSha256) {
    report(
      engine,
      `executable hash changed: expected ${resolution.executableSha256}, found ${metadata.executableSha256}`,
    );
    continue;
  }

  const identity = resolution
    ? `${resolution.channel}:${resolution.resolvedVersion}${resolution.sourceRevision ? `@${resolution.sourceRevision}` : ''}`
    : metadata.version;
  console.log(`${engine.id}: ${identity} reported=${metadata.version} sha256=${metadata.executableSha256}`);
}

if (failed) process.exitCode = 1;
