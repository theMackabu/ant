/** WebSocket settings for connections that the server upgrades. */
export interface AntWebSocketOptions {
  /** Idle timeout for a WebSocket, in seconds. The default is 120. Set 0 to disable the timeout. */
  idleTimeout?: number;
  /** Maximum size of an incoming WebSocket message, in bytes. The default is 16 MiB. */
  maxPayloadLength?: number;
  /** Enables permessage-deflate compression for clients that request it. */
  perMessageDeflate?: boolean | object;
}

/** Options for `Ant.serve()`. */
export interface AntServeOptions {
  /** Handles one request. Ant passes the server as the second argument. */
  fetch(request: Request, server: AntServer): Response | Promise<Response>;
  /** TCP port to listen on. Set 0 to let Ant select a free port. */
  port?: number;
  /** Hostname or IP address to bind. */
  hostname?: string;
  /** Path of a Unix domain socket. When set, Ant uses the socket instead of TCP. */
  unix?: string;
  /** Idle timeout for a connection, in seconds. */
  idleTimeout?: number;
  /** Timeout for one request, in seconds. */
  requestTimeout?: number;
  /** WebSocket settings for upgraded connections. */
  websocket?: AntWebSocketOptions;
  /** TLS settings. */
  tls?: unknown;
}

/** Address and port of the remote peer. `server.requestIP()` returns this. */
export interface AntRequestIP {
  address: string;
  port: number;
}

/** Options for `server.upgradeWebSocket()`. */
export interface AntUpgradeWebSocketOptions {
  /** Subprotocol to accept. The client must offer it in the `Sec-WebSocket-Protocol` header. */
  protocol?: string;
}

/** Result of `server.upgradeWebSocket()`. */
export interface AntWebSocketUpgrade {
  /** The server side of the WebSocket. */
  socket: WebSocket;
  /** The 101 response. Return it from the request handler. */
  response: Response;
}

/** A server-sent events stream. `server.eventSource()` returns this. */
export interface AntEventSourceStream {
  /** The streaming response. Return it from the request handler. */
  response: Response;
  /** Sends one event with the given data. */
  send(data: string): void;
  /** Sends one comment line. */
  comment(text: string): void;
  /** Ends the stream. */
  close(): void;
}

/** The running Ant server. Ant also passes it to `fetch` as the second argument. */
export interface AntServer {
  /** The bound hostname or IP address. */
  hostname: string;
  /** The bound TCP port. */
  port: number;
  /** The base URL of the server, when it listens on TCP. */
  url?: string;
  /** The Unix domain socket path, when it listens on a socket. */
  unix?: string;
  /** Returns the remote address of a request, or null when it is not known. */
  requestIP(request: Request): AntRequestIP | null;
  /** Sets the timeout of one request, in seconds. */
  timeout(request: Request, seconds: number): void;
  /** Stops the server. Set `force` to true to close open connections. */
  stop(force?: boolean): Promise<void>;
  /** Replaces the fetch handler. */
  reload(options: { fetch: AntServeOptions['fetch'] }): void;
  /** Upgrades a request to a WebSocket. */
  upgradeWebSocket(request: Request, options?: AntUpgradeWebSocketOptions): AntWebSocketUpgrade;
  /** Creates a server-sent events stream. */
  eventSource(): AntEventSourceStream;
}

/** Hono bindings for a custom `fetch` wrapper that passes `{ server }` as `env`. */
export interface AntBindings {
  server: AntServer;
}
