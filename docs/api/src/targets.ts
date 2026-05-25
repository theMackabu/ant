import { HttpError } from './errors';
import type { AntTarget } from './types';

export const antTargets: AntTarget[] = [
  {
    key: 'linux-x64',
    artifact: 'ant-linux-x64',
    aliases: ['linux-glibc-x64'],
    os: 'linux',
    arch: 'x64',
    libc: 'glibc',
  },
  {
    key: 'linux-aarch64',
    artifact: 'ant-linux-aarch64',
    aliases: ['linux-glibc-aarch64'],
    os: 'linux',
    arch: 'aarch64',
    libc: 'glibc',
  },
  {
    key: 'linux-x64-musl',
    artifact: 'ant-linux-x64-musl',
    aliases: ['linux-musl-x64'],
    os: 'linux',
    arch: 'x64',
    libc: 'musl',
  },
  {
    key: 'linux-aarch64-musl',
    artifact: 'ant-linux-aarch64-musl',
    aliases: ['linux-musl-aarch64'],
    os: 'linux',
    arch: 'aarch64',
    libc: 'musl',
  },
  {
    key: 'darwin-x64',
    artifact: 'ant-darwin-x64',
    aliases: ['macos-x64'],
    os: 'darwin',
    arch: 'x64',
  },
  {
    key: 'darwin-aarch64',
    artifact: 'ant-darwin-aarch64',
    aliases: ['macos-aarch64', 'darwin-arm64', 'macos-arm64'],
    os: 'darwin',
    arch: 'aarch64',
  },
  {
    key: 'windows-x64',
    artifact: 'ant-windows-x64',
    aliases: [],
    os: 'windows',
    arch: 'x64',
  },
];

const targetByName = new Map<string, AntTarget>(
  antTargets.flatMap(target => [
    [target.key, target],
    ...target.aliases.map(alias => [alias, target] as const),
  ]),
);

const archAliases = new Map([
  ['x86_64', 'x64'],
  ['amd64', 'x64'],
  ['x64', 'x64'],
  ['arm64', 'aarch64'],
  ['aarch64', 'aarch64'],
]);

export function resolveTarget(name: string): AntTarget {
  const target = targetByName.get(name);
  if (!target) throw new HttpError(`unknown target: ${name || '(missing)'}`, 400);
  return target;
}

export function resolveArch(name: string): string {
  const arch = archAliases.get(name);
  if (!arch) throw new HttpError(`unknown arch: ${name || '(missing)'}`, 400);
  return arch;
}
