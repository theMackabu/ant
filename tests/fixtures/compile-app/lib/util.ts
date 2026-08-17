export interface Greeting { who: string }

export function greet(who: string): string {
  const g: Greeting = { who };
  return `hello ${g.who}`;
}

export function boom(): never {
  throw new Error("kaboom");
}
