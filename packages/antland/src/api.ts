import semiver from 'semiver';
import { AntPackage } from './utils';

export const REGISTRY_URL = process.env.ANTS_REGISTRY ?? 'https://npm.ants.land';

export interface Packument {
  name: string;
  description?: string;
  'dist-tags': { latest?: string };
  versions: Record<
    string,
    {
      name: string;
      version: string;
      description?: string;
      dist: { tarball: string; shasum?: string; integrity?: string };
    }
  >;
  time: Record<string, string>;
}

export async function getPackument(pkg: AntPackage): Promise<Packument> {
  const url = `${REGISTRY_URL}/${pkg.id}`;
  const res = await fetch(url);
  if (!res.ok) {
    await res.body?.cancel();
    throw new Error(`Received ${res.status} from ${url}`);
  }
  return (await res.json()) as Packument;
}

export function resolveVersion(meta: Packument, requested: string | null): string {
  if (requested !== null) {
    if (!meta.versions[requested]) throw new Error(`Version ${requested} not found`);
    return requested;
  }
  const latest = meta['dist-tags']?.latest;
  if (latest && meta.versions[latest]) return latest;
  const versions = Object.keys(meta.versions);
  if (versions.length === 0) throw new Error('No published versions');
  return versions.sort(semiver).reverse()[0];
}

export async function getTarballUrl(pkg: AntPackage): Promise<string> {
  const meta = await getPackument(pkg);
  const version = resolveVersion(meta, pkg.version);
  const tarball = meta.versions[version]?.dist?.tarball;
  if (!tarball) throw new Error(`No tarball for ${pkg.id}@${version}`);
  return tarball;
}
