import { readFile, stat } from 'node:fs/promises';
import { join } from 'node:path';
import type { Env, MiddlewareHandler } from 'hono';
import type { ServeStaticOptions } from 'hono/serve-static';
import { serveStatic as baseServeStatic } from 'hono/serve-static';

export type { ServeStaticOptions };

/**
 * Serves files from disk. It reads the files with Ant's `fs` module.
 *
 * ```ts
 * app.use('/static/*', serveStatic({ root: './public' }));
 * ```
 */
export const serveStatic = <E extends Env = Env>(options: ServeStaticOptions<E> = {}): MiddlewareHandler => {
  return async function serveStatic(c, next) {
    const getContent = async (path: string) => {
      try {
        return (await readFile(path)) as Uint8Array<ArrayBuffer>;
      } catch {
        return null;
      }
    };

    const isDir = async (path: string) => {
      try {
        return (await stat(path)).isDirectory();
      } catch {
        return undefined;
      }
    };

    return baseServeStatic({ ...options, getContent, join, isDir })(c, next);
  };
};
