#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { repoRoot } from './util_repo_root.js';
import childProcess from 'node:child_process';

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

function addRecommendation(recommendations, command, reason) {
  if (!recommendations.has(command)) recommendations.set(command, reason);
}

function isDocsOnly(files) {
  return files.every(
    filePath =>
      filePath === 'AGENTS.md' ||
      filePath === 'ARCHITECTURE.md' ||
      filePath === 'CONTRIBUTING.md' ||
      filePath === 'BUILDING.md' ||
      filePath.startsWith('docs/') ||
      filePath.endsWith('.md')
  );
}

function main() {
  const options = parseArgs(process.argv.slice(2));
  const changedFiles = loadFiles(options).map(normalize);
  const recommendations = new Map();
  const notes = [];

  if (changedFiles.length === 0) {
    console.log('No changed files detected.');
    console.log('');
    console.log('Recommended validation:');
    console.log('- maid knowledge');
    return;
  }

  if (isDocsOnly(changedFiles)) {
    addRecommendation(recommendations, 'maid knowledge', 'docs-only change set');
  }

  for (const filePath of changedFiles) {
    if (filePath === 'AGENTS.md' || filePath === 'ARCHITECTURE.md' || filePath.startsWith('docs/repo/') || filePath.startsWith('docs/exec-plans/')) {
      addRecommendation(recommendations, 'maid knowledge', 'repo knowledge docs changed');
    }

    if (
      filePath.startsWith('src/silver/') ||
      filePath.startsWith('src/gc/') ||
      filePath === 'src/runtime.c' ||
      filePath === 'src/ant.c' ||
      filePath === 'src/main.c' ||
      filePath === 'src/errors.c' ||
      filePath === 'src/descriptors.c' ||
      filePath === 'src/shapes.c'
    ) {
      addRecommendation(recommendations, 'meson compile -C build', 'runtime or engine core changed');
      addRecommendation(
        recommendations,
        './build/ant examples/spec/run.js <spec_name or --all>',
        'engine-level behavior can affect broad language semantics'
      );
    }

    if (
      filePath.startsWith('src/modules/') ||
      filePath.startsWith('src/esm/') ||
      filePath.startsWith('src/builtins/') ||
      filePath.startsWith('src/http/') ||
      filePath.startsWith('src/net/') ||
      filePath.startsWith('src/streams/')
    ) {
      addRecommendation(recommendations, 'meson compile -C build', 'runtime-facing modules or I/O code changed');
      addRecommendation(recommendations, './build/ant examples/spec/run.js <spec_name or --all>', 'shared runtime semantics may have shifted');

      const stem = path.basename(filePath, path.extname(filePath));
      if (stem) {
        notes.push(`Consider running focused tests that match '${stem}', for example: rg --files tests | rg '${stem}'`);
      }
    }

    if (
      filePath === 'meson.build' ||
      filePath.startsWith('meson/') ||
      filePath === 'maidfile.toml' ||
      filePath.startsWith('.github/workflows/') ||
      filePath.startsWith('.github/actions/') ||
      filePath.startsWith('libant/')
    ) {
      addRecommendation(recommendations, 'meson setup build --reconfigure', 'build graph or automation changed');
      addRecommendation(recommendations, 'meson compile -C build', 'build configuration changes should still produce a binary');
    }

    if (filePath.startsWith('.github/agents/')) {
      addRecommendation(recommendations, 'maid knowledge', 'tooling change touched the repo harness');
    }

    if (filePath.startsWith('tests/')) {
      addRecommendation(recommendations, 'meson compile -C build', 'tests should run against a fresh binary');
      addRecommendation(recommendations, `./build/ant ${filePath}`, 'a focused regression test changed');
    }
  }

  console.log('Changed files:');
  for (const filePath of changedFiles) {
    console.log(`- ${filePath}`);
  }

  console.log('');
  console.log('Recommended validation:');
  for (const [command, reason] of recommendations.entries()) {
    console.log(`- ${command}  # ${reason}`);
  }

  if (notes.length > 0) {
    console.log('');
    console.log('Notes:');
    for (const note of [...new Set(notes)]) console.log(`- ${note}`);
  }
}

main();
