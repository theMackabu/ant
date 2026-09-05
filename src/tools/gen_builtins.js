import * as esbuild from 'esbuild';
import path from 'node:path';

import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

function toSpecifier(rootDir, filePath) {
  const relativePath = path.relative(rootDir, filePath).replaceAll('\\', '/');
  const withoutExt = relativePath.replace(/\.(cts|mts|ts|cjs|mjs|js)$/u, '');

  if (withoutExt.startsWith('node/')) {
    return `node:${withoutExt.slice('node/'.length)}`;
  }

  if (withoutExt.startsWith('ant/')) {
    return `ant:${withoutExt.slice('ant/'.length)}`;
  }

  throw new Error(`Unsupported builtin module path: ${relativePath}`);
}

function toAliasSpecifiers(specifier) {
  if (specifier === 'node:path' || specifier === 'node:path/posix' || specifier === 'node:path/win32') {
    const name = specifier.slice('node:'.length);
    return [name, `ant:${name}`];
  }
  if (specifier.startsWith('node:')) return [specifier.slice('node:'.length)];
  return [];
}

function toFormat(filePath) {
  if (/\.(cts|cjs)$/u.test(filePath)) return 'MODULE_EVAL_FORMAT_CJS';
  if (/\.(mts|mjs)$/u.test(filePath)) return 'MODULE_EVAL_FORMAT_ESM';
  return 'MODULE_EVAL_FORMAT_UNKNOWN';
}

function toDefineReplacements(rawDefinitions) {
  return rawDefinitions.reduce((replacements, definition) => {
    const pivot = definition.indexOf('=');
    if (pivot === -1) {
      throw new Error(`Invalid bootstrap definition: ${definition}`);
    }

    const key = definition.slice(0, pivot);
    let value = definition.slice(pivot + 1);

    if (value === 'true') value = true;
    else if (value === 'false') value = false;

    replacements[`import.meta.env.${key}`] = JSON.stringify(value);
    return replacements;
  }, {});
}

function isBootstrapSource(rootDir, filePath) {
  const relativePath = path.relative(rootDir, filePath).replaceAll('\\', '/');
  return relativePath.startsWith('bootstrap/');
}

async function bundleBootstrap(entryPath, replacements) {
  const output = await esbuild.build({
    entryPoints: [entryPath],
    bundle: true,
    write: false,
    minify: true,
    format: 'esm',
    define: replacements
  });

  if (output.outputFiles.length !== 1) {
    throw new Error(`Expected exactly one bundled output for ${entryPath}`);
  }

  return output.outputFiles[0].contents;
}

const builtinExternals = {
  name: 'builtin-externals',
  setup(build) {
    build.onResolve({ filter: /^(node:|ant:)/ }, args => ({
      path: args.path,
      external: true
    }));
  }
};

async function bundleBuiltin(entryPath, format) {
  const output = await esbuild.build({
    entryPoints: [entryPath],
    bundle: true,
    write: false,
    minify: true,
    legalComments: 'eof',
    nodePaths: [fileURLToPath(new URL('./node_modules/', import.meta.url))],
    platform: 'neutral',
    format: format === 'MODULE_EVAL_FORMAT_ESM' ? 'esm' : 'cjs',
    target: ['es2020'],
    plugins: [builtinExternals]
  });

  if (output.outputFiles.length !== 1) {
    throw new Error(`Expected exactly one bundled output for ${entryPath}`);
  }

  return output.outputFiles[0].contents;
}

