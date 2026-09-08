/**
 * @module
 * Ant adapter for Hono.
 */
export { serve, getAntServer } from './server.js';
export type { ServeOptions } from './server.js';
export { upgradeWebSocket, createWSContext } from './websocket.js';
export { getConnInfo } from './conninfo.js';
export { serveStatic } from './serve-static.js';
export type { ServeStaticOptions } from './serve-static.js';
export type {
  AntBindings,
  AntEventSourceStream,
  AntRequestIP,
  AntServeOptions,
  AntServer,
  AntUpgradeWebSocketOptions,
  AntWebSocketOptions,
  AntWebSocketUpgrade
} from './types.js';
