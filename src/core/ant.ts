Ant.version = import.meta.env.VERSION;
Ant.target = import.meta.env.TARGET;
Ant.revision = import.meta.env.GIT_HASH;
Ant.buildDate = import.meta.env.BUILD_TIMESTAMP;
Ant.host = import.meta.env.HOST as AntHost;

Ant.typeof = function (t) {
  const value = Ant.raw.typeof(t);

  const types = {
    kTypeObject: 'object',
    kTypeString: 'string',
    kTypeArray: 'array',
    kTypeFunction: 'function',
    kTypeBuiltin: 'cfunc',
    kTypePromise: 'promise',
    kTypeGenerator: 'generator',
    kTypeUndefined: 'undefined',
    kTypeNull: 'null',
    kTypeBool: 'boolean',
    kTypeNumber: 'number',
    kTypeBigInt: 'bigint',
    kTypeSymbol: 'symbol',
    kTypeTypedArray: 'typedarray',
    kTypeError: 'err',
    kTypeFunctionInfo: 'functioninfo',
    kTypeMap: 'map',
    kTypeSet: 'set',
    kTypeWeakMap: 'weakmap',
    kTypeWeakSet: 'weakset',
    kTypeSourceCode: 'sourcecode'
  } as const;

  const names = Object.values(types);
  return value < names.length ? names[value] : '??';
};

Ant.inspect = (...args) => console.inspect(...args);
