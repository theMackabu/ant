import { antTargets } from './targets';

export function routeIndex(url: URL) {
  return {
    schema: 1,
    routes: {
      latest: route(url, '/v1/latest'),
      version: route(url, '/v1/version?kind={kind}&target={target}&arch={arch}&current={version}'),
      check: route(url, '/v1/check?kind={kind}&target={target}&arch={arch}&current={version}'),
      downloads: {
        ant: route(url, '/v1/download/ant/{target}'),
        sandbox: route(url, '/v1/download/sandbox/{arch}'),
        kernel: route(url, '/v1/download/kernel/{arch}'),
      },
    },
    targets: {
      ant: antTargets.map(target => ({
        name: target.key,
        aliases: target.aliases,
        url: route(url, `/v1/download/ant/${encodeURIComponent(target.key)}`),
        version_url: route(
          url,
          `/v1/version?kind=ant&target=${encodeURIComponent(target.key)}&current={version}`,
        ),
      })),
      sandbox: arches.map(arch => ({
        name: `sandbox-${arch}`,
        arch,
        url: route(url, `/v1/download/sandbox/${arch}`),
        version_url: route(url, `/v1/version?kind=sandbox&arch=${arch}&current={version}`),
      })),
      kernel: arches.map(arch => ({
        name: `kernel-${arch}`,
        arch,
        url: route(url, `/v1/download/kernel/${arch}`),
        version_url: route(url, `/v1/version?kind=kernel&arch=${arch}&current={version}`),
      })),
    },
    arches: ['x64', 'aarch64'],
  };
}

const arches = ['x64', 'aarch64'];

function route(url: URL, path: string): string {
  return new URL(path, url).toString().replaceAll('%7B', '{').replaceAll('%7D', '}');
}
