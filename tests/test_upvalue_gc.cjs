let memo;
function setMemo() {
  const o = {};
  o.KEY = 'value-live';
  memo = o;
}
function getMemo() {
  return memo ? JSON.stringify(memo) : 'unset';
}

function churn(n) {
  let j = [];
  for (let i = 0; i < n; i++) j.push({ x: i, s: 'str' + i });
  return j.length;
}

churn(400000); // age the closures (and their upvalue cells) through minor GCs
churn(400000);
setMemo(); // young object -> old closure's closed upvalue
churn(400000); // minor GCs with the upvalue as sole reference
churn(400000);

setTimeout(() => {
  const got = getMemo();
  const ok = got === '{"KEY":"value-live"}';
  console.log(ok ? 'ALL PASS' : 'FAIL: memo is ' + got);
  if (!ok) process.exitCode = 1;
}, 10);

let memoW;
function setMemoWith() {
  const scope = { unrelated: 1 };
  with (scope) {
    memoW = { KEY: 'with-live' };
  }
}
function getMemoWith() {
  return memoW ? JSON.stringify(memoW) : 'unset';
}

churn(400000);
churn(400000);
setMemoWith();
churn(400000);
churn(400000);

setTimeout(() => {
  const got = getMemoWith();
  const ok = got === '{"KEY":"with-live"}';
  console.log(ok ? 'WITH PASS' : 'WITH FAIL: memo is ' + got);
  if (!ok) process.exitCode = 1;
}, 20);
