#!/usr/bin/env ant

import fs from 'node:fs';
import path from 'node:path';
import { stripTypes } from 'ant:syntax';

function exit(message) {
  console.log(message);
  process.exit(1);
}

const args = process.argv.slice(2);
const file = args[0];
let output = null;

if (!file) exit('usage: tsc <file> [-o <output>]');

for (let i = 1; i < args.length; i++) {
  if (args[i] === '-o' || args[i] === '--out') {
    output = args[++i];
    if (!output) exit(`option '${args[i - 1]}' needs an output file`);
  } else exit(`unknown option '${args[i]}'`);
}

try {
  const source = fs.readFileSync(file, 'utf8');
  const extension = path.extname(file);
  const sourceType = extension === '.cts' ? 'commonjs' : extension === '.mts' ? 'module' : 'auto';
  const javascript = stripTypes(source, { filename: file, sourceType });

  if (output) fs.writeFileSync(output, javascript);
  else console.log(javascript);
} catch (error) {
  exit(error.path ? `file '${error.path}' not found` : String(error));
}