function generateBuiltinHeader(rootDir, bundles) {
  const lines = [];

  lines.push('/* Auto-generated builtin bundle data. DO NOT EDIT. */');
  lines.push('');
  lines.push('#ifndef ANT_BUILTIN_BUNDLE_DATA_H');
  lines.push('#define ANT_BUILTIN_BUNDLE_DATA_H');
  lines.push('');
  lines.push('#include <stddef.h>');
  lines.push('#include <stdint.h>');
  lines.push('');

  bundles.forEach((bundle, index) => {
    const byteLines = [];
    for (let i = 0; i < bundle.bytes.length; i += 16) {
      byteLines.push('  ' + Array.from(bundle.bytes.slice(i, i + 16)).join(', '));
    }

    lines.push(`/* ${bundle.specifier} <- ${path.relative(rootDir, bundle.entryPath).replaceAll('\\', '/')} */`);
    lines.push(`static const uint8_t ant_builtin_bundle_${index}[] = {`);
    lines.push(byteLines.join(',\n'));
    lines.push('};');
    lines.push('');
  });

  lines.push('static const ant_builtin_bundle_module_t ant_builtin_bundle_modules[] = {');
  bundles.forEach((bundle, index) => {
    lines.push(`  { ant_builtin_bundle_${index}, sizeof(ant_builtin_bundle_${index}), ${bundle.format} },`);
  });
  lines.push('};');
  lines.push('');
  lines.push('static const ant_builtin_bundle_alias_t ant_builtin_bundle_aliases[] = {');
  bundles.forEach((bundle, index) => {
    for (const specifier of bundle.specifiers) {
      lines.push(`  { ${JSON.stringify(specifier)}, ${specifier.length}, ${JSON.stringify(bundle.specifier)}, ${index} },`);
    }
  });
  lines.push('};');
  lines.push('');
  lines.push('static const size_t ant_builtin_bundle_module_count =');
  lines.push('  sizeof(ant_builtin_bundle_modules) / sizeof(ant_builtin_bundle_modules[0]);');
  lines.push('');
  lines.push('static const size_t ant_builtin_bundle_alias_count =');
  lines.push('  sizeof(ant_builtin_bundle_aliases) / sizeof(ant_builtin_bundle_aliases[0]);');
  lines.push('');
  lines.push('#endif');

  return lines.join('\n') + '\n';
}

function generateSnapshotHeader(inputFile, bytes) {
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
  const separator = args.indexOf('--');
  if (args.length < 6 || separator < 4) {
    console.error(
      `Usage: ${process.argv[1]} <builtins-root> <builtin-output.h> <snapshot-output.h> ` + '<bootstrap-entry> [KEY=value...] -- <entry...>'
    );
    process.exit(1);
  }

  const [builtinsRoot, builtinOutputFile, snapshotOutputFile, bootstrapEntry] = args;
  const rawDefinitions = args.slice(4, separator);
  const entryFiles = args.slice(separator + 1);
  const replacements = toDefineReplacements(rawDefinitions);
  const bundles = [];

  for (const entryPath of entryFiles) {
    if (isBootstrapSource(builtinsRoot, entryPath)) continue;

    const specifier = toSpecifier(builtinsRoot, entryPath);
    const format = toFormat(entryPath);
    const bytes = await bundleBuiltin(entryPath, format);
    const specifiers = [specifier, ...toAliasSpecifiers(specifier)];
    bundles.push({ entryPath, specifier, specifiers, format, bytes });
  }

  const snapshotBytes = await bundleBootstrap(bootstrapEntry, replacements);
  const builtinHeader = generateBuiltinHeader(builtinsRoot, bundles);
  const snapshotHeader = generateSnapshotHeader(bootstrapEntry, snapshotBytes);
  const totalBundledBytes = bundles.reduce((sum, bundle) => sum + bundle.bytes.length, 0);

  writeFileSync(builtinOutputFile, builtinHeader);
  writeFileSync(snapshotOutputFile, snapshotHeader);

  console.log(`builtins generated successfully:`);
  console.log(`  builtin bundle: ${builtinOutputFile}`);
  console.log(`  builtin modules: ${bundles.length}`);
  console.log(`  builtin size: ${totalBundledBytes} bytes`);
  console.log(`  bootstrap snapshot: ${snapshotOutputFile}`);
  console.log(`  bootstrap size: ${snapshotBytes.length} bytes`);
  console.log(`  bootstrap replacements: ${Object.keys(replacements).length}`);
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
