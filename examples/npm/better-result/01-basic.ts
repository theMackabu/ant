import { Result, TaggedError, type Result as ResultType } from 'better-result';

class InvalidJson extends TaggedError('InvalidJson')<{
  cause: unknown;
  message: string;
}> {}

class InvalidProfile extends TaggedError('InvalidProfile')<{
  field: string;
  message: string;
}> {}

type Profile = {
  name: string;
  age: number;
};

const parseProfile = (input: string): ResultType<Profile, InvalidJson | InvalidProfile> =>
  Result.try({
    try: () => JSON.parse(input) as unknown,
    catch: cause => new InvalidJson({ cause, message: 'Input is not valid JSON' })
  }).andThen(value => {
    if (typeof value !== 'object' || value === null || !('name' in value) || typeof value.name !== 'string') {
      return Result.err(new InvalidProfile({ field: 'name', message: 'name must be a string' }));
    }

    if (!('age' in value) || typeof value.age !== 'number' || value.age < 0) {
      return Result.err(
        new InvalidProfile({
          field: 'age',
          message: 'age must be a non-negative number'
        })
      );
    }

    return Result.ok({ name: value.name, age: value.age });
  });

for (const input of ['{"name":"Ada","age":36}', '{"name":"Ada","age":-1}', '{not json}']) {
  const output = parseProfile(input)
    .map(profile => `Welcome, ${profile.name} (age ${profile.age})`)
    .match({
      ok: message => message,
      err: error =>
        error.match({
          InvalidJson: () => 'Could not parse JSON',
          InvalidProfile: invalid => `Invalid ${invalid.field}: ${invalid.message}`
        })
    });

  console.log(output);
}
