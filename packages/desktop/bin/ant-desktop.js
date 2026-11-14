#!/usr/bin/env ant

import fs from 'node:fs';
import path from 'node:path';
import * as desktop from '../../index.js';
import { CONFIG_NAME, loadConfig, optionsFromConfig } from '../../config.js';

const { version } = JSON.parse(fs.readFileSync(new URL('../../package.json', import.meta.url), 'utf8'));

function usage() {
  process.stdout.write(`Usage:
  ant-desktop <main.js> [args...]
  ant-desktop dev [main.js] [options]
  ant-desktop package [main.js] [options]

Project manifest:
  ${CONFIG_NAME} supplies main, renderer, include, name, identifier, version, icon, and output.

Options:
  --config <path>           Project manifest path (default: ${CONFIG_NAME})
  --name <name>             Application display name
  --app-dir <path>          Application source root
  --identifier <id>         Application identifier

Dev options:
  --renderer-dir <path>     Renderer root watched for hot reload

Package options:
  --out <path>              Output artifact path
  --version <version>       Application version
  --icon <path>             Platform icon file (.icns on macOS)
  --overwrite               Replace an existing output artifact
  --bundle-runtime          Copy the browser runtime into the app instead of sharing it
`);
}

function commandArguments(command, argv) {
  const options = {};
  let configPath;
  let entry;
  for (let index = 0; index < argv.length; index++) {
    const argument = argv[index];
    if (argument === '--help' || argument === '-h') return { help: true };
    if (!argument.startsWith('-') && !entry) {
      entry = argument;
      continue;
    }
    if (command === 'package' && (argument === '--overwrite' || argument === '--bundle-runtime')) {
      options[argument === '--overwrite' ? 'overwrite' : 'bundleRuntime'] = true;
      continue;
    }
    const separator = argument.indexOf('=');
    const flag = separator === -1 ? argument : argument.slice(0, separator);
    const inline = separator === -1 ? undefined : argument.slice(separator + 1);
    const common = {
      '--app-dir': 'appDir',
      '--identifier': 'identifier',
      '--name': 'name'
    };
    const commandOptions = command === 'dev' ? { '--renderer-dir': 'rendererDir' } : { '--icon': 'icon', '--out': 'out', '--version': 'version' };
    const name = { ...common, ...commandOptions }[flag];
    if (flag !== '--config' && !name) {
      throw new Error(`Unknown ${command} option: ${argument}`);
    }
    const value = inline === undefined ? argv[++index] : inline;
    if (!value) throw new Error(`${flag} requires a value`);
    if (flag === '--config') configPath = value;
    else options[name] = value;
  }
  return { configPath, entry, options };
}

function commandRequest(command, argv) {
  const request = commandArguments(command, argv);
  if (request.help) return request;
  const configPath = request.configPath || (request.options.appDir && path.join(request.options.appDir, CONFIG_NAME));
  const config = loadConfig(configPath, Boolean(request.configPath));
  const entry = request.entry ? path.resolve(request.entry) : config?.main;
  if (!entry) {
    throw new Error(`${command} requires a main file or ${CONFIG_NAME}`);
  }
  return {
    entry,
    options: { ...optionsFromConfig(config, command), ...request.options }
  };
}

async function main() {
  const argv = process.argv.slice(2);
  if (argv.length === 0 || argv[0] === '--help' || argv[0] === '-h') {
    usage();
    process.exitCode = argv.length === 0 ? 64 : 0;
    return;
  }
  if (argv[0] === '--version' || argv[0] === '-v') {
    process.stdout.write(`${version}\n`);
    return;
  }
  if (argv[0] === 'package') {
    const request = commandRequest('package', argv.slice(1));
    if (request.help) return usage();
    const result = await desktop.packageApp(request.entry, request.options);
    process.stdout.write(`${result.path || result}\n`);
    return;
  }
  if (argv[0] === 'dev') {
    const request = commandRequest('dev', argv.slice(1));
    if (request.help) return usage();
    await desktop.dev(request.entry, request.options);
    return;
  }

  const result = desktop.runSync(argv[0], argv.slice(1));
  if (result.error) throw result.error;
  process.exitCode = result.status === null ? 1 : result.status;
}

try {
  await main();
} catch (error) {
  process.stderr.write(`ant-desktop: ${error.message}\n`);
  process.exitCode = 1;
}
