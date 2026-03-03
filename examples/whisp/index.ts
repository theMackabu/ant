import whisp from './whisp';
import { readFile } from 'fs/promises';

const file = process.argv[2];
whisp`(write "running ${file}:\n\n"`;

const source = await readFile(file, 'utf8');
whisp(source);
