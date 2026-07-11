import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const desktopRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const rootPackagePath = path.join(desktopRoot, 'package.json');
const platforms = JSON.parse(fs.readFileSync(path.join(desktopRoot, 'platforms.json'), 'utf8'));
const semver = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$/;

function isSemver(value) {
  const match = semver.exec(value);
  if (!match) return false;
  return !(match[4] || '').split('.').some(identifier => /^0\d+$/.test(identifier));
}

function usage() {
  process.stdout.write(`Usage: node scripts/set-version.js <version> [--dry-run]

Set the Ant Desktop version across the root package and all supported native
platform packages.

Options:
  --dry-run  Show the files that would change without writing them
  -h, --help Show this help
`);
}

function parseArguments(argv) {
  let version;
  let dryRun = false;
  for (const argument of argv) {
    if (argument === '-h' || argument === '--help') return { help: true };
    if (argument === '--dry-run') {
      dryRun = true;
      continue;
    }
    if (argument.startsWith('-') || version) {
      throw new Error(`Unexpected version argument: ${argument}`);
    }
    version = argument;
  }
  if (!version) throw new Error('A version is required');
  if (!isSemver(version)) {
    throw new Error(`Version must be valid SemVer without a leading v: ${version}`);
  }
  return { dryRun, help: false, version };
}

function readPackage(filename) {
  return JSON.parse(fs.readFileSync(filename, 'utf8'));
}

function platformPackages() {
  return platforms.supported.map(platform => {
    const filename = path.join(desktopRoot, 'packaging', 'npm', platform, 'package.json');
    if (!fs.existsSync(filename)) {
      throw new Error(`Supported platform package is missing: ${filename}`);
    }
    return { filename, metadata: readPackage(filename) };
  });
}

function updateVersion(version, dryRun) {
  const rootPackage = readPackage(rootPackagePath);
  const nativePackages = platformPackages();
  rootPackage.version = version;

  for (const entry of nativePackages) {
    entry.metadata.version = version;
    if (!rootPackage.optionalDependencies?.[entry.metadata.name]) {
      throw new Error(`${rootPackage.name} does not declare ${entry.metadata.name} as an optional dependency`);
    }
    rootPackage.optionalDependencies[entry.metadata.name] = version;
  }

  const updates = [{ filename: rootPackagePath, metadata: rootPackage }, ...nativePackages];
  for (const update of updates) {
    const relative = path.relative(desktopRoot, update.filename) || 'package.json';
    process.stdout.write(`${dryRun ? 'Would update' : 'Updated'} ${relative} to ${version}\n`);
    if (!dryRun) {
      fs.writeFileSync(update.filename, `${JSON.stringify(update.metadata, null, 2)}\n`);
    }
  }
}

try {
  const options = parseArguments(process.argv.slice(2));
  if (options.help) usage();
  else updateVersion(options.version, options.dryRun);
} catch (error) {
  process.stderr.write(`ant-desktop version: ${error.message}\n`);
  process.exitCode = 1;
}
