import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { parse } from 'smol-toml';

export function consoleUrl(): string {
  return 'https://console.antjs.org';
}

export interface BindingDef {
  kind: 'kv' | 'sql';
  binding: string;
  id: string;
  name?: string;
  migrationsDir?: string;
}

export interface AssetsDef {
  binding: string;
  name?: string;
  directory: string;
  notFound: 'single-page-application' | 'none';
  startAnt: boolean | string[];
}

export interface ColonyConfig {
  name: string;
  main: string;
  placement: string;
  observability: boolean;
  vars: Record<string, string>;
  bindings: BindingDef[];
  assets?: AssetsDef;
}

export function findColonyToml(dir = process.cwd()): string | null {
  const configPath = join(dir, 'colony.toml');
  return existsSync(configPath) ? configPath : null;
}

type Table = Record<string, unknown>;

function table(value: unknown, key: string): Table {
  if (value === undefined) return {};
  if (typeof value !== 'object' || value === null || Array.isArray(value)) throw new Error(`\`${key}\` must be a table.`);
  return value as Table;
}

function string(value: unknown, key: string, fallback?: string): string {
  if (value === undefined && fallback !== undefined) return fallback;
  if (typeof value !== 'string' || !value) throw new Error(`\`${key}\` must be a non-empty string.`);
  return value;
}

function optionalString(value: unknown, key: string): string | undefined {
  if (value === undefined) return undefined;
  return string(value, key);
}

export function normalizeProjectName(value: string): string {
  const name = value.toLowerCase();
  if (!/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(name))
    throw new Error('project name must be a valid hostname label (letters, numbers, and interior hyphens; 63 characters maximum).');
  return name;
}

function bindings(value: unknown, kind: BindingDef['kind']): BindingDef[] {
  if (value === undefined) return [];
  if (!Array.isArray(value)) throw new Error(`\`[[${kind}]]\` must be an array of tables.`);
  return value.map((entry, index) => {
    const prefix = `${kind}[${index}]`;
    const item = table(entry, prefix);
    const binding: BindingDef = {
      kind,
      binding: string(item.binding, `${prefix}.binding`),
      id: string(item.id, `${prefix}.id`),
      name: optionalString(item.name, `${prefix}.name`)
    };
    if (kind === 'sql') binding.migrationsDir = optionalString(item.migrations_dir, `${prefix}.migrations_dir`);
    return binding;
  });
}

function startAnt(value: unknown): boolean | string[] {
  if (value === undefined) return [];
  if (typeof value === 'boolean') return value;
  const routes = typeof value === 'string' ? [value] : value;
  if (!Array.isArray(routes) || routes.some(route => typeof route !== 'string'))
    throw new Error('`assets.start_ant` must be a boolean, string, or array of strings.');
  return routes as string[];
}

export function loadColonyToml(dir = process.cwd()): ColonyConfig {
  const p = findColonyToml(dir);
  if (!p) throw new Error('no colony.toml here. Run `colony init` first.');
  let doc: Table;
  try {
    doc = parse(readFileSync(p, 'utf-8')) as Table;
  } catch (error) {
    throw new Error(`could not parse ${p}: ${error instanceof Error ? error.message : String(error)}`);
  }

  const varsTable = table(doc.vars, 'vars');
  const vars: Record<string, string> = {};
  for (const [key, value] of Object.entries(varsTable)) {
    if (!['string', 'number', 'boolean'].includes(typeof value)) throw new Error(`\`vars.${key}\` must be a scalar value.`);
    vars[key] = String(value);
  }

  const allBindings = [...bindings(doc.kv, 'kv'), ...bindings(doc.sql, 'sql')];
  const names = new Set<string>();
  for (const binding of allBindings) {
    if (names.has(binding.binding)) throw new Error(`duplicate binding: ${binding.binding}`);
    names.add(binding.binding);
  }

  let assets: AssetsDef | undefined;
  if (doc.assets !== undefined) {
    const a = table(doc.assets, 'assets');
    const notFound = a.not_found_handling ?? 'none';
    if (notFound !== 'none' && notFound !== 'single-page-application')
      throw new Error('`assets.not_found_handling` must be `none` or `single-page-application`.');
    assets = {
      binding: string(a.binding, 'assets.binding', 'ASSETS'),
      name: optionalString(a.name, 'assets.name'),
      directory: string(a.directory, 'assets.directory', './dist'),
      notFound,
      startAnt: startAnt(a.start_ant)
    };
  }

  const observability = table(doc.observability, 'observability');
  if (observability.enabled !== undefined && typeof observability.enabled !== 'boolean')
    throw new Error('`observability.enabled` must be a boolean.');

  return {
    name: normalizeProjectName(string(doc.name, 'name')),
    main: string(doc.main ?? doc.entry, 'main', 'server.js'),
    placement: string(doc.placement, 'placement', 'default'),
    observability: observability.enabled === true,
    vars,
    bindings: allBindings,
    assets
  };
}
