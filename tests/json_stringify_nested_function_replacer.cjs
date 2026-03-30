const obj = {
  action() {
    return 1;
  },
  child: {
    inner() {
      return 2;
    },
  },
  missing: undefined,
};

const out = JSON.stringify(obj, (_key, value) => {
  if (typeof value === 'function') return `[fn:${value.name || 'anonymous'}]`;
  if (value === undefined) return '[undef]';
  return value;
});

console.log(out);
