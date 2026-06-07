import { antTargets } from './targets';

export function routeIndex(url: URL) {
  return {
    schema: 1,
    routes: {
      latest: route(url, '/v1/latest'),
      refresh: route(url, '/v1/refresh?branch={branch}&run_id={run_id}&revision={full_git_hash}'),
      version: {
        get_tag: route(url, '/v1/version/get-tag'),
        manifest: route(url, '/v1/version/{version}'),
        check: route(url, '/v1/version?target={target}&current={version}'),
      },
      check: route(url, '/v1/check?target={target}&current={version}'),
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
          `/v1/version?target=${encodeURIComponent(target.key)}&current={version}`,
        ),
      })),
      sandbox: arches.map(arch => ({
        name: `sandbox-${arch}`,
        arch,
        url: route(url, `/v1/download/sandbox/${arch}`),
      })),
      kernel: arches.map(arch => ({
        name: `kernel-${arch}`,
        arch,
        url: route(url, `/v1/download/kernel/${arch}`),
      })),
    },
    arches: ['x64', 'aarch64'],
  };
}

const arches = ['x64', 'aarch64'];

function route(url: URL, path: string): string {
  return new URL(path, url).toString().replaceAll('%7B', '{').replaceAll('%7D', '}');
}
