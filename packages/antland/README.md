# antland

The CLI for [ants.land](https://ants.land), the package registry for the Ant
runtime. Works with npm, yarn, pnpm, and bun.

```sh
npx antland login        # authorize this device
npx antland add thing    # install a package
npx antland publish      # publish the current package
```

## Commands

| Command               | Description                  |
| --------------------- | ---------------------------- |
| `add`, `i`, `install` | Install one or more packages |
| `remove`, `r`         | Remove one or more packages  |
| `publish`             | Publish the current package  |
| `login`, `logout`     | Manage your publish token    |
| `info`                | Show package information     |
| `run <script>`        | Run a `package.json` script  |

Pass `--npm`, `--yarn`, `--pnpm`, or `--bun` to force a package manager, and
`-D` or `-O` to save to dev or optional dependencies. Set `ANTS_REGISTRY` to use
a different registry.

Full documentation: https://ants.land/docs/cli
