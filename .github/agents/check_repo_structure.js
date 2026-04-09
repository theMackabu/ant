#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import childProcess from 'node:child_process';
import { repoRoot } from './util_repo_root.js';

function parseArgs(argv) {
  const result = {
    files: [],
    filesFrom: null
  };

  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg === '--files-from') {
      result.filesFrom = argv[index + 1] || null;
      index += 1;
      continue;
    }

    result.files.push(arg);
  }

  return result;
}

function readChangedFilesFromGit() {
  const commands = [
    ['git', ['diff', '--name-only', '--cached']],
    ['git', ['diff', '--name-only']],
    ['git', ['ls-files', '--others', '--exclude-standard']]
  ];

  const files = new Set();
  for (const [command, args] of commands) {
    const output = childProcess.execFileSync(command, args, {
      cwd: repoRoot,
      encoding: 'utf8'
    });

    for (const line of output.split(/\r?\n/)) {
      const trimmed = line.trim();
      if (!trimmed) {
        continue;
      }
      files.add(trimmed);
    }
  }

  return [...files].sort();
}

function loadFiles(options) {
  if (options.filesFrom) {
    const contents = fs.readFileSync(path.resolve(repoRoot, options.filesFrom), 'utf8');
    return contents
      .split(/\r?\n/)
      .map(line => line.trim())
      .filter(Boolean);
  }

  if (options.files.length > 0) {
    return options.files;
  }

  return readChangedFilesFromGit();
}

function normalize(filePath) {
  return filePath.split(path.sep).join('/');
}

function main() {
  const options = parseArgs(process.argv.slice(2));
  const changedFiles = loadFiles(options).map(normalize);
  const errors = [];
  const notes = [];

  for (const filePath of changedFiles) {
    if (filePath.startsWith('build/')) {
      errors.push(`${filePath}: do not commit build output; keep build artifacts local`);
    }

    if (filePath.startsWith('docs/output/') || filePath.startsWith('docs/symbols/')) {
      errors.push(`${filePath}: generated docs artifacts do not belong in normal source changes`);
    }

    if (/^todo\/.+\.md$/u.test(filePath)) {
      errors.push(`${filePath}: durable markdown should live under docs/repo/ or docs/exec-plans/, not todo/`);
    }

    if (filePath.startsWith('vendor/')) {
      notes.push(`${filePath}: vendored dependency change detected; keep it isolated and document the reason in an execution plan`);
    }
  }

  if (errors.length > 0) {
    console.error('repo structure check failed:');
    for (const error of errors) {
      console.error(`  - ${error}`);
    }
    process.exitCode = 1;
    return;
  }

  console.log(changedFiles.length === 0 ? 'repo structure check passed (no changed files)' : 'repo structure check passed');

  if (notes.length > 0) {
    console.log('');
    console.log('notes:');
    for (const note of notes) console.log(`  - ${note}`);
  }
}

main();
