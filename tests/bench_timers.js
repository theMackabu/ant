let frameCount = 0;
let startTime = Date.now();

function tick() {
  frameCount++;
}

setInterval(tick, 0.5);

setInterval(() => {
  const elapsed = Date.now() - startTime;
  const actualHz = frameCount / (elapsed / 1000);
  console.log(`Elapsed: ${elapsed}ms | Frames: ${frameCount} | Actual Hz: ${actualHz.toFixed(2)}`);
}, 1000);

setTimeout(() => {
  process.exit(0);
}, 10000);
