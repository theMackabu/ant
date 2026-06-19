[Hono](https://hono.dev) WebSockets helpers for Ant's native server runtime.

## Usage

```js
import { Hono } from 'hono';
import { upgradeWebSocket } from '@ant/hono';

const app = new Hono();

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

## API

The middleware that upgrades a request to a WebSocket.

<Symbol name="upgradeWebSocket"/>

The lifecycle handlers returned by `createEvents`.

<Symbol name="WSEvents"/>

Hono's per-socket API, passed as the second argument to each handler.

<Symbol name="WSContext"/>

Wraps a native Ant socket in a `WSContext` for adapter internals and tests.

<Symbol name="createWSContext"/>

The upgrade is backed by Ant's native `server.upgradeWebSocket(request)`. Ant
exposes the server context as the second argument to `fetch`, so the Hono app
must forward it via `c.env`. `app.fetch` does this by default.
