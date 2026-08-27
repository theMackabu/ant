# @antjs.org/wasm examples

These examples import the package's generated files from `../dist`. Published
packages already contain that directory. In a repository checkout, build it
once before running an example:

```sh
npm run build
```

## Node

Run the complete Node gallery:

```sh
npm run examples
```

Or run one use case:

```sh
npm run example:basic
npm run example:host-functions
npm run example:isolated-runtimes
npm run example:rules-engine
npm run example:limits
```

- `basic.mjs` evaluates an expression and transfers its result to Node.
- `host-functions.mjs` exposes synchronous functions and structured data.
- `isolated-runtimes.mjs` demonstrates persistent state without sharing it
  between engines.
- `rules-engine.mjs` evaluates short, user-provided rules against controlled
  input data.
- `limits-and-errors.mjs` handles guest exceptions, unsupported transfers, and
  execution timeouts.

## Browser playground

Start the local server:

```sh
npm run example:browser
```

Then open <http://127.0.0.1:4173>. The server exposes only the playground and
the three required `dist` assets, serves `ant.wasm` with `application/wasm`,
and requires no external dependencies. The playground includes editable
examples for value transfer, host functions, persistent state, and timeout
handling.
