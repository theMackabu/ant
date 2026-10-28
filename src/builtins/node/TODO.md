# node builtins TODO

## http2.mjs

- [ ] Implement HTTP/2; `createServer` and `createSecureServer` return HTTP/1.1 servers.
- [ ] Implement `connect()`; it throws.
- [ ] Fill in the session and stream classes; they are empty.

## http.mjs

- [ ] Give `ClientRequest` a socket transport; it sends through `fetch`.
- [ ] Implement `Agent.createConnection`, `createSocket` and `addRequest`; they throw.
- [ ] Implement `OutgoingMessage`; it is an empty `EventEmitter`.
