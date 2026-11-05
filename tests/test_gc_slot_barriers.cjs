const assert = require('node:assert');

function churn(rounds = 3) {
  const keep = [];
  for (let r = 0; r < rounds; r++) for (let i = 0; i < 120000; i++) keep[i % 4096] = { i, r };
  return keep.length;
}

const base = 'abcdefghij'.repeat(30);

function cachedGroupsSurviveOnOldMatch() {
  const matches = [];
  for (let i = 0; i < 200; i++) matches.push(/(?<a>x)(?<b>y)/.exec('xy'));
  churn();
  for (const match of matches) match.groups;
  churn();
  for (const match of matches) {
    assert.strictEqual(match.groups.a, 'x');
    assert.strictEqual(match.groups.b, 'y');
  }
}

function compiledSourceSurvivesOnOldRegExp() {
  const regexps = [];
  for (let i = 0; i < 300; i++) regexps.push(/a/);
  churn();
  for (let i = 0; i < 300; i++) regexps[i].compile(base + String(1000000 + i));
  churn();
  for (let i = 0; i < 300; i++) assert.strictEqual(regexps[i].source, base + String(1000000 + i));
}

function stringWrapperKeepsValueProducedDuringCoercion() {
  const wrappers = [];
  for (let i = 0; i < 12; i++) {
    wrappers.push(new String({
      toString() {
        churn(1);
        return base + String(1000000 + i);
      },
    }));
  }
  churn();
  for (let i = 0; i < 12; i++) assert.strictEqual(wrappers[i].toString(), base + String(1000000 + i));
}

cachedGroupsSurviveOnOldMatch();
compiledSourceSurvivesOnOldRegExp();
stringWrapperKeepsValueProducedDuringCoercion();
console.log('gc-slot-barriers:ok');
