import { $ } from 'ant:shell';
import parse from 'https://esm.sh/destr';

const data = await $`maid -g json-hydrated`.text();
console.log(parse(data));
