import { Expr, call, lambda, letIn, path, ref, render } from './expression.ts';
import type { Attrs, Value } from './expression.ts';

let nextBinding = 0;
let buildDepth = 0;
function building<T>(build: () => T): T {
  if (buildDepth === 0) nextBinding = 0;
  buildDepth++;
  try { return build(); } finally { buildDepth--; }
}
function fresh(hint: string): string {
  ref(hint);
  if (hint.startsWith('__tsnix_')) throw new TypeError('The __tsnix_ prefix is reserved for generated bindings');
  return `__tsnix_${nextBinding++}_${hint}`;
}
type Handles<Names extends string> = { readonly [Name in Names]: Expr };

export function fn<const Names extends string | readonly string[]>(
  names: Names,
  body: (args: Names extends string ? Expr : Handles<Names[number]>) => Value,
  defaults: Partial<Record<Names[number], Value>> = {},
): Expr {
  return building(() => {
    if (typeof names === 'string') {
      const name = fresh(names);
      return lambda(name, (body as (arg: Expr) => Value)(ref(name)), defaults as Attrs);
    }
    if (new Set(names).size !== names.length) throw new TypeError('Duplicate function argument');
    const aliases: Record<string, Value> = {};
    const handles = Object.fromEntries(names.map(name => {
      const alias = fresh(name);
      aliases[alias] = ref(name);
      return [name, ref(alias)];
    }));
    return lambda(names, letIn(aliases, (body as (args: Handles<string>) => Value)(handles)), defaults as Attrs);
  });
}

export class Scope {
  private bindings: Record<string, Value> = {};
  private closed = false;
  bind(hint: string, value: Value): Expr {
    if (this.closed) throw new Error('Scope is already closed');
    const name = fresh(hint);
    this.bindings[name] = value;
    return ref(name);
  }
  finish(body: Value): Expr {
    if (this.closed) throw new Error('Scope is already closed');
    this.closed = true;
    return letIn(this.bindings, body);
  }
}
export function scope(build: (scope: Scope) => Value): Expr {
  return building(() => {
    const context = new Scope();
    return context.finish(build(context));
  });
}

/** Immutable literal attribute sets; set() returns a new value. */
export class AttrSet extends Expr {
  private readonly attrs: Attrs;
  constructor(attrs: Attrs = {}) {
    super(render(attrs).trimEnd());
    this.attrs = { ...attrs };
  }
  set(name: string | Expr, value: Value): Expr {
    if (name instanceof Expr) return super.set(name, value);
    return new AttrSet({ ...this.attrs, [name]: value });
  }
}

export class NixFunction<const Names extends string | readonly string[]> extends Expr {
  constructor(
    names: Names,
    body: (args: Names extends string ? Expr : Handles<Names[number]>) => Value,
    defaults: Partial<Record<Names[number], Value>> = {},
  ) {
    super(fn(names, body, defaults).source);
  }
}

export class Let extends Expr {
  constructor(build: (scope: Scope) => Value) { super(scope(build).source); }
}

export class Import extends Expr {
  constructor(file: string | Expr, ...args: Value[]) {
    super(call(ref('import'), typeof file === 'string' ? path(file) : file, ...args).source);
  }
}

export class EachSystem extends Expr {
  constructor(utils: Expr, build: (system: Expr) => Value) {
    super(utils.get('lib', 'eachDefaultSystem').call(fn('system', build)).source);
  }
}

/** A named flake output whose expression can also be used by other builders. */
export class Package extends Expr {
  readonly kind = 'packages';
  readonly name: string;
  readonly isDefault: boolean;
  constructor(name: string, value: Value, isDefault = false) {
    super(render(value).trimEnd());
    this.name = name;
    this.isDefault = isDefault;
  }
  asDefault(): Package { return new Package(this.name, this, true); }
}

export class DevShell extends Expr {
  readonly kind = 'devShells';
  readonly name: string;
  readonly isDefault: boolean;
  constructor(name: string, value: Value, isDefault = false) {
    super(render(value).trimEnd());
    this.name = name;
    this.isDefault = isDefault;
  }
  asDefault(): DevShell { return new DevShell(this.name, this, true); }
}

type Output = Package | DevShell;
function outputAttrs(resources: readonly Output[]): Attrs {
  const groups: Record<string, Record<string, Value>> = {};
  for (const resource of resources) {
    const group = groups[resource.kind] ??= Object.create(null);
    const names = resource.isDefault && resource.name !== 'default' ? [resource.name, 'default'] : [resource.name];
    for (const name of names) {
      if (Object.hasOwn(group, name)) throw new Error(`Duplicate ${resource.kind} output: ${name}`);
      group[name] = resource;
    }
  }
  return groups;
}

