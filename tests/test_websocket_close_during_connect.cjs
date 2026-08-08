// Regression: WebSocket.close() while the connect is still in flight.
//
// tlsuv used to complete the close synchronously after starting connector
// cancellation, so the close event fired inside close(), the active-list root
// was dropped, GC freed the native state, and the connector's later
// cancellation callback dereferenced freed memory (SIGSEGV under allocation
// pressure). The close must stay deferred until the connector has released
// the websocket: readyState stays CLOSING when close() returns, exactly one
// close event fires per socket, no open event fires, and the process drains.
const assert = require('node:assert');

const TOTAL = 64;
let closes = 0;
let opens = 0;
let extraCloses = 0;

function churn() {
  // allocation pressure so a prematurely-freed native state gets reused
  const junk = [];
  for (let i = 0; i < 2000; i++) junk.push({ a: i, b: 'x'.repeat(64), c: [i, i + 1, i + 2] });
  return junk.length;
}

for (let r = 0; r < TOTAL; r++) {
  let ws = new WebSocket('ws://127.0.0.1:1/'); // nothing listens on port 1; connect stays in flight
  let closed = false;
  ws.addEventListener('open', () => opens++);
  ws.addEventListener('close', () => {
    if (closed) extraCloses++;
    else {
      closed = true;
      closes++;
    }
  });
  ws.close();
  assert.strictEqual(ws.readyState, WebSocket.CLOSING, 'readyState must remain CLOSING immediately after close()');
  ws = null;
  churn();
}

const deadline = Date.now() + 5000;
(function waitForCloses() {
  churn();
  if (closes === TOTAL && extraCloses === 0 && opens === 0) {
    console.log('websocket:close-during-connect:ok');
    return; // let the loop drain naturally
  }
  if (Date.now() > deadline || extraCloses > 0 || opens > 0) {
    console.error(`FAIL: closes=${closes}/${TOTAL} opens=${opens} extraCloses=${extraCloses}`);
    process.exit(1);
  }
  setTimeout(waitForCloses, 20);
})();
