#!/usr/bin/env ant

import fs from 'node:fs';
import path from 'node:path';

function exit(error) {
  console.log(error);
  process.exit(1);
}

const file = process.argv[2];
if (!file) exit('usage: kat <file>');

try {
  const content = fs.readFileSync(file, 'utf8');
  if (/\.(c|m)?(j|t)s$/.test(path.extname(file))) {
    console.log(Ant.highlight(content));
  } else console.log(content);
} catch (err) {
  exit(err.message);
}