export class Outputs extends Expr {
  private readonly resources: readonly Output[];
  constructor(...resources: Output[]) {
    super(render(outputAttrs(resources)).trimEnd());
    this.resources = resources;
  }
  add(...resources: Output[]): Outputs { return new Outputs(...this.resources, ...resources); }
}

/** A mkShell expression; wrap it in a DevShell to give it a flake output name. */
export class Shell extends Expr {
  private readonly pkgs: Expr;
  private readonly attrs: Attrs;
  constructor(pkgs: Expr, attrs: Attrs = {}) {
    super(render(pkgs.get('mkShell').call(attrs)).trimEnd());
    this.pkgs = pkgs;
    this.attrs = { ...attrs };
  }
  packages(...packages: Value[]): Shell {
    const previous = this.attrs.packages ?? [];
    const combined = Array.isArray(previous) ? [...previous, ...packages] : new Expr(render(previous).trimEnd()).concat(packages);
    return new Shell(this.pkgs, { ...this.attrs, packages: combined });
  }
  env(values: Readonly<Record<string, string | Expr>>): Shell {
    const previous = this.attrs.env ?? {};
    return new Shell(this.pkgs, { ...this.attrs, env: new Expr(render(previous).trimEnd()).merge(values) });
  }
  hook(script: string): Shell {
    const previous = this.attrs.shellHook;
    if (previous !== undefined && typeof previous !== 'string') throw new TypeError('Shell.hook requires a literal shellHook');
    return new Shell(this.pkgs, { ...this.attrs, shellHook: previous === undefined ? script : previous + '\n' + script });
  }
}

export class Input<const Name extends string> {
  readonly name: Name;
  readonly url: string;
  private readonly dependencies: Readonly<Record<string, string>>;
  constructor(name: Name, url: string, dependencies: Readonly<Record<string, string>> = {}) {
    ref(name);
    if (name === 'self') throw new Error('Reserved flake input: self');
    this.name = name;
    this.url = url;
    this.dependencies = { ...dependencies };
  }
  follows(name: string, target: Input<string> | string): Input<Name> {
    return new Input(this.name, this.url, { ...this.dependencies, [name]: typeof target === 'string' ? target : target.name });
  }
  toAttrs(): Attrs {
    return {
      url: this.url,
      ...(Object.keys(this.dependencies).length ? {
        inputs: Object.fromEntries(Object.entries(this.dependencies).map(([name, follows]) => [name, { follows }])),
      } : {}),
    };
  }
}

export class BinaryCache {
  readonly url: string;
  readonly publicKey: string;
  constructor(url: string, publicKey: string) {
    this.url = url;
    this.publicKey = publicKey;
  }
}

export class Flake<Names extends string = never> {
  private readonly description: string;
  private readonly definitions: readonly Input<Names>[];
  private readonly caches: readonly BinaryCache[];
  constructor(description: string, inputs: readonly Input<Names>[] = [], caches: readonly BinaryCache[] = []) {
    const names = inputs.map(input => input.name);
    if (new Set(names).size !== names.length) throw new Error('Duplicate flake input');
    this.description = description;
    this.definitions = [...inputs];
    this.caches = [...caches];
  }
  inputs<const Added extends readonly Input<string>[]>(...inputs: Added): Flake<Names | Added[number]['name']> {
    return new Flake<Names | Added[number]['name']>(this.description, [...this.definitions, ...inputs], this.caches);
  }
  cache(...caches: BinaryCache[]): Flake<Names> {
    return new Flake(this.description, this.definitions, [...this.caches, ...caches]);
  }
  outputs(build: (inputs: Handles<Names | 'self'>) => Value): Expr {
    const names = this.definitions.map(input => input.name);
    return this.definition(fn(['self', ...names], build as (inputs: Handles<string>) => Value));
  }
  outputsFrom(file: string | Expr): Expr {
    return this.outputs(inputs => new Import(file).get('outputs').call(inputs));
  }
  private definition(outputs: Expr): Expr {
    const inputs = Object.fromEntries(this.definitions.map(input => [input.name, input.toAttrs()]));
    return new Expr(render({
      description: this.description,
      ...(this.caches.length ? { nixConfig: {
        'extra-substituters': this.caches.map(cache => cache.url),
        'extra-trusted-public-keys': this.caches.map(cache => cache.publicKey),
      } } : {}),
      inputs,
      outputs,
    }).trimEnd());
  }
}
