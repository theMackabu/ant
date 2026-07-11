import * as os from 'node:os';
import * as path from 'node:path';
import * as fs from 'node:fs';
import { AntPackage, confirm, exec, getNewLineChars, isInteractive, styleText, timeAgo } from './utils';
import { Bun, getPkgManager, YarnBerry, type InstallMode, type PkgManagerName } from './pkg_manager';
import { getPackument, getScore, getTarballUrl, resolveVersion, uploadSnippet, fetchSnippet, REGISTRY_URL, SITE_URL, type PackageScore } from './api';
import { readToken } from './login';
import { multipartPublish } from './multipart_publish';

const NPMRC_FILE = '.npmrc';
const BUNFIG_FILE = 'bunfig.toml';

async function setupNpmRc(dir: string, scope: string) {
  const line = `@${scope}:registry=${REGISTRY_URL}`;
  const npmRcPath = path.join(dir, NPMRC_FILE);
  try {
    let content = await fs.promises.readFile(npmRcPath, 'utf-8');
    if (!content.includes(line)) {
      const nl = getNewLineChars(content);
      content += (content.endsWith(nl) ? '' : nl) + line + nl;
      await fs.promises.writeFile(npmRcPath, content);
      console.log(`Configured ${styleText('cyan', `@${scope}`)} in ${NPMRC_FILE}`);
    }
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === 'ENOENT') {
      await fs.promises.writeFile(npmRcPath, line + '\n');
      console.log(`Created ${NPMRC_FILE} for ${styleText('cyan', `@${scope}`)}`);
    } else {
      throw err;
    }
  }
}

async function setupBunfig(dir: string, scope: string) {
  const bunfigPath = path.join(dir, BUNFIG_FILE);
  const block = `[install.scopes]\n"@${scope}" = "${REGISTRY_URL}"\n`;
  const test = new RegExp(`^"@${scope}"\\s*=`, 'm');
  try {
    let content = await fs.promises.readFile(bunfigPath, 'utf-8');
    if (!test.test(content)) {
      content += (content.endsWith('\n') ? '' : '\n') + block;
      await fs.promises.writeFile(bunfigPath, content);
    }
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === 'ENOENT') await fs.promises.writeFile(bunfigPath, block);
    else throw err;
  }
}

export interface BaseOptions {
  pkgManagerName: PkgManagerName | null;
}
export interface InstallOptions extends BaseOptions {
  mode: InstallMode;
}

export async function install(packages: AntPackage[], options: InstallOptions) {
  const { pkgManager, root } = await getPkgManager(process.cwd(), options.pkgManagerName);
  if (packages.length === 0) {
    await pkgManager.install([], options.mode);
    return;
  }

  const specs: string[] = [];
  for (const pkg of packages) {
    if (pkg.scoped) {
      if (pkgManager instanceof YarnBerry) await pkgManager.setConfigValue(`npmScopes.${pkg.scope}.npmRegistryServer`, REGISTRY_URL);
      else if (pkgManager instanceof Bun && !(await pkgManager.isNpmrcSupported())) await setupBunfig(root, pkg.scope);
      else await setupNpmRc(root, pkg.scope);
      specs.push(pkg.toString());
    } else {
      const url = await getTarballUrl(pkg);
      specs.push(`${pkg.name}@${url}`);
    }
  }

  console.log(`Installing ${styleText('cyan', packages.map(p => p.toString()).join(', '))} from ants.land...`);
  await pkgManager.install(specs, options.mode);
}

export async function remove(packages: AntPackage[], options: BaseOptions) {
  const { pkgManager } = await getPkgManager(process.cwd(), options.pkgManagerName);
  console.log(`Removing ${styleText('cyan', packages.map(p => p.id).join(', '))}...`);
  await pkgManager.remove(packages.map(p => p.id));
}

export async function runScript(script: string, options: BaseOptions) {
  const { pkgManager } = await getPkgManager(process.cwd(), options.pkgManagerName);
  await pkgManager.runScript(script);
}

export interface PublishOptions extends BaseOptions {
  publishArgs: string[];
}

export async function publish(options: PublishOptions) {
  console.log(`Publishing to ${styleText('cyan', REGISTRY_URL)}...`);
  console.log(styleText('dim', 'Authenticate with a token from your package’s Publish tab (//npm.ants.land/:_authToken=...).'));
  const token = await readToken();
  await multipartPublish(REGISTRY_URL, token, options.publishArgs.filter(a => a !== '--verbose'));
}

