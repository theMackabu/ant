import { setShareFilter } from 'upm/src/link.ts';

const LIFECYCLE = ['preinstall', 'install', 'postinstall'];

export function shareSelfContained(): void {
  setShareFilter((_pkg, manifest, files) => {
    const scripts = (manifest.scripts ?? {}) as Record<string, unknown>;
    if (LIFECYCLE.some(name => scripts[name])) return false;
    return !files.some(file => file.path === 'binding.gyp');
  });
}
