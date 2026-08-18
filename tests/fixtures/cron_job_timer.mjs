let active = 0;
let calls = 0;
let maxActive = 0;
let receiverMatches = true;
let job;

const watchdog = setTimeout(() => {
  console.error('cron timer fixture timed out');
  process.exit(1);
}, 1_000);
watchdog.unref();

job = Ant.cron('@yearly', async function () {
  receiverMatches &&= this === job;
  calls++;
  active++;
  maxActive = Math.max(maxActive, active);
  await new Promise(resolve => setTimeout(resolve, 30));
  active--;
  if (calls === 2) {
    this.stop();
    console.log(JSON.stringify({ calls, maxActive, receiverMatches }));
  }
});
