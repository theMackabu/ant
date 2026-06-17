import * as path from 'node:path';
import * as fs from 'node:fs';
import { AntPackage, getNewLineChars, styleText, timeAgo } from './utils';
import { Bun, getPkgManager, YarnBerry, type InstallMode, type PkgManagerName } from './pkg_manager';
import { getPackument, getTarballUrl, resolveVersion, REGISTRY_URL } from './api';

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
  const { pkgManager } = await getPkgManager(process.cwd(), options.pkgManagerName);
  console.log(`Publishing to ${styleText('cyan', REGISTRY_URL)}...`);
  console.log(styleText('dim', 'Authenticate with a token from your package’s Publish tab (//npm.ants.land/:_authToken=...).'));
  await pkgManager.publish(['--registry', REGISTRY_URL, ...options.publishArgs.filter(a => a !== '--verbose')]);
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
