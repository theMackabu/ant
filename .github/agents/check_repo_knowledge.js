#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { repoRoot } from './util_repo_root.js';

const docs = new Map([
  [
    'AGENTS.md',
    {
      requiredLinks: new Set(['ARCHITECTURE.md', 'docs/repo/index.md', 'docs/repo/testing.md', 'docs/exec-plans/index.md']),
      maxLines: 90
    }
  ],
  [
    'ARCHITECTURE.md',
    {
      requiredLinks: new Set(['docs/repo/testing.md', 'docs/exec-plans/index.md', 'meson.build'])
    }
  ],
  [
    'docs/repo/index.md',
    {
      requiredLinks: new Set(['AGENTS.md', 'ARCHITECTURE.md', 'BUILDING.md', 'CONTRIBUTING.md', 'docs/repo/testing.md', 'docs/exec-plans/index.md'])
    }
  ],
  [
    'docs/repo/testing.md',
    {
      requiredLinks: new Set(['docs/exec-plans/index.md'])
    }
  ],
  [
    'docs/exec-plans/index.md',
    {
      requiredLinks: new Set(['docs/exec-plans/active/README.md', 'docs/exec-plans/completed/README.md', 'docs/exec-plans/tech-debt.md'])
    }
  ],
  [
    'docs/exec-plans/active/README.md',
    {
      requiredLinks: new Set()
    }
  ],
  [
    'docs/exec-plans/completed/README.md',
    {
      requiredLinks: new Set()
    }
  ],
  [
    'docs/exec-plans/tech-debt.md',
    {
      requiredLinks: new Set()
    }
  ]
]);

const markdownLinkPattern = /\[[^\]]+\]\(([^)]+)\)/g;
const requiredMetadata = ['Status:', 'Last reviewed:', 'Owner:'];

function toRepoRelative(targetPath) {
  return path.relative(repoRoot, targetPath).split(path.sep).join('/');
}

function resolveLink(docPath, rawLink) {
  if (!rawLink || rawLink.startsWith('#')) {
    return null;
  }

  const [target] = rawLink.split('#', 1);
  if (target.includes('://') || target.startsWith('mailto:') || target.startsWith('data:')) {
    return null;
  }

  return path.resolve(path.dirname(docPath), target);
}

function checkDoc(docRelPath, config) {
  const errors = [];
  const docPath = path.join(repoRoot, docRelPath);
  if (!fs.existsSync(docPath)) return [`missing required doc: ${docRelPath}`];

  const content = fs.readFileSync(docPath, 'utf8');
  const lines = content.split(/\r?\n/);
  const head = lines.slice(0, 8).join('\n');

  for (const key of requiredMetadata) {
    if (!head.includes(key)) errors.push(`${docRelPath}: missing metadata field '${key}'`);
  }

  if (config.maxLines !== undefined && lines.length > config.maxLines) {
    errors.push(`${docRelPath}: exceeds ${config.maxLines} lines; keep the entrypoint concise`);
  }

  const discoveredLinks = new Set();
  for (const match of content.matchAll(markdownLinkPattern)) {
    const rawLink = match[1].trim();
    const resolved = resolveLink(docPath, rawLink);
    if (resolved === null) continue;

    if (!fs.existsSync(resolved)) {
      errors.push(`${docRelPath}: broken link '${rawLink}'`);
      continue;
    }

    discoveredLinks.add(toRepoRelative(resolved));
  }

  const missingLinks = [...config.requiredLinks].sort().filter(link => !discoveredLinks.has(link));
  for (const missing of missingLinks) {
    errors.push(`${docRelPath}: missing required cross-link to '${missing}'`);
  }

  return errors;
}

function main() {
  const allErrors = [];

  for (const [docRelPath, config] of docs.entries()) {
    allErrors.push(...checkDoc(docRelPath, config));
  }

  if (allErrors.length > 0) {
    console.error('repo knowledge check failed:');
    for (const error of allErrors) {
      console.error(`  - ${error}`);
    }
    process.exitCode = 1;
    return;
  }

  console.log('repo knowledge check passed');
}

main();
