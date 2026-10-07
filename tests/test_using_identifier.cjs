// `using` starts a declaration only when a binding name follows on the same
// line. Elsewhere it is an ordinary identifier: calls, member access, ASI and
// for-in/of heads must parse the way Node parses them.
const assert = require('node:assert');

const out = [];
{
  const using = x => out.push('called ' + x);
  using(1);
}
{
  let using = [5];
  using[0] = 6;
  out.push('index ' + using[0]);
}
{
  let using = 3, x = 2;
  using
  x;
  using
  +1;
  out.push('asi ' + using);
}
{
  var using;
  for (using of [7]);
  out.push('for-of ' + using);
  for (using in { k: 1 });
  out.push('for-in ' + using);
}
{
  using d = { [Symbol.dispose]() { out.push('disposed') } };
}
for (using d of [{ [Symbol.dispose]() { out.push('for disposed') } }]);

(async () => {
  const using = v => v;
  out.push('await call ' + await using(8));
  {
    await using d = { async [Symbol.asyncDispose]() { out.push('async disposed') } };
  }
  assert.deepStrictEqual(out, [
    'called 1', 'index 6', 'asi 3', 'for-of 7', 'for-in k', 'disposed', 'for disposed', 'await call 8', 'async disposed',
  ]);
  console.log('PASS using parses as an identifier unless it declares');
})();
