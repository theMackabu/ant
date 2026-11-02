#!/usr/bin/env ant

import { $ } from 'ant:shell';

const root = process.argv[2] || 'src';
const started = Date.now();

const report = await $`
  declare -A lines files
  while read -r count path; do
    [[ $path == total ]] && continue
    ext=\${path##*.}
    (( lines[$ext] += count, files[$ext] += 1 ))
  done < <(find ${root} -type f \( -name '*.c' -o -name '*.h' -o -name '*.js' -o -name '*.mjs' -o -name '*.ts' \) \
             -not -path '*/node_modules/*' -print0 | xargs -0 wc -l)

  for ext in "\${!lines[@]}"; do
    printf '%s %d %d\n' "$ext" "\${lines[$ext]}" "\${files[$ext]}"
  done | sort -k2 -nr | while read -r ext count n; do
    printf '  %-5s %8d lines in %5d files\n' ".$ext" "$count" "$n"
  done
`.text();

console.log(`line counts under ${root}/`);
process.stdout.write(report);
console.log(`(${Date.now() - started}ms)`);
