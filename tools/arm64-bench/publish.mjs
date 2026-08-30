#!/usr/bin/env node

import fs from 'node:fs';

const resultPath = process.argv[2];
if (!resultPath) throw new Error('usage: node publish.mjs <result.json>');

const endpoint = process.env.BENCHMARK_API_URL || 'https://bench.antjs.org/v1/runs';
const token = process.env.BENCHMARK_PUBLISH_TOKEN;
if (!token) throw new Error('BENCHMARK_PUBLISH_TOKEN is required');

const payload = fs.readFileSync(resultPath, 'utf8');
JSON.parse(payload);

let lastError;
for (let attempt = 1; attempt <= 3; attempt++) {
  try {
    const response = await fetch(endpoint, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json'
      },
      body: payload,
      signal: AbortSignal.timeout(30_000)
    });
    const body = await response.text();
    if (!response.ok) throw new Error(`publish returned ${response.status}: ${body.slice(0, 2048)}`);
    process.stdout.write(`${body}\n`);
    lastError = null;
    break;
  } catch (error) {
    lastError = error;
    if (attempt < 3) await new Promise(resolve => setTimeout(resolve, attempt * 1000));
  }
}

if (lastError) throw lastError;
