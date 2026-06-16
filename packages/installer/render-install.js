import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { chmodSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, '../..');
const configPath = path.join(scriptDir, 'install.json');
const templatePath = path.join(scriptDir, 'install.template.sh');
const outputPath = path.join(scriptDir, 'dist', 'install');

const config = JSON.parse(readFileSync(configPath, 'utf8'));
const template = readFileSync(templatePath, 'utf8');

function gitHash() {
  try {
    return execFileSync('git', ['rev-parse', '--short', 'HEAD'], {
      cwd: repoRoot,
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore']
    }).trim();
  } catch {
    return 'unknown';
  }
}

function requireString(object, name) {
  if (typeof object[name] !== 'string' || object[name].length === 0) {
    throw new Error(`install.json must define non-empty string "${name}"`);
  }
}

function shellSingleQuote(value) {
  return `'${String(value).replaceAll("'", "'\\''")}'`;
}

function patternFor(parts) {
  if (!Array.isArray(parts) || parts.length === 0) {
    throw new Error('target matchers must be non-empty arrays');
  }
  return parts.map(part => `*${String(part).replaceAll('*', '\\*')}`).join('');
}

function renderMatcher(target) {
  if (target.all) return patternFor(target.all);
  if (target.any) return target.any.map(patternFor).join('|');
  return null;
}

function validateConfig(config) {
  for (const name of ['name', 'binary', 'installEnv', 'defaultInstallDir', 'downloadUrl']) {
    requireString(config, name);
  }

  if (!Array.isArray(config.targets) || config.targets.length === 0) {
    throw new Error('install.json must define at least one target');
  }

  const defaults = config.targets.filter(target => target.default);
  if (defaults.length !== 1) {
    throw new Error('install.json must define exactly one default target');
  }

  for (const target of config.targets) {
    requireString(target, 'name');
    if (!target.default && !renderMatcher(target)) {
      throw new Error(`target "${target.name}" needs "all", "any", or "default"`);
    }
  }
}

function renderTargetSelection(config) {
  const defaultTarget = config.targets.find(target => target.default);
  const rosettaTarget = config.targets.find(target => target.rosettaTarget);
  const lines = ['  local platform target rosetta', '  platform="$(uname -ms)"', '  case "$platform" in'];

  for (const target of config.targets) {
    if (target.default) {
      continue;
    }

    const matcher = renderMatcher(target);
    if (target.unsupported) {
      lines.push(`    ${matcher}) die ${shellSingleQuote(target.unsupported)} ;;`);
    } else {
      lines.push(`    ${matcher}) target=${shellSingleQuote(target.name)} ;;`);
    }
  }

  lines.push(`    *) target=${shellSingleQuote(defaultTarget.name)} ;;`);
  lines.push('  esac');

  if (config.linuxMuslFile && config.linuxMuslSuffix) {
    lines.push('');
    lines.push(`  if [[ "$target" == linux* && -f ${shellSingleQuote(config.linuxMuslFile)} ]]; then`);
    lines.push(`    target="$target${config.linuxMuslSuffix}"`);
    lines.push('  fi');
  }

  if (rosettaTarget) {
    lines.push('');
    lines.push(`  if [[ "$target" == ${shellSingleQuote(rosettaTarget.name)} ]]; then`);
    lines.push('    rosetta="$(sysctl -n sysctl.proc_translated 2>/dev/null || true)"');
    lines.push('    if [[ "$rosetta" == "1" ]]; then');
    lines.push(`      target=${shellSingleQuote(rosettaTarget.rosettaTarget)}`);
    lines.push('      dim "Your shell is running in Rosetta 2. Downloading $app_name for $target instead"');
    lines.push('    fi');
    lines.push('  fi');
  }

  lines.push('');
  lines.push('  printf \'%s\' "$target"');
  return lines.join('\n');
}

function renderInstallScript() {
  validateConfig(config);

  const replacements = {
    '{{configPath}}': path.relative(repoRoot, configPath),
    '{{generatorPath}}': path.relative(repoRoot, fileURLToPath(import.meta.url)),
    '{{gitHash}}': gitHash(),
    '{{name}}': config.name,
    '{{binary}}': config.binary,
    '{{installEnv}}': config.installEnv,
    '{{defaultInstallDir}}': config.defaultInstallDir,
    '{{downloadUrl}}': config.downloadUrl,
    '{{targetSelection}}': renderTargetSelection(config)
  };

  let output = template;
  for (const [placeholder, value] of Object.entries(replacements)) {
    output = output.replaceAll(placeholder, String(value));
  }
  return output;
}

const output = renderInstallScript();
mkdirSync(path.dirname(outputPath), { recursive: true });
writeFileSync(outputPath, output, { mode: 0o755 });
chmodSync(outputPath, 0o755);
execFileSync('bash', ['-n', outputPath], { stdio: 'inherit' });

for (const relativeCopy of config.publishedCopies ?? []) {
  const copyPath = path.join(repoRoot, relativeCopy);
  if (existsSync(copyPath)) {
    mkdirSync(path.dirname(copyPath), { recursive: true });
    writeFileSync(copyPath, output, { mode: 0o755 });
    chmodSync(copyPath, 0o755);
    execFileSync('bash', ['-n', copyPath], { stdio: 'inherit' });
  }
}
