console.log('Ant.cron demo');

const origin = new Date('2026-01-01T00:00:01Z');
const schedules = ['@hourly', '0 9 * * MON-FRI', '@yearly'];

for (const schedule of schedules) {
  const next = Ant.cron.parse(schedule, origin, { tz: 'UTC' });
  console.log(`${schedule.padEnd(18)} -> ${next.toISOString()}`);
}

const job = Ant.cron('@hourly', async function () {
  console.log(`fired ${this.cron} at ${new Date().toISOString()}`);
}, { tz: 'UTC' });

console.log('job cron:', job.cron);
console.log('lifecycle methods:', ['ref', 'stop', 'unref', Symbol.dispose].filter(key => typeof job[key] === 'function').length);

// the demo should exit immediately instead of waiting for the next hour.
job.unref().stop();
