#!/usr/bin/env ant

import fs from 'node:fs';
import path from 'node:path';

const file = process.argv[2];
if (!file) exit('usage: kat <file>');

try {
  const content = fs.readFileSync(file, 'utf8');
  if (/\.(c|m)?(j|t)s$/.test(path.extname(file))) {
    console.log(Ant.highlight(content));
  } else console.log(content);
} catch (err) {
  console.log(`file '${err.path}' not found`);
  process.exit(1);
}
