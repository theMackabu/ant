import * as esbuild from 'esbuild';
import { dirname, resolve } from 'node:path';
import { readFileSync, writeFileSync } from 'node:fs';

const processedModules = new Set();

function wrapModule(content) {
  const exports = [];
  let output = '(function() {\n';

  const exportRegex = /export\s+(const|let|var|function)\s+(\w+)/g;
  let match;
  let lastIndex = 0;
  let processed = '';

  while ((match = exportRegex.exec(content)) !== null) {
    processed += content.slice(lastIndex, match.index);
    processed += `${match[1]} ${match[2]}`;
    exports.push(match[2]);
    lastIndex = match.index + match[0].length;
  }

  processed += content.slice(lastIndex);
  output += processed;

  if (exports.length > 0) {
    output += `\nreturn { ${exports.join(', ')} };\n})()`;
  } else output += '})()';

  return output;
}

function processInlines(filePath, content) {
  const dir = dirname(filePath);

  return content.replace(/snapshot_inline\s*\(\s*['"](.+?)['"]\s*\);?/g, (_, importPath) => {
    const resolved = resolve(dir, importPath);

    try {
      return readFileSync(resolved, 'utf-8');
    } catch (err) {
      throw Error(`Error: Cannot read inline file: ${resolved}`);
    }
  });
}

function processIncludes(filePath, content) {
  const dir = dirname(filePath);

  return content.replace(/snapshot_include\s*\(\s*['"](.+?)['"]\s*\)/g, (_, importPath) => {
    const resolved = resolve(dir, importPath);

    if (processedModules.has(resolved)) {
      console.error(`Warning: Circular dependency detected: ${resolved}`);
      return '';
    }

    processedModules.add(resolved);

    try {
      let moduleContent = readFileSync(resolved, 'utf-8');
      moduleContent = processIncludes(resolved, moduleContent);
      return wrapModule(moduleContent);
    } catch (err) {
      throw Error(`Error: Cannot read module: ${resolved}`);
    }
  });
}

function replaceTemplates(content, replacements) {
  for (const [key, value] of Object.entries(replacements)) {
    content = content.replaceAll(`{{${key}}}`, value);
  }
  return content;
}

function generateHeader(inputFile, data) {
  const bytes = [...Buffer.from(data)].map(b => `0x${b.toString(16).padStart(2, '0')}`);

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

static const size_t ant_snapshot_source_len = ${data.length};

/* bundled source size: ${data.length} bytes */
static const size_t ant_snapshot_mem_size = ${data.length};

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

  const [inputFile, outputFile, ...replacementArgs] = args;

  const replacements = {};
  for (const arg of replacementArgs) {
    const idx = arg.indexOf('=');
    if (idx !== -1) {
      const key = arg.slice(0, idx);
      const value = arg.slice(idx + 1);
      replacements[key] = value;
      console.log(`template replacement: {{${key}}} -> ${value}`);
    }
  }

  let content;
  try {
    content = readFileSync(inputFile, 'utf-8');
  } catch (err) {
    throw Error(`Error: Cannot open input file: ${inputFile}`);
  }

  const originalSize = content.length;

  content = processInlines(inputFile, content);
  content = processIncludes(inputFile, content);
  content = replaceTemplates(content, replacements);

  const result = await esbuild.transform(content, {
    minify: true,
    loader: 'ts'
  });

  const minified = result.code.trimEnd();
  console.log(minified);

  const header = generateHeader(inputFile, minified);
  writeFileSync(outputFile, header);

  console.log(`snapshot generated successfully: ${outputFile}`);
  console.log(`  original size: ${originalSize} bytes`);
  console.log(`  bundled size: ${minified.length} bytes`);
  console.log(`  replacements: ${Object.keys(replacements).length}`);
}

main().catch(err => {
  console.error(err);
  process.exit(1);
});
