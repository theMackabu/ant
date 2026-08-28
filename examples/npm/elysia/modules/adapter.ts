import type { ElysiaAdapter } from 'elysia/adapter';
import { WebStandardAdapter } from 'elysia/adapter/web-standard';

const normalizePort = (port: string | number) => {
  const normalized = Number(port);

  if ((typeof port === 'string' && !port.trim()) || !Number.isInteger(normalized)) {
    throw new TypeError('Port must be an integer');
  }

  if (normalized < 0 || normalized > 65535) throw new RangeError('Port must be between 0 and 65535');

  return normalized;
};

export const AntAdapter: ElysiaAdapter = {
  ...WebStandardAdapter,
  name: 'ant',
  listen: app => (options, callback) => {
    app.compile();

    const serveOptions = {
      ...app.config.serve,
      ...(typeof options === 'object' ? options : { port: options }),
      fetch: app.fetch
    };

    if (serveOptions.port !== undefined) serveOptions.port = normalizePort(serveOptions.port);
    app.server = Ant.serve(serveOptions) as typeof app.server;

    for (const hook of app.event.start ?? []) hook.fn(app);
    callback?.(app.server);

    if (app.modules.size) void app.modules.then(() => app.compile());
  }
};
