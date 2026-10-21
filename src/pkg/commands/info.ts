import { escapeName } from 'upm/src/spec.ts';
import { maxSatisfying, validRange } from 'upm/src/semver.ts';
import { describe } from 'upm/src/util.ts';

import { positionals } from '../args.ts';
import type { Manifest } from '../project.ts';
import { registriesFor } from '../registry.ts';
import type { Session } from '../session.ts';
import { c, formatSize } from '../ui/ansi.ts';
import { out, usage, write } from '../ui/output.ts';

const err = (text: string) => write('stderr', text);

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

async function fetchPackument(name: string, session: Session): Promise<Packument | undefined> {
  for (const registry of registriesFor(session.settings)) {
    const response = await fetch(`${registry.replace(/\/+$/, '')}/${escapeName(name)}`, { headers: { accept: 'application/json' } });
    if (response.status === 404) continue;
    if (!response.ok) throw new Error(`GET ${response.url} - ${response.status}`);
    return (await response.json()) as Packument;
  }
  return undefined;
}

const TAG_COLORS: Record<string, 'blue' | 'cyan'> = { beta: 'blue', latest: 'cyan' };

export async function info(args: string[], session: Session): Promise<number> {
  const [spec] = positionals(args);
  if (!spec) return usage('info needs a package name');
  const at = spec.indexOf('@', spec.startsWith('@') ? 1 : 0);
  const name = at < 0 ? spec : spec.slice(0, at);
  const wanted = at < 0 ? 'latest' : spec.slice(at + 1);

  let doc: Packument | undefined;
  try {
    doc = await fetchPackument(name, session);
  } catch (error) {
    err(`Error: ${describe(error)}\n`);
    return 1;
  }
  if (!doc) {
    err(`Error: package not found: ${name}\n`);
    return 1;
  }

  const versions = Object.keys(doc.versions ?? {});
  const version = doc['dist-tags']?.[wanted] ?? (doc.versions?.[wanted] ? wanted : validRange(wanted) ? maxSatisfying(versions, wanted) : undefined);
  const pkg = version ? doc.versions?.[version] : undefined;
  if (!version || !pkg) {
    err(`Error: version not found: ${name}@${wanted}\n`);
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
  if (dist.unpackedSize) out(` ${c.dim}.unpackedSize:${c.reset} ${c.blue}${formatSize(dist.unpackedSize)}${c.reset}\n`);

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
