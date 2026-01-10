import * as esbuild from 'esbuild';
import { writeFileSync } from 'node:fs';

function replaceTemplates(content, replacements) {
  for (const [key, value] of Object.entries(replacements)) {
    content = content.replaceAll(`{{${key}}}`, value);
  }
  return content;
}

function generateHeader(inputFile, bytes) {
  const lines = [];
  for (let i = 0; i < bytes.length; i += 16) {
    lines.push('  ' + bytes.slice(i, i + 16).join(', '));
  }

  return `/* Auto-generated snapshot from ${inputFile} */
/* DO NOT EDIT - Generated during build */

#ifndef ANT_SNAPSHOT_DATA_H
#define ANT_SNAPSHOT_DATA_H

#include <stddef.h>
#include <stdint.h>

static const uint8_t ant_snapshot_source[] = {
${lines.join(',\n')}
};

/* bundled source size: ${bytes.length} bytes */
static const size_t ant_snapshot_source_len = ${bytes.length};

#endif /* ANT_SNAPSHOT_DATA_H */
`;
}

async function main() {
  const args = process.argv.slice(2);

  if (args.length < 2) {
    console.error(`Usage: ${process.argv[1]} <input.js> <output.h> [KEY=value...]`);
    console.error(`Example: ${process.argv[1]} core.js snapshot.h VERSION=1.0.0 GIT_HASH=abc123`);
    process.exit(1);
  }

  const [inputFile, outputFile, ...rawArgs] = args;

  const replacements = rawArgs.reduce((acc, arg) => {
    const pivot = arg.indexOf('=');
    if (pivot === -1) return acc;

    const key = arg.slice(0, pivot);
    const value = arg.slice(pivot + 1);

    acc[`import.meta.env.${key}`] = JSON.stringify(value);
    return acc;
  }, {});

  const result = await esbuild.build({
    minify: true,
    bundle: true,
    write: false,
    format: 'esm',
    define: replacements,
    entryPoints: [inputFile]
  });

  const minified = result.outputFiles[0].contents;
  console.log(new TextDecoder().decode(minified));

  const header = generateHeader(inputFile, minified);
  writeFileSync(outputFile, header);

  console.log(`snapshot generated successfully: ${outputFile}`);
  console.log(`  bundled size: ${minified.length} bytes`);
  console.log(`  replacements: ${Object.keys(replacements).length}`);
}

main().catch(err => {
  console.error(err);
  process.exit(1);
});
