import type { Bindings } from './env';

let antLogoDataUrl: Promise<string> | null = null;
let ogFontBytes: Promise<Uint8Array[]> | null = null;

export async function sha256(input: string): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(input));
  return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('');
}

export function publicUrl(requestUrl: string, id: string): string {
  const url = new URL(requestUrl);
  return `${url.origin}/${id}`;
}

export function publicOgImageUrl(requestUrl: string, id: string): string {
  const url = new URL(requestUrl);
  return `${url.origin}/og/${id}.png`;
}

export function ogImageId(value: string): string {
  return value.endsWith('.png') ? value.slice(0, -4) : value;
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = '';
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunkSize));
  }
  return btoa(binary);
}

async function loadAntLogoDataUrl(env: Bindings, requestUrl: string): Promise<string> {
  const response = await env.ASSETS.fetch(new Request(new URL('/assets/ant.png', requestUrl)));
  if (!response.ok) return '';
  const bytes = new Uint8Array(await response.arrayBuffer());
  return `data:image/png;base64,${bytesToBase64(bytes)}`;
}

async function loadAssetBytes(
  env: Bindings,
  requestUrl: string,
  path: string,
): Promise<Uint8Array> {
  const response = await env.ASSETS.fetch(new Request(new URL(path, requestUrl)));
  if (!response.ok) return new Uint8Array();
  return new Uint8Array(await response.arrayBuffer());
}

export function getAntLogoDataUrl(env: Bindings, requestUrl: string): Promise<string> {
  antLogoDataUrl ??= loadAntLogoDataUrl(env, requestUrl);
  return antLogoDataUrl;
}

export function getOgFontBytes(env: Bindings, requestUrl: string): Promise<Uint8Array[]> {
  ogFontBytes ??= Promise.all([
    loadAssetBytes(env, requestUrl, '/assets/Arial.ttf'),
    loadAssetBytes(env, requestUrl, '/assets/Arial-Bold.ttf'),
    loadAssetBytes(env, requestUrl, '/assets/BerkeleyMono-Regular.woff2'),
  ]);
  return ogFontBytes;
}
