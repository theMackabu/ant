import * as esbuild from 'esbuild';
import path from 'node:path';
import { writeFileSync } from 'node:fs';

function cString(input) {
  return JSON.stringify(input);
}

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
  if (specifier.startsWith('node:')) return [specifier.slice('node:'.length)];
  return [];
}

function toFormat(filePath) {
  if (/\.(cts|cjs)$/u.test(filePath)) return 'MODULE_EVAL_FORMAT_CJS';
  if (/\.(mts|mjs)$/u.test(filePath)) return 'MODULE_EVAL_FORMAT_ESM';
  return 'MODULE_EVAL_FORMAT_UNKNOWN';
}

async function bundleBuiltin(entryPath, format) {
  const output = await esbuild.build({
    entryPoints: [entryPath],
    bundle: true,
    write: false,
    minify: true,
    platform: 'neutral',
    format: format === 'MODULE_EVAL_FORMAT_ESM' ? 'esm' : 'cjs',
    target: ['es2020'],
    plugins: [
      {
        name: 'builtin-externals',
        setup(build) {
          build.onResolve({ filter: /^(node:|ant:)/ }, args => ({
            path: args.path,
            external: true
          }));
        }
      }
    ]
  });

  if (output.outputFiles.length !== 1) {
    throw new Error(`Expected exactly one bundled output for ${entryPath}`);
  }

  return output.outputFiles[0].contents;
}

function generateHeader(rootDir, bundles) {
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
      lines.push(`  { ${cString(specifier)}, ${specifier.length}, ${cString(bundle.specifier)}, ${index} },`);
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

async function main() {
  const args = process.argv.slice(2);
  if (args.length < 3) {
    console.error(`Usage: ${process.argv[1]} <builtins-root> <output.h> <entry...>`);
    process.exit(1);
  }

  const [builtinsRoot, outputFile, ...entryFiles] = args;
  const bundles = [];

  for (const entryPath of entryFiles) {
    const specifier = toSpecifier(builtinsRoot, entryPath);
    const format = toFormat(entryPath);
    const bytes = await bundleBuiltin(entryPath, format);
    const specifiers = [specifier, ...toAliasSpecifiers(specifier)];
    bundles.push({ entryPath, specifier, specifiers, format, bytes });
  }

  const header = generateHeader(builtinsRoot, bundles);
  const totalBundledBytes = bundles.reduce((sum, bundle) => sum + bundle.bytes.length, 0);

  writeFileSync(outputFile, header);

  console.log(`builtin bundle generated successfully: ${outputFile}`);
  console.log(`  bundled size: ${totalBundledBytes} bytes`);
  console.log(`  modules: ${bundles.length}`);
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
