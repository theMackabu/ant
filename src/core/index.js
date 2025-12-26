snapshot_inline('./events.js');

Ant.version = '{{VERSION}}';
Ant.revision = '{{GIT_HASH}}';
Ant.buildDate = '{{BUILD_DATE}}';

Ant.typeof = function (t) {
  const value = Ant.raw.typeof(t);

  const types = {
    T_OBJ: 'object',
    T_PROP: 'prop',
    T_STR: 'string',
    T_UNDEF: 'undefined',
    T_NULL: 'null',
    T_NUM: 'number',
    T_BOOL: 'boolean',
    T_FUNC: 'function',
    T_CODEREF: 'coderef',
    T_CFUNC: 'cfunc',
    T_ERR: 'err',
    T_ARR: 'array',
    T_PROMISE: 'promise',
    T_TYPEDARRAY: 'typedarray',
    T_BIGINT: 'bigint',
    T_PROPREF: 'propref'
  };

  const names = Object.values(types);
  return value < names.length ? names[value] : '??';
};
