import { setUsage } from 'upm/src/cli.ts';

import { binDir, configPath, dataPath, globalDir } from '../paths.ts';
import { CONFIG_KEYS } from '../settings.ts';

export function installUsage(): void {
  const keys = Object.entries(CONFIG_KEYS).map(([key, values]) => `  ${key.padEnd(32)} ${values.join(' | ')}`).join('\n');
  setUsage(`ant ${process.versions.ant} — package manager

Usage
  ant install [--production] [--frozen-lockfile] [--verify]    (also i)
  ant add <spec>... [-D | -O] [-E] [-g] [-w <workspace>]    (also a)
  ant remove <name>... [-g] [-w <workspace>]    (also rm)
  ant update [<name>...]    (also up)
  ant run [<script> [args...]]
  ant <script> [args...]
  ant x [--ant] [-p <spec>...] <command> [args...]    (also exec)
  ant x [-p <spec>...] -c '<command line>'
  ant trust [<name>... | --all]
  ant why <name>
  ant info <spec>
  ant ls [-g]
  ant cache info | prune
  ant config get <key>
  ant config set <key> <value>
  ant init
  ant create <template | org/repo> [dest] [args...]

Sources
  land:<spec>          from ants.land, https://npm.ants.land
  npm:<spec>           from npm, https://registry.npmjs.org
  Unprefixed specs use install.defaultRegistry, and a name ants.land does not have
  is read from npm while install.missingPackageFallback is npm. Scoped ants.land
  packages get an .npmrc scope line, unscoped ones their tarball url, so other
  package managers install them too.

Options
  -g, --global         add, remove, ls: the global packages; their bins go in
                       ${binDir()}
  -D, --dev            add: save to devDependencies
  -O, --optional       add: save to optionalDependencies
  -E, --exact          add: save an exact version for names and tags
  -a, --all            trust: every package with pending lifecycle scripts
  --ant                x: run the command's node scripts with ant
  -p, --package <spec> x: install a package for the command (repeatable)
  -c, --call <line>    x: run a shell line with -p packages on PATH
  --frozen-lockfile    install: fail if ant.lock is missing or stale
  --production         skip dev-only packages
  --verify             install: check file contents, links, bins and peers
  --offline            never use the network
  --prefer-offline     pick from kept registry documents without checking for newer ones
  --registry <url>     override the registry
  --store <dir>        package store directory
  -w, --workspace <name|path>
                       add, remove, run: select workspaces (repeatable)
  --json               print JSON
  -s, --silent         no progress, run banner or install summary
  --verbose            print debug messages
  -h, --help           show help

Notes
  Dependency lifecycle scripts run only for trustedDependencies; ant trust adds
  packages there and runs their scripts. ant.lockb is read once and replaced by
  ant.lock. update with no names resolves everything again; with names it moves
  them to their newest allowed versions.

Config
${keys}
  File: ${configPath()}

Paths
  store                ${dataPath('store')}
  global               ${globalDir()}`);
}
