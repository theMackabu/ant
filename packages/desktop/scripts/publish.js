import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { execute } = require('./lib/command.cjs');
const { packagePlatform } = require('./package-platform.cjs');

const scriptsRoot = path.dirname(fileURLToPath(import.meta.url));
const desktopRoot = path.resolve(scriptsRoot, '..');
const repositoryRoot = path.resolve(desktopRoot, '../..');
const platformRoot = path.join(desktopRoot, 'packaging', 'npm', 'darwin-arm64');

function readPackage(root) {
  return JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
}

function usage() {
  process.stdout.write(`Usage: node scripts/publish.js [--npm | --ant] [--dry-run]

Build Ant Desktop and publish the platform and root packages to npm and
ants.land. With no registry option, both registries are published. Platform
packages are always published before ant-desktop.

Options:
  --npm      Publish only to npm
  --ant      Publish only to ants.land
  --dry-run  Build and pack everything without uploading
  -h, --help Show this help
`);
}

function parseArguments(argv) {
  let dryRun = false;
  let npm = false;
  let ant = false;
  for (const argument of argv) {
    if (argument === '--dry-run') dryRun = true;
    else if (argument === '--npm') npm = true;
    else if (argument === '--ant') ant = true;
    else if (argument === '-h' || argument === '--help') return { help: true };
    else throw new Error(`Unknown publish option: ${argument}`);
  }
  const all = !npm && !ant;
  return { ant: ant || all, dryRun, help: false, npm: npm || all };
}

function validateVersions(rootPackage, platformPackage) {
  const selectedVersion = rootPackage.optionalDependencies?.[platformPackage.name];
  if (rootPackage.version !== platformPackage.version) {
    throw new Error(
      `Package versions do not match: ${rootPackage.name}@${rootPackage.version} and ` + `${platformPackage.name}@${platformPackage.version}`
    );
  }
  if (selectedVersion !== platformPackage.version) {
    throw new Error(
      `${rootPackage.name} must select ${platformPackage.name}@${platformPackage.version}; ` + `found ${selectedVersion || 'no optional dependency'}`
    );
  }
}

function step(message, command, args, cwd) {
  process.stdout.write(`\n${message}\n`);
  execute(command, args, { cwd });
}

function publishPackages(command, packages, registryArguments, dryRun) {
  for (const entry of packages) {
    step(
      `Publishing ${entry.metadata.name}@${entry.metadata.version} to ${entry.registry}`,
      command,
      [...registryArguments, ...(dryRun ? ['--dry-run'] : [])],
      entry.root
    );
  }
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  if (options.help) return usage();

  const rootPackage = readPackage(desktopRoot);
  const platformPackage = readPackage(platformRoot);
  validateVersions(rootPackage, platformPackage);

  step('Building Ant Desktop', process.execPath, [path.join(scriptsRoot, 'build.js')], desktopRoot);

  process.stdout.write('\nPreparing native platform package\n');
  packagePlatform();

  const packageOrder = [
    { root: platformRoot, metadata: platformPackage },
    { root: desktopRoot, metadata: rootPackage }
  ];

  const registries = [];
  if (options.npm) {
    publishPackages(
      'npm',
      packageOrder.map(entry => ({ ...entry, registry: 'npm' })),
      ['publish', '--access', 'public'],
      options.dryRun
    );
    registries.push('npm');
  }
  if (options.ant) {
    publishPackages(
      'antland',
      packageOrder.map(entry => ({ ...entry, registry: 'ants.land' })),
      ['publish'],
      options.dryRun
    );
    registries.push('ants.land');
  }

  process.stdout.write(
    options.dryRun
      ? '\nPublish dry run completed; nothing was uploaded.\n'
      : `\nPublished Ant Desktop ${rootPackage.version} to ${registries.join(' and ')}.\n`
  );
}

const invokedPath = process.argv[1] ? path.resolve(process.argv[1]) : '';
if (invokedPath === fileURLToPath(import.meta.url)) {
  try {
    await main();
  } catch (error) {
    process.stderr.write(`ant-desktop publish: ${error.message}\n`);
    process.exitCode = 1;
  }
}

export { parseArguments };
