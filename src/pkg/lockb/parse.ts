export const LOCKB = 'ant.lockb';

const MAGIC = 0x504b474c;
const VERSION = 4;
const HEADER_SIZE = 88;
const PACKAGE_SIZE = 136;
const DEPENDENCY_SIZE = 16;
const BIN_SIZE = 20;
const DISABLED_SIZE = 20;
const PLATFORM_SIZE = 28;

export interface LockbDependency {
  index: number;
  constraint: string;
  peer: boolean;
  optional: boolean;
}

export interface LockbPackage {
  name: string;
  version: string;
  integrity: string;
  resolved?: string;
  parent: string;
  direct: boolean;
  hasBin: boolean;
  dependencies: LockbDependency[];
  bin?: Record<string, string>;
  os?: string[];
  cpu?: string[];
  libc?: string[];
}

function invalid(reason: string): Error {
  return new Error(`${LOCKB} is not a valid v${VERSION} lockfile: ${reason}`);
}

function align4(offset: number): number {
  return (offset + 3) & ~3;
}

export function parseLockb(bytes: Uint8Array): LockbPackage[] {
  if (bytes.length < HEADER_SIZE) throw invalid('too short');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u32 = (at: number) => view.getUint32(at, true);

  if (u32(0) !== MAGIC) throw invalid('bad magic');
  if (u32(4) !== VERSION) throw invalid(`version ${u32(4)}`);

  const packageCount = u32(8);
  const dependencyCount = u32(12);
  const stringOffset = u32(16);
  const stringSize = u32(20);
  const packageOffset = u32(24);
  const dependencyOffset = u32(28);
  const hashOffset = u32(32);
  const hashSize = u32(36);
  const binOffset = u32(52);
  const binCount = u32(56);
  const disabledCount = u32(60);
  const platformOffsetField = u32(64);
  const platformCount = u32(68);

  const within = (offset: number, count: number, size: number, what: string) => {
    if (offset + count * size > bytes.length) throw invalid(`${what} runs past the end`);
  };
  
  within(stringOffset, stringSize, 1, 'string table');
  within(packageOffset, packageCount, PACKAGE_SIZE, 'packages');
  within(dependencyOffset, dependencyCount, DEPENDENCY_SIZE, 'dependencies');
  within(hashOffset, hashSize, 8, 'hash table');
  if (binCount > 0) within(binOffset, binCount, BIN_SIZE, 'bins');

  const decoder = new TextDecoder();
  const str = (at: number): string => {
    const offset = u32(at);
    const len = u32(at + 4);
    if (len === 0 || offset >= stringSize) return '';
    const start = stringOffset + offset;
    return decoder.decode(bytes.subarray(start, Math.min(start + len, stringOffset + stringSize)));
  };
  const list = (value: string) => (value ? value.split(',').filter(Boolean) : undefined);

  const packages: LockbPackage[] = [];
  for (let i = 0; i < packageCount; i++) {
    const at = packageOffset + i * PACKAGE_SIZE;
    const major = view.getBigUint64(at + 8, true);
    const minor = view.getBigUint64(at + 16, true);
    const patch = view.getBigUint64(at + 24, true);
    const prerelease = str(at + 32);
    const digest = bytes.subarray(at + 40, at + 104);
    const flags = bytes[at + 128];
    const depsStart = u32(at + 120);
    const depsCount = u32(at + 124);
    if (depsStart + depsCount > dependencyCount) throw invalid(`package ${i} has dependencies out of range`);

    const dependencies: LockbDependency[] = [];
    for (let d = depsStart; d < depsStart + depsCount; d++) {
      const dat = dependencyOffset + d * DEPENDENCY_SIZE;
      const index = u32(dat);
      if (index >= packageCount) throw invalid(`dependency ${d} points past the packages`);
      const depFlags = bytes[dat + 12];
      dependencies.push({
        index,
        constraint: str(dat + 4),
        peer: (depFlags & 1) !== 0,
        optional: (depFlags & 4) !== 0,
      });
    }

    const resolved = str(at + 104);
    packages.push({
      name: str(at),
      version: `${major}.${minor}.${patch}${prerelease ? `-${prerelease}` : ''}`,
      integrity: digest.some(byte => byte !== 0) ? `sha512-${Buffer.from(digest).toString('base64')}` : '',
      ...(resolved && { resolved }),
      parent: str(at + 112),
      direct: (flags & 64) !== 0,
      hasBin: (flags & 16) !== 0,
      dependencies,
    });
  }

  for (let i = 0; i < binCount; i++) {
    const at = binOffset + i * BIN_SIZE;
    const pkg = packages[u32(at)];
    if (!pkg) throw invalid(`bin ${i} points past the packages`);
    (pkg.bin ??= {})[str(at + 4)] = str(at + 12);
  }

  if (platformCount > 0) {
    const binBase = binOffset !== 0 ? binOffset + binCount * BIN_SIZE : hashOffset + hashSize * 8;
    const disabledOffset = align4(binBase);
    const platformOffset = platformOffsetField || align4(disabledOffset + disabledCount * DISABLED_SIZE);
    within(platformOffset, platformCount, PLATFORM_SIZE, 'platform entries');
    for (let i = 0; i < platformCount; i++) {
      const at = platformOffset + i * PLATFORM_SIZE;
      const pkg = packages[u32(at)];
      if (!pkg) throw invalid(`platform entry ${i} points past the packages`);
      pkg.os = list(str(at + 4));
      pkg.cpu = list(str(at + 12));
      pkg.libc = list(str(at + 20));
    }
  }

  return packages;
}
