import type { Context } from 'hono';
import type { AntServeOptions, AntServer } from './types.js';

interface AntGlobal {
  serve(options: AntServeOptions): AntServer;
}

/** Options for {@link serve}. */
export interface ServeOptions extends Omit<AntServeOptions, 'port'> {
  /** TCP port to listen on. A string such as `process.env.PORT` is accepted. */
  port?: number | string;
}

const getAnt = (): AntGlobal => {
  const ant = (globalThis as { Ant?: Partial<AntGlobal> }).Ant;
  if (!ant || typeof ant.serve !== 'function') {
    throw new TypeError('@ant/hono requires the Ant runtime: Ant.serve is not available');
  }
  return ant as AntGlobal;
};

const normalizePort = (port: number | string): number => {
  const normalized = Number(port);

  if ((typeof port === 'string' && !port.trim()) || !Number.isInteger(normalized)) {
    throw new TypeError('Port must be an integer');
  }

  if (normalized < 0 || normalized > 65535) throw new RangeError('Port must be between 0 and 65535');
  return normalized;
};

/**
 * Starts an Ant server for a Hono app and returns the server.
 *
 * ```ts
 * const server = serve({ fetch: app.fetch, port: 3000 });
 * console.log(server.url);
 * ```
 *
 * Ant passes the server to `fetch` as the second argument. Handlers can read
 * it from `c.env`. See {@link getAntServer}.
 */
export const serve = (options: ServeOptions, listeningListener?: (server: AntServer) => void): AntServer => {
  if (!options || typeof options.fetch !== 'function') {
    throw new TypeError('serve() requires a fetch function');
  }

  const serveOptions = { ...options } as AntServeOptions;
  if (options.port !== undefined) serveOptions.port = normalizePort(options.port);

  const server = getAnt().serve(serveOptions);
  listeningListener?.(server);
  return server;
};

/**
 * Returns the Ant server that received the current request.
 *
 * Ant calls `fetch(request, server)`. When you pass `app.fetch` directly,
 * `c.env` is the server. When a custom `fetch` wrapper passes an env object,
 * the function also accepts `c.env.server`.
 */
export const getAntServer = (c: Context): AntServer => {
  const env = c.env as Partial<AntServer> | { server?: Partial<AntServer> } | undefined;
  if (env && typeof (env as AntServer).upgradeWebSocket === 'function') return env as AntServer;

  const nested = (env as { server?: Partial<AntServer> } | undefined)?.server;
  if (nested && typeof nested.upgradeWebSocket === 'function') return nested as AntServer;

  throw new TypeError('env has to include the Ant server as the 2nd argument of fetch.');
};
