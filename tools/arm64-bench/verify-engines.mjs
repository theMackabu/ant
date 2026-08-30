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

for (const configured of config.engines) {
  if (externalOnly && configured.id === 'ant') continue;
  const engine = resolveEngine(configured, { repo, engineRoot });
  if (!fs.existsSync(engine.executable)) {
    console.error(`${engine.id}: missing ${engine.executable}`);
    failed = true;
    continue;
  }

  const metadata = await executableMetadata(engine);
  const resolution = state[engine.id];
  if (engine.id !== 'ant' && !resolution) {
    console.error(`${engine.id}: missing resolution in ${statePath}`);
    failed = true;
    continue;
  }
  if (resolution && resolution.executableSha256 !== metadata.executableSha256) {
    console.error(
      `${engine.id}: executable hash changed: expected ${resolution.executableSha256}, found ${metadata.executableSha256}`,
    );
    failed = true;
    continue;
  }

  const identity = resolution
    ? `${resolution.channel}:${resolution.resolvedVersion}${resolution.sourceRevision ? `@${resolution.sourceRevision}` : ''}`
    : metadata.version;
  console.log(`${engine.id}: ${identity} reported=${metadata.version} sha256=${metadata.executableSha256}`);
}

if (failed) process.exitCode = 1;
