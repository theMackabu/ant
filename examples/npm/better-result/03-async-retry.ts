import { Result, TaggedError } from 'better-result';

class ServiceUnavailable extends TaggedError('ServiceUnavailable')<{
  attempt: number;
  cause: unknown;
  message: string;
}> {}

let calls = 0;
let currentAttempt = 0;

const result = await Result.tryPromise(
  {
    try: async ({ attempt }) => {
      currentAttempt = attempt;
      calls += 1;
      console.log(`attempt ${attempt}`);

      if (attempt < 3) {
        throw new Error('temporary outage');
      }

      return { id: 'usr_123', name: 'Grace' };
    },
    catch: cause =>
      new ServiceUnavailable({
        attempt: currentAttempt,
        cause,
        message: `Service failed on attempt ${currentAttempt}`
      })
  },
  {
    retry: {
      times: 3,
      delayMs: 10,
      backoff: 'constant',
      shouldRetry: error => error._tag === 'ServiceUnavailable'
    }
  }
);

console.log(
  result.match({
    ok: user => `loaded ${user.name} after ${calls} calls`,
    err: error => `gave up after attempt ${error.attempt}: ${error.message}`
  })
);
