# Colony

Colony is the command-line deployment tool for Ant applications hosted on
[ants.page](https://ants.page). It bundles an application locally, uploads it
through [console.antjs.org](https://console.antjs.org), and manages the hosted
project described by `colony.toml`.

Colony can deploy script-only applications or applications with static assets.
Deployments may also include environment variables, KV and SQL bindings, SQL
migrations, placement settings, and observability configuration.

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
placement = "default"

[observability]
enabled = true

[vars]
GREETING = "hello"

[[kv]]
binding = "CACHE"
id = "kv_example"

[[sql]]
binding = "DB"
id = "sql_example"
migrations_dir = "schema"

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
| `placement`                 | Deployment placement. Defaults to `default`.                                                                 |
| `observability.enabled`     | Enables observability for the deployment.                                                                    |
| `vars`                      | String values exposed to the application as environment variables.                                           |
| `kv`                        | KV resources exposed as `env.<binding>`.                                                                     |
| `sql`                       | SQL resources exposed as `env.<binding>`.                                                                    |
| `sql.migrations_dir`        | Directory of `.sql` migrations, applied in filename order.                                                   |
| `assets.directory`          | Directory of static files to upload. Defaults to `./dist`.                                                   |
| `assets.not_found_handling` | Set to `single-page-application` for SPA fallback behavior.                                                  |
| `assets.start_ant`          | Routes requests to the Ant application. Use `true` for all requests or a list of globs such as `["/api/*"]`. |

KV and SQL resources are referenced by stable IDs. An optional `name` can be
provided on a binding as resource metadata. The assets binding defaults to
`ASSETS` and can be changed with `assets.binding`.

Filesystem and subprocess modules (`fs`, `fs/promises`, and `child_process`)
are not available on ants.page and are rejected while bundling.

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