const RISK_LABELS: Record<string, string> = {
  'install-scripts': 'runs install scripts',
  eval: 'dynamic eval',
  'encoded-blobs': 'encoded blobs',
  'native-binary': 'ships a native binary',
  'suspicious-files': 'suspicious files',
  'compression-bomb': 'huge when unpacked',
  process: 'spawns processes',
  network: 'network access',
  filesystem: 'filesystem access',
  env: 'reads env vars',
  oversized: 'large package'
};

const DANGER_RISKS = new Set(['install-scripts', 'eval', 'encoded-blobs', 'native-binary', 'suspicious-files', 'compression-bomb']);

type StyleColor = Parameters<typeof styleText>[0];
const scoreColor = (score: number): StyleColor => (score >= 70 ? 'green' : score >= 40 ? 'yellow' : 'red');
const fieldLabel = (s: string) => '  ' + styleText('dim', s.padEnd(11));

function printSafetyReport(id: string, version: string, score: PackageScore | null) {
  console.log();
  console.log(`  ${styleText('cyan', `${id}@${version}`)}  ${styleText('dim', 'ants.land')}`);
  console.log();
  if (!score) {
    console.log(`  ${styleText('yellow', 'No safety report available for this package.')}`);
    return;
  }

  console.log(fieldLabel('Score') + styleText(scoreColor(score.score), `${score.score}/100`));

  if (score.publisher) {
    const p = score.publisher;
    let line = p.handle ? `${p.name} ${styleText('dim', `@${p.handle}`)}` : p.name;
    if (p.githubVerified && p.githubLogin) line += `  ${styleText('green', `✓ github:${p.githubLogin}`)}`;
    console.log(fieldLabel('Publisher') + line);
  }

  if (score.risks.length) {
    const reviewed = new Set(score.acknowledgedRisks ?? []);
    const parts = score.risks.map(r => {
      const label = RISK_LABELS[r] ?? r;
      if (reviewed.has(r)) return styleText('dim', styleText('strikethrough', label));
      return styleText(DANGER_RISKS.has(r) ? 'red' : 'yellow', label);
    });
    let line = fieldLabel('Risks') + parts.join(styleText('dim', ', '));
    const left = score.risks.filter(r => !reviewed.has(r)).length;
    if (reviewed.size && left === 0) line += styleText('dim', '  (all reviewed)');
    console.log(line);
  } else {
    console.log(fieldLabel('Risks') + styleText('green', 'none detected'));
  }

  if (score.audit?.audited) {
    const a = score.audit;
    const supply = a.supplyChain != null ? ` ${a.supplyChain}% supply-chain` : '';
    const verdict = a.passed ? styleText('green', `✓ passed audit${supply}`) : styleText('yellow', `⚠ audit flagged issues${supply}`);
    console.log(fieldLabel('Audit') + verdict);
  }

  if (score.flags.obfuscated) console.log(fieldLabel('') + styleText('red', 'ships obfuscated source'));
  else if (score.flags.minified) console.log(fieldLabel('') + styleText('yellow', 'ships minified-only source'));

  if (score.typosquat) {
    console.log();
    console.log(`  ${styleText('yellow', '⚠')} Name looks similar to ${styleText('cyan', score.typosquat)} (possible typosquat).`);
  }
}

export interface ExecOptions extends BaseOptions {
  yes: boolean;
  binArgs: string[];
}

function resolveBinName(pkg: AntPackage, bin: string | Record<string, string> | undefined): string {
  const fallback = pkg.name;
  if (typeof bin === 'string') return fallback;
  if (!bin) return fallback;
  if (bin[fallback]) return fallback;
  const names = Object.keys(bin);
  if (names.length === 1) return names[0];
  return fallback;
}

