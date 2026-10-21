import { escapeName } from 'upm/src/spec.ts';
import { maxSatisfying, validRange } from 'upm/src/semver.ts';
import { describe } from 'upm/src/util.ts';

import { positionals } from '../args.ts';
import type { Manifest } from '../project.ts';
import { registriesFor } from '../registry.ts';
import type { Session } from '../session.ts';
import { color, fail, out, plural, size, usage } from '../ui/output.ts';

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

function section(title: string, rows: [string, string][]): void {
  if (rows.length === 0) return;
  const width = Math.max(...rows.map(([label]) => label.length));
  out(`\n${color('bold', title)}\n`);
  for (const [label, value] of rows) out(`  ${color('gray', label.padEnd(width))}  ${value}\n`);
}

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
    fail(describe(error));
    return 1;
  }
  if (!doc) {
    fail(`no package named ${name} (E404)`);
    return 1;
  }

  const versions = Object.keys(doc.versions ?? {});
  const version = doc['dist-tags']?.[wanted] ?? (doc.versions?.[wanted] ? wanted : validRange(wanted) ? maxSatisfying(versions, wanted) : undefined);
  const pkg = version ? doc.versions?.[version] : undefined;
  if (!version || !pkg) {
    fail(`no version of ${name} matches ${wanted} (ETARGET)`);
    return 1;
  }

  const deps = Object.entries(pkg.dependencies ?? {});
  const facts = [pkg.license, plural(deps.length, 'dep'), plural(versions.length, 'version')].filter(Boolean);
  out(`${color('cyan', name)}@${version} ${color('gray', `· ${facts.join(' · ')}`)}\n`);
  if (pkg.description) out(`${pkg.description}\n`);
  if (pkg.homepage) out(`${color('gray', pkg.homepage)}\n`);
  if (pkg.keywords?.length) out(`${color('gray', `keywords: ${pkg.keywords.join(', ')}`)}\n`);

  section('dependencies', deps);
  const dist = pkg.dist ?? {};
  section('dist', [
    ['tarball', dist.tarball ?? ''],
    ['shasum', dist.shasum ?? ''],
    ['integrity', dist.integrity ?? ''],
    ['unpacked', dist.unpackedSize ? size(dist.unpackedSize) : ''],
  ].filter(([, value]) => value) as [string, string][]);
  section('dist-tags', Object.entries(doc['dist-tags'] ?? {}));
  section('maintainers', (doc.maintainers ?? []).map(m => [m.name ?? '', m.email ? `<${m.email}>` : '']));
  if (doc.time?.[version]) out(`\n${color('gray', `published ${doc.time[version]}`)}\n`);
  return 0;
}
