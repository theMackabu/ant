// JIT object-literal templates are cached on the function. A minor GC must not
// free a cached template while the owning function is idle (not on the stack),
// or later literals are copied from a recycled object.
function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const creators = {
  five: (() => () => ({ a: null, b: null, c: null, d: null, e: null }))(),
  three: (() => () => ({ a: 1, b: true, c: 'camp' }))(),
  nested: (() => () => ({ kind: 'span', text: 'x', style: { bold: true } }))(),
  empty: (() => () => ({}))(),
};
const expected = {
  five: '{"a":null,"b":null,"c":null,"d":null,"e":null}',
  three: '{"a":1,"b":true,"c":"camp"}',
  nested: '{"kind":"span","text":"x","style":{"bold":true}}',
  empty: '{}',
};

const holder = { creators };
let sink = [];

function churn(rounds) {
  for (let r = 0; r < rounds; r++) {
    sink = [];
    for (let i = 0; i < 40000; i++) sink.push({ i, row: [i, i + 1] });
  }
  sink = [];
}

churn(2);

setImmediate(() => {
  for (const name of Object.keys(holder.creators))
    for (let i = 0; i < 300; i++) holder.creators[name]();

  setTimeout(() => {
    for (let round = 0; round < 6; round++) {
      for (const name of Object.keys(holder.creators)) {
        const create = holder.creators[name];
        const first = create();
        first.extra = round;
        churn(1);
        const second = create();
        assert(JSON.stringify(second) === expected[name], `${name}: fresh literal was ${JSON.stringify(second)}`);
        assert(first.extra === round && !('extra' in second), `${name}: literal results share state`);
        churn(1);
      }
    }
    console.log('PASS');
  }, 0);
});
