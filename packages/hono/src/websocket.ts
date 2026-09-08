import type { UpgradeWebSocket, WSMessageReceive } from 'hono/ws';
import { WSContext, createWSMessageEvent, defineWebSocketHelper } from 'hono/ws';
import { getAntServer } from './server.js';
import type { AntUpgradeWebSocketOptions } from './types.js';

const toMessageData = (data: unknown): WSMessageReceive => {
  if (typeof data === 'string' || data instanceof ArrayBuffer || data instanceof Blob) return data;
  if (ArrayBuffer.isView(data)) return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
  return String(data);
};

/**
 * Wraps a native Ant WebSocket in a Hono `WSContext`.
 *
 * Event handlers receive this context as their second argument. The package
 * exports the function for adapter internals and tests.
 */
export const createWSContext = (socket: WebSocket, url?: string | URL | null): WSContext<WebSocket> =>
  new WSContext<WebSocket>({
    close: (code, reason) => socket.close(code, reason),
    get protocol() {
      return socket.protocol || null;
    },
    raw: socket,
    get readyState() {
      return socket.readyState as 0 | 1 | 2 | 3;
    },
    url: url ? new URL(url) : socket.url ? new URL(socket.url) : null,
    send: source => socket.send(source)
  });

/**
 * Hono `upgradeWebSocket` middleware. It uses Ant's `server.upgradeWebSocket()`.
 *
 * By default, the middleware accepts the first subprotocol in the client's
 * `Sec-WebSocket-Protocol` header. Pass `{ protocol }` as the second argument
 * to select a different subprotocol.
 *
 * ```ts
 * app.get('/ws', upgradeWebSocket(c => ({
 *   onMessage(event, ws) { ws.send(event.data) }
 * })));
 * ```
 */
export const upgradeWebSocket: UpgradeWebSocket<WebSocket, AntUpgradeWebSocketOptions> = defineWebSocketHelper<
  WebSocket,
  AntUpgradeWebSocketOptions
>((c, events, options) => {
  if (c.req.header('upgrade')?.toLowerCase() !== 'websocket') return;

  const server = getAntServer(c);
  const protocol = options?.protocol ?? c.req.header('sec-websocket-protocol')?.split(',')[0]?.trim();
  const { socket, response } = server.upgradeWebSocket(c.req.raw, protocol ? { protocol } : undefined);

  const ws = createWSContext(socket, c.req.url);
  socket.addEventListener('open', event => events.onOpen?.(event, ws));
  socket.addEventListener('message', event => {
    events.onMessage?.(createWSMessageEvent(toMessageData(event.data)), ws);
  });
  socket.addEventListener('close', event => events.onClose?.(event as CloseEvent, ws));
  socket.addEventListener('error', event => events.onError?.(event, ws));

  return response;
});
