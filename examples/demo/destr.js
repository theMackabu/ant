import { $ } from 'ant:shell';
import parse from 'https://esm.sh/destr';

const data = $`maid -g json-hydrated`.text();
console.log(parse(data));
