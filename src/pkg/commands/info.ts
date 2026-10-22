import { readConfig } from 'upm/src/config.ts';
import { hosts, registryBase } from 'upm/src/registry.ts';
import { escapeName } from 'upm/src/spec.ts';
import { maxSatisfying, validRange } from 'upm/src/semver.ts';
import { describe } from 'upm/src/util.ts';

import { nameOfSpec, parseArgs } from '../args.ts';
import { registryOf } from '../registry.ts';
import type { Source } from '../settings.ts';
import type { Manifest } from '../project.ts';
import { projectRoot } from '../session.ts';
import type { Session } from '../session.ts';
import { c, error } from '../ui/ansi.ts';
import { out, size, usage } from '../ui/output.ts';

interface Packument {
  name: string;
  'dist-tags'?: Record<string, string>;
  versions?: Record<string, Manifest & {
    license?: string; description?: string; homepage?: string; keywords?: string[];
    dist?: { tarball?: string; shasum?: string; integrity?: string; unpackedSize?: number };
  }>;
  maintainers?: { name?: string; email?: string }[];
  time?: Record<string, string>;
}

function registriesFor(name: string, dir: string, source: Source | undefined): string[] {
  if (source) return [registryBase(registryOf(source))];
  const { registry, scopes, fallback } = readConfig(dir);
  const base = hosts(registry, scopes)(name);
  return base === registry && fallback ? [base, fallback] : [base];
}

async function fetchPackument(name: string, dir: string, source: Source | undefined): Promise<Packument | undefined> {
  for (const registry of registriesFor(name, dir, source)) {
    const response = await fetch(`${registry}/${escapeName(name)}`, { headers: { accept: 'application/json' } });
    if (response.status === 404) continue;
    if (!response.ok) throw new Error(`GET ${response.url} - ${response.status}`);
    return (await response.json()) as Packument;
  }
  return undefined;
}

const TAG_COLORS: Record<string, 'blue' | 'cyan'> = { beta: 'blue', latest: 'cyan' };

export async function info(args: string[], session: Session): Promise<number> {
  const [first] = parseArgs(args).sourced;
  if (!first) return usage('info needs a package name');
  const { spec, source } = first;
  const name = nameOfSpec(spec);
  const wanted = spec.slice(name.length + 1) || 'latest';

  let doc: Packument | undefined;
  try {
    doc = await fetchPackument(name, projectRoot(session), source);
  } catch (cause) {
    error(describe(cause));
    return 1;
  }
  if (!doc) {
    error(`package not found: ${name}`);
    return 1;
  }

  const versions = Object.keys(doc.versions ?? {});
  const version = doc['dist-tags']?.[wanted] ?? (doc.versions?.[wanted] ? wanted : validRange(wanted) ? maxSatisfying(versions, wanted) : undefined);
  const pkg = version ? doc.versions?.[version] : undefined;
  if (!version || !pkg) {
    error(`version not found: ${name}@${wanted}`);
    return 1;
  }

  const deps = Object.entries(pkg.dependencies ?? {});
  out(`${c.blue}${c.ul}${name}${c.ulOff}@${c.blue}${c.bold}${c.ul}${version}${c.reset}`);
  if (pkg.license) out(` | ${c.cyan}${pkg.license}${c.reset}`);
  out(` | deps: ${deps.length} | versions: ${versions.length}\n`);

  if (pkg.description) out(`${pkg.description}\n`);
  if (pkg.homepage) out(`${c.blue}${pkg.homepage}${c.reset}\n`);
  if (pkg.keywords?.length) out(`keywords: ${pkg.keywords.join(', ')}\n`);

  if (deps.length > 0) {
    out(`\n${c.bold}dependencies${c.reset} (${deps.length}):\n`);
    for (const [dep, range] of deps) out(`- ${c.cyan}${dep}${c.reset}: ${range}\n`);
  }

  const dist = pkg.dist ?? {};
  out(`\n${c.bold}dist${c.reset}\n`);
  if (dist.tarball) out(` ${c.dim}.tarball:${c.reset} ${dist.tarball}\n`);
  if (dist.shasum) out(` ${c.dim}.shasum:${c.reset} ${c.green}${dist.shasum}${c.reset}\n`);
  if (dist.integrity) out(` ${c.dim}.integrity:${c.reset} ${c.green}${dist.integrity}${c.reset}\n`);
  if (dist.unpackedSize) out(` ${c.dim}.unpackedSize:${c.reset} ${c.blue}${size(dist.unpackedSize)}${c.reset}\n`);

  const tags = Object.entries(doc['dist-tags'] ?? {});
  if (tags.length > 0) {
    out(`\n${c.bold}dist-tags:${c.reset}\n`);
    for (const [tag, v] of tags) out(`${c[TAG_COLORS[tag] ?? 'magenta']}${tag}${c.reset}: ${v}\n`);
  }

  if (doc.maintainers?.length) {
    out(`\n${c.bold}maintainers:${c.reset}\n`);
    for (const m of doc.maintainers) out(`- ${m.name ?? ''}${m.email ? ` <${m.email}>` : ''}\n`);
  }

  if (doc.time?.[version]) out(`\n${c.bold}Published:${c.reset} ${doc.time[version]}\n`);
  return 0;
}
