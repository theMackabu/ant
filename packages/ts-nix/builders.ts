import { Expr, boundFunction, boundLet, call, path, ref, symbol } from './expression.ts';
import type { Attrs, Value } from './expression.ts';

type Handles<Names extends string> = { readonly [Name in Names]: Expr };

export function fn<const Names extends string | readonly string[]>(
  names: Names,
  body: (args: Names extends string ? Expr : Handles<Names[number]>) => Value,
  defaults: Partial<Record<Names[number], Value>> = {},
): Expr {
  if (typeof names === 'string') {
    const handle = symbol(names);
    return boundFunction(names, [handle], (body as (arg: Expr) => Value)(handle), defaults as Attrs);
  }
  if (new Set(names).size !== names.length) throw new TypeError('Duplicate function argument');
  const handles = names.map(symbol);
  const args = Object.fromEntries(names.map((name, i) => [name, handles[i]]));
  return boundFunction(names, handles, (body as (args: Handles<string>) => Value)(args), defaults as Attrs);
}

export class Scope {
  private bindings: [Expr, Value][] = [];
  private closed = false;
  bind(hint: string, value: Value): Expr {
    if (this.closed) throw new Error('Scope is already closed');
    const handle = symbol(hint);
    this.bindings.push([handle, Expr.value(value)]);
    return handle;
  }
  finish(body: Value): Expr {
    if (this.closed) throw new Error('Scope is already closed');
    this.closed = true;
    return boundLet(this.bindings, body);
  }
}
export function scope(build: (scope: Scope) => Value): Expr {
  const context = new Scope();
  return context.finish(build(context));
}

/** Immutable literal attribute sets; set() returns a new value. */
export class AttrSet extends Expr {
  private readonly attrs: Attrs;
  constructor(attrs: Attrs = {}) {
    super(Expr.value(attrs));
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
    super(fn(names, body, defaults));
  }
}

export class Let extends Expr {
  constructor(build: (scope: Scope) => Value) { super(scope(build)); }
}

export class Import extends Expr {
  constructor(file: string | Expr, ...args: Value[]) {
    super(call(ref('import'), typeof file === 'string' ? path(file) : file, ...args));
  }
}

export class EachSystem extends Expr {
  constructor(utils: Expr, build: (system: Expr) => Value) {
    super(utils.get('lib', 'eachDefaultSystem').call(fn('system', build)));
  }
}

/** A named flake output whose expression can also be used by other builders. */
export class Package extends Expr {
  readonly kind = 'packages';
  readonly name: string;
  readonly isDefault: boolean;
  constructor(name: string, value: Value, isDefault = false) {
    super(Expr.value(value));
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
    super(Expr.value(value));
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
    super(Expr.value(outputAttrs(resources)));
    this.resources = resources;
  }
  add(...resources: Output[]): Outputs { return new Outputs(...this.resources, ...resources); }
}

/** A mkShell expression; wrap it in a DevShell to give it a flake output name. */
export class Shell extends Expr {
  private readonly pkgs: Expr;
  private readonly attrs: Attrs;
  constructor(pkgs: Expr, attrs: Attrs = {}) {
    super(pkgs.get('mkShell').call(attrs));
    this.pkgs = pkgs;
    this.attrs = { ...attrs };
  }
  packages(...packages: Value[]): Shell {
    const previous = this.attrs.packages ?? [];
    const combined = Array.isArray(previous) ? [...previous, ...packages] : Expr.value(previous).concat(packages);
    return new Shell(this.pkgs, { ...this.attrs, packages: combined });
  }
  env(values: Readonly<Record<string, string | Expr>>): Shell {
    const previous = this.attrs.env ?? {};
    return new Shell(this.pkgs, { ...this.attrs, env: Expr.value(previous).merge(values) });
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
    return Expr.value({
      description: this.description,
      ...(this.caches.length ? { nixConfig: {
        'extra-substituters': this.caches.map(cache => cache.url),
        'extra-trusted-public-keys': this.caches.map(cache => cache.publicKey),
      } } : {}),
      inputs,
      outputs,
    });
  }
}
