import { BuildPipeline } from '~/pipeline/BuildPipeline';
import { exact } from '@exact';
import { a } from '@lib/a';
import { b } from '@lib/deep/b';
import { dir } from '~/dir';
import { tsExt } from '~/ts-ext.js';
import { odd } from 'odd';
import { required } from '~/req.cjs';
import { plain } from '~/plain.mjs';
import cond from 'cond-exports';

export const results = {
  pipeline: new BuildPipeline().name,
  exact, a, b, dir, tsExt, odd, required, plain,
  cond: cond.branch,
};
