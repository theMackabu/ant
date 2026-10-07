# Colony

Colony is the command-line deployment tool for Ant applications hosted on
[ants.page](https://ants.page). It bundles an application locally, uploads it
through [console.antjs.org](https://console.antjs.org), and manages the hosted
project described by `colony.toml`.

Colony can deploy script-only applications or applications with static assets.
Deployments may also include environment variables, KV and SQL bindings, SQL
migrations, and observability configuration.

## Quick start

Authorize the CLI, initialize a project, and deploy it:

```sh
colony login
colony init my-app
colony deploy
```

`colony init` creates a `colony.toml` in the current directory. By default,
Colony bundles `server.js` and deploys the application to
`https://my-app.ants.page`.

If the project has package dependencies but no `node_modules` directory,
`colony deploy` installs them with Antland before building. The application is
bundled with Rolldown, then uploaded along with any configured assets and
migrations.

## Configuration

A complete `colony.toml` can look like this:

```toml
name = "my-app"
main = "server.js"

[observability]
enabled = true

[vars]
GREETING = "hello"

[[kv]]
binding = "CACHE"
name = "cache"

[[sql]]
binding = "DB"
name = "app-db"
migrations_dir = "schema"

[[files]]
binding = "FILES"

[assets]
directory = "./dist"
not_found_handling = "single-page-application"
start_ant = ["/api/*"]
```

The main settings are:

| Setting                     | Description                                                                                                  |
| --------------------------- | ------------------------------------------------------------------------------------------------------------ |
| `name`                      | Project name and the `<name>.ants.page` hostname. Required.                                                  |
| `main`                      | Application entrypoint. Defaults to `server.js`.                                                             |
| `observability.enabled`     | Enables observability for the deployment.                                                                    |
| `vars`                      | String values exposed to the application as environment variables.                                           |
| `kv`                        | KV resources exposed as `env.<binding>`.                                                                     |
| `sql`                       | SQL resources exposed as `env.<binding>`.                                                                    |
| `sql.migrations_dir`        | Directory of `.sql` migrations, applied in filename order.                                                   |
| `files`                     | Your account's Files (object storage, 256 MB) exposed as `env.<binding>`. Takes no `name`.                   |
| `assets.directory`          | Directory of static files to upload. Defaults to `./dist`.                                                   |
| `assets.not_found_handling` | Set to `single-page-application` for SPA fallback behavior.                                                  |
| `assets.start_ant`          | Routes requests to the Ant application. Use `true` for all requests or a list of globs such as `["/api/*"]`. |

A binding exposes one of your stores to the application as `env.<binding>`.
Stores are named with `name` (defaulting to the binding in lowercase), belong
to your account, and are created on the first deploy that uses them. Projects
that bind the same `name` share the store. Names use lowercase letters,
numbers, `-` and `_`.

## Static sites

A project with `[assets]` and no script (no `main`, and no `server.js` next to
`colony.toml`) deploys as a static site, like Cloudflare Pages: the files are
served straight from the edge node and no code runs, so there are no cold
starts. `/about` serves `about.html`, `/blog/` serves `blog/index.html`, and a
`404.html` (if you have one) is the not-found page; with
`not_found_handling = "single-page-application"`, unknown paths get
`index.html` instead.

```toml
name = "my-site"

[assets]
directory = "./dist"
```

Files is one store per account, shared by every project that binds it, with
an R2-style API: `put(key, body, { httpMetadata: { contentType } })`,
`get(key)` (with `.text()`, `.json()`, `.arrayBuffer()`, `.body`; `null` if
absent; `{ range: { offset, length } }` for part of a file), `head(key)`,
`list({ prefix, delimiter, cursor, limit })` and `delete(key | keys)`. You can
browse, upload and download them in the console under Storage → Files.

## Commands

| Command                | Description                                                                                |
| ---------------------- | ------------------------------------------------------------------------------------------ |
| `colony login`         | Authorize the device and save a deployment token.                                          |
| `colony logout`        | Remove the saved deployment token.                                                         |
| `colony whoami`        | Show the current account.                                                                  |
| `colony init [name]`   | Create `colony.toml` in the current directory.                                             |
| `colony deploy`        | Build and deploy the current project.                                                      |
| `colony list`          | List projects. Alias: `ls`.                                                                |
| `colony delete [name]` | Delete a project. Alias: `rm`. If no name is given, Colony uses the current `colony.toml`. |

Interactive login stores credentials in `~/.colony/config.json`. For CI and
other non-interactive environments, set `COLONY_TOKEN` instead:

```sh
COLONY_TOKEN="$COLONY_TOKEN" colony deploy
```
