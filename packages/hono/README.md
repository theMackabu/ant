[Hono](https://hono.dev) adapter for the Ant runtime: start a server with
`Ant.serve`, upgrade WebSockets, read connection info, and serve static files.

## Usage

```js
import { Hono } from 'hono';
import { upgradeWebSocket, getConnInfo, serveStatic } from '@ant/hono';

const app = new Hono();

app.get('/', c => c.text(`hello from ${getConnInfo(c).remote.address}`));
app.use('/static/*', serveStatic({ root: './public' }));

app.get(
  '/ws',
  upgradeWebSocket(() => ({
    onOpen(event, ws) {
      ws.send('connected');
    },
    onMessage(event, ws) {
      ws.send(event.data);
    },
    onClose() {
      console.log('closed');
    }
  }))
);

export default app;
```

Exporting `{ fetch: app.fetch, port }` as the module's default export also
works; Ant starts the server from the default export automatically.

## API

Starts an Ant server for a Hono app and returns it. Accepts every
`Ant.serve` option; `port` may be a string such as `process.env.PORT`.

<Symbol name="serve"/>

The middleware that upgrades a request to a WebSocket. The subprotocol
defaults to the first entry the client offered; pass `{ protocol }` as the
second argument to choose explicitly.

<Symbol name="upgradeWebSocket"/>

Returns the remote address and port of the current request via
`server.requestIP`.

<Symbol name="getConnInfo"/>

Serves files from disk. Accepts Hono's usual `root`, `path`, `rewriteRequestPath`,
`onFound`, and `onNotFound` options.

<Symbol name="serveStatic"/>

Returns the Ant server handling the current request. Ant passes the server as
the second argument to `fetch`, so it is available as `c.env` when `app.fetch`
is passed directly, or as `c.env.server` when wrapped.

<Symbol name="getAntServer"/>

Wraps a native Ant socket in a Hono `WSContext` for adapter internals and tests.

<Symbol name="createWSContext"/>

The Ant server type, also available as `c.env`.

<Symbol name="AntServer"/>
