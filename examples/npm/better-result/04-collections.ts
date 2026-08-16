import { Result, TaggedError, type Result as ResultType } from 'better-result';

class InvalidNumber extends TaggedError('InvalidNumber')<{
  input: string;
  message: string;
}> {}

const parsePositiveNumber = (input: string): ResultType<number, InvalidNumber> => {
  const value = Number(input);
  return Number.isFinite(value) && value > 0
    ? Result.ok(value)
    : Result.err(new InvalidNumber({ input, message: `${JSON.stringify(input)} is not positive` }));
};

const inputs = ['10', '-2', '3.5', 'oops'];
const parsed = inputs.map(parsePositiveNumber);

const [values, errors] = Result.partition(parsed);
console.log('valid:', values);
console.log(
  'invalid:',
  errors.map(error => error.input)
);

const allValid = Result.all(['2', '4', '8'].map(parsePositiveNumber));
console.log(
  allValid.match({
    ok: numbers => `sum: ${numbers.reduce((sum, number) => sum + number, 0)}`,
    err: error => error.message
  })
);
