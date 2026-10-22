import { setShareFilter } from 'upm/src/link.ts';

import { buildScripts } from './lifecycle/discover.ts';
import type { Manifest } from './project.ts';

export function shareSelfContained(): void {
  setShareFilter((_pkg, manifest, files) => {
    const hasGyp = () => files.some(file => file.path === 'binding.gyp');
    return buildScripts(manifest as Manifest, hasGyp).length === 0;
  });
}
