# Ant Desktop

Desktop apps with an Ant main process and a Chromium window.

macOS 15+ on Apple silicon is the tested platform. Linux and Windows (x64 and arm64) build from the same sources but have not been run yet.

## Install

```sh
ant add -D ant-desktop
```

## Quick start

`ant-desktop.json`:

```json
{
  "main": "main.js",
  "name": "My App",
  "identifier": "com.example.my-app",
  "version": "1.0.0",
  "include": ["main.js", "renderer/**"],
  "output": "dist/My App.app"
}
```

`main.js`:

```js
import { app, BrowserWindow, ipcMain } from 'ant:desktop';

await app.ready();

ipcMain.handle('app:version', () => Ant.version);

const window = new BrowserWindow({
  width: 900,
  height: 600,
  title: 'My App',
  webPreferences: {
    capabilities: [{ channel: 'app:version', access: ['invoke'] }]
  }
});

await window.loadFile(new URL('renderer/index.html', import.meta.url).pathname);
```

`renderer/index.html`:

```html
<!doctype html>
<main></main>
<script type="module">
  const version = await Ant.ipc.invoke('app:version');
  document.querySelector('main').textContent = `Ant ${version}`;
</script>
```

Run it, then package it:

```sh
antx ant-desktop dev
antx ant-desktop package
```

## Development

`ant-desktop dev` restarts the app when an included file changes.

If the renderer needs a build step, set `renderer.buildCommand` and
`renderer.watchDir`. The command runs before launch and again on every change
under `watchDir`, and the page reloads when it succeeds.

To use a dev server instead:

```json
{
  "renderer": {
    "buildCommand": "vite build",
    "devServer": { "command": "vite", "url": "http://localhost:5173" }
  }
}
```

Ant starts the command, waits for the URL, and passes it to the app as
`ANT_DESKTOP_RENDERER_URL`:

```js
await window.loadURL(process.env.ANT_DESKTOP_RENDERER_URL);
```

DevTools are only available under `ant-desktop dev`.

## Packaging

`ant-desktop package` writes the `.app` named by `output`.

| Flag               | Effect                                  |
| ------------------ | --------------------------------------- |
| `--out <path>`     | Write somewhere else.                   |
| `--overwrite`      | Replace an existing bundle.             |
| `--bundle-runtime` | Copy Chromium into the bundle.          |
| `--icon <path>`    | Use this `.icns` file.                  |
| `--version <v>`    | Override the version.                   |

Only files matching `include` are packaged. They land in
`Contents/Resources/app`, which is `path.join(app.resourcesPath, 'app')` at run
time. `*` matches within a directory, `**` matches across directories, and
`main` must be matched by at least one pattern.

Bundles are not code signed.

## Browser runtime

Apps share one copy of Chromium per user instead of each shipping their own. It
lives in `~/Library/Application Support/Ant Desktop/CEF/<version>`.

If it is missing when an app starts, the app downloads the official CEF build
(125 MB), checks its SHA-1, and installs it. This happens once per CEF version.

- `--bundle-runtime` puts the framework inside the app, for machines that
  cannot download it. English UI resources only.
- `ANT_DESKTOP_CEF=<dir>` points at a directory containing
  `Chromium Embedded Framework.framework`.

## Configuration

Paths in `ant-desktop.json` are relative to the file.

| Field                   | Description                                        |
| ----------------------- | -------------------------------------------------- |
| `main`                  | Main-process entry. Defaults to `index.js`.        |
| `name`                  | App name.                                          |
| `identifier`            | Bundle identifier.                                 |
| `version`               | App version.                                       |
| `icon`                  | `.icns` file.                                      |
| `output`                | Where `package` writes the `.app`.                 |
| `include`               | Globs for packaged files. Defaults to none.        |
| `renderer.watchDir`     | Directory watched in dev. Defaults to `renderer`.  |
| `renderer.buildCommand` | Shell command that builds the renderer.            |
| `renderer.devServer`    | `{ command, url }`. Replaces `watchDir`.           |

## IPC

A page can only use channels its window grants. The main process enforces this.

```js
new BrowserWindow({
  webPreferences: {
    capabilities: [
      { channel: 'settings:read', access: ['invoke'] },
      { channel: 'page:ready', access: ['send'] },
      { channel: 'theme:changed', access: ['receive'] }
    ]
  }
});
```

| Access    | Page                    | Main process                |
| --------- | ----------------------- | --------------------------- |
| `invoke`  | `Ant.ipc.invoke()`      | `ipcMain.handle()`          |
| `send`    | `Ant.ipc.send()`        | `ipcMain.on()`              |
| `receive` | `Ant.ipc.on()`          | `window.webContents.send()` |

Values are structured-cloned: `Date`, `Map`, `Set`, `BigInt`, typed arrays,
errors and cyclic objects survive the trip. Functions and symbols do not.

## Preload

A preload runs before the page and can expose a small API to it:

```js
import { contextBridge, ipcRenderer } from 'ant:desktop/renderer';

contextBridge.exposeInMainWorld('settings', {
  read: () => ipcRenderer.invoke('settings:read')
});
```

Pass its path as `webPreferences.preload`.

By default the page and the preload cannot touch Ant builtins. Two options
change that:

| Option                 | Effect                                                        |
| ---------------------- | ------------------------------------------------------------- |
| `sandbox: false`       | The preload can import `node:` and `ant:` modules.            |
| `antIntegration: true` | The page gets `require` and `process`. Needs `sandbox: false`. |

`nodeIntegration` is accepted as an alias for `antIntegration`. Either way the
runtime is Ant, and only synchronous module functions can be called. Do not
enable these for content you do not control.

## API

`ant:desktop` exports `app`, `BrowserWindow`, `ipcMain`, `Menu`, `MenuItem` and
`versions`. Types are in `desktop.d.ts`.

- `app.getPath(name)` accepts `home`, `temp`, `appData`, `userData`, `desktop`,
  `documents`, `downloads`, `resources` and `exe`.
- `versions` has `ant`, `desktop` and `chrome`. The last two are also on
  `process.versions` as `ant-desktop` and `chrome`.
- `window.on()` receives `move`, `resize`, `closed`, `quit`, `loading`,
  `ready`, `title`, `navigation-start`, `navigation-commit`,
  `navigation-error`, `permission-request`, `devtools-opened` and
  `devtools-closed`.
- The app quits when its last window closes.