export async function execPackage(raw: string, options: ExecOptions) {
  const pkg = AntPackage.from(raw);
  const meta = await getPackument(pkg);
  const version = resolveVersion(meta, pkg.version);
  const info = meta.versions[version];
  const tarball = info?.dist?.tarball;
  if (!tarball) throw new Error(`No tarball for ${pkg.id}@${version}`);
  const binName = resolveBinName(pkg, info.bin);

  const score = await getScore(AntPackage.from(`${pkg.id}@${version}`));
  printSafetyReport(pkg.id, version, score);

  if (!options.yes) {
    if (!isInteractive()) throw new Error('Refusing to run without confirmation in a non-interactive shell, re-run with --yes.');
    const ok = await confirm(`\n  ${styleText('yellow', 'Run this package?')} ${styleText('dim', '[y/N]')} `, false);
    if (!ok) {
      console.log(styleText('dim', '  Aborted.'));
      return;
    }
  }

  const { pkgManager } = await getPkgManager(process.cwd(), options.pkgManagerName);
  console.log();
  console.log(`Running ${styleText('cyan', `${pkg.id}@${version}`)} from ants.land...`);
  await pkgManager.dlx(tarball, options.binArgs, binName);
}

export async function uploadSnippetFile(file: string, options: { private: boolean }) {
  const token = await readToken();
  if (!token) throw new Error('Not logged in. Run `antland login` first.');
  let content: string;
  try {
    content = await fs.promises.readFile(file, 'utf-8');
  } catch {
    throw new Error(`Could not read ${file}`);
  }
  const s = await uploadSnippet(token, path.basename(file), content, options.private);
  console.log(`Uploaded ${styleText('cyan', s.filename)} as a ${s.private ? 'private' : 'public'} snippet.`);
  console.log();
  console.log(`  run:  ${styleText('green', s.run)}`);
  console.log(`  raw:  ${styleText('cyan', s.url)}`);
}

export interface ExecSnippetOptions {
  yes: boolean;
  binArgs: string[];
}

export async function execSnippet(id: string, options: ExecSnippetOptions) {
  const token = await readToken();
  const { filename, content } = await fetchSnippet(id, token);

  console.log();
  console.log(`  ${styleText('cyan', `snippet:${id}`)}  ${styleText('dim', filename)}`);
  console.log(`  ${styleText('yellow', 'Unverified single-file snippet.')} ${styleText('dim', `${SITE_URL}/s/${id}`)}`);
  if (!options.yes) {
    if (!isInteractive()) throw new Error('Refusing to run a snippet without confirmation in a non-interactive shell, re-run with --yes.');
    const ok = await confirm(`\n  ${styleText('yellow', 'Run this snippet?')} ${styleText('dim', '[y/N]')} `, false);
    if (!ok) {
      console.log(styleText('dim', '  Aborted.'));
      return;
    }
  }

  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'antland-snippet-'));
  const tmp = path.join(dir, filename);
  await fs.promises.writeFile(tmp, content);

  let cmd: string;
  let args: string[];
  if (/\.(ts|mts|cts)$/i.test(filename)) {
    cmd = 'npx';
    args = ['-y', 'tsx', tmp, ...options.binArgs];
  } else if (content.startsWith('#!') && process.platform !== 'win32') {
    await fs.promises.chmod(tmp, 0o755);
    cmd = tmp;
    args = options.binArgs;
  } else {
    cmd = 'node';
    args = [tmp, ...options.binArgs];
  }

  console.log();
  console.log(`Running ${styleText('cyan', `snippet:${id}`)} from ants.land...`);
  try {
    await exec(cmd, args, process.cwd());
  } finally {
    await fs.promises.rm(dir, { recursive: true, force: true }).catch(() => {});
  }
}

export async function showPackageInfo(raw: string) {
  const pkg = AntPackage.from(raw);
  const meta = await getPackument(pkg);
  const version = resolveVersion(meta, pkg.version);
  const info = meta.versions[version];
  const versionCount = Object.keys(meta.versions).length;
  const published = meta.time[version] ? Date.now() - new Date(meta.time[version]).getTime() : null;

  console.log();
  console.log(
    `${styleText('cyan', `${pkg.id}@${version}`)} | latest: ${styleText('magenta', meta['dist-tags']?.latest ?? '-')} | versions: ${styleText('magenta', String(versionCount))}`
  );
  if (info?.description) console.log(info.description);
  console.log();
  console.log(`tarball:   ${styleText('cyan', info?.dist?.tarball ?? '-')}`);
  if (info?.dist?.integrity) console.log(`integrity: ${styleText('cyan', info.dist.integrity)}`);
  if (published !== null) {
    console.log();
    console.log(`published: ${styleText('magenta', timeAgo(published))}`);
  }
}
