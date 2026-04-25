import { crashDetail } from './format';
import type { CrashReport } from './schema';
import { html as honoHtml, raw } from 'hono/html';
import { initWasm, Resvg } from '@resvg/resvg-wasm';
import resvgWasm from '@resvg/resvg-wasm/index_bg.wasm';

let resvgReady: Promise<void> | null = null;

function ensureResvg(): Promise<void> {
  resvgReady ??= initWasm(resvgWasm);
  return resvgReady;
}

function truncate(value: string, maxLength: number): string {
  if (value.length <= maxLength) return value;
  return `${value.slice(0, Math.max(0, maxLength - 3))}...`;
}

type SvgChunk = ReturnType<typeof raw>;

function html(strings: TemplateStringsArray, ...values: unknown[]): SvgChunk {
  return honoHtml(strings, ...values) as SvgChunk;
}

function text(x: number, y: number, value: string, className = ''): SvgChunk {
  if (className) return html`<text x="${x}" y="${y}" class="${className}">${value}</text>`;
  return html`<text x="${x}" y="${y}">${value}</text>`;
}

function renderOgSvg(report: CrashReport, logoDataUrl: string): string {
  const detail = crashDetail(report.code);
  const frames = report.frames.slice(0, 8);
  const frameLines = frames.length ? frames : ['No native frames were captured.'];

  const frameSvg = frameLines
    .map((frame, index) => {
      const y = 402 + index * 28;
      return [
        String(text(46, y, `${index + 1}.`, 'frame-index')),
        String(text(94, y, truncate(frame, 96), 'frame')),
      ].join('');
    })
    .join('');

  return String(
    html`<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="630" viewBox="0 0 1200 630">
      <style>
        text {
          font-family: Arial;
          fill: #222;
        }
        .muted {
          fill: #777;
        }
        .title {
          font-size: 40px;
          font-weight: 700;
        }
        .subtitle {
          font-size: 34px;
        }
        .meta {
          font-size: 30px;
        }
        .label {
          fill: #777;
        }
        .code {
          font-family: 'Berkeley Mono';
          font-size: 22px;
        }
        .frame {
          font-family: 'Berkeley Mono';
          font-size: 22px;
        }
        .frame-index {
          font-family: 'Berkeley Mono';
          font-size: 22px;
          fill: #999;
        }
      </style>
      <defs>
        <linearGradient id="backtrace-fade" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stop-color="#fff" stop-opacity="0" />
          <stop offset="1" stop-color="#fff" />
        </linearGradient>
      </defs>
      <rect width="1200" height="630" fill="#fff" />
      <image href="${logoDataUrl}" x="46" y="32" width="65" height="65" />
      ${text(46, 149, `${report.reason}.`, 'title')}
      ${text(46, 196, `${detail} at ${report.addr}`, 'subtitle muted')}
      ${text(46, 256, 'Runtime:', 'meta label')}
      ${text(180, 256, `Ant ${truncate(report.version, 40)}`, 'meta')}
      ${text(46, 294, 'Platform:', 'meta label')}
      ${text(180, 294, `${truncate(report.os, 36)} ${report.arch}`, 'meta')}
      ${text(46, 352, 'Native backtrace:', 'meta')} ${raw(frameSvg)}
      <rect x="46" y="540" width="1108" height="64" fill="url(#backtrace-fade)" />
    </svg>`,
  );
}

export async function renderOgPng(
  report: CrashReport,
  logoDataUrl: string,
  fontBytes: Uint8Array[],
): Promise<ArrayBuffer> {
  await ensureResvg();

  const resvg = new Resvg(renderOgSvg(report, logoDataUrl), {
    font: {
      fontBuffers: fontBytes,
      loadSystemFonts: false,
      defaultFontFamily: 'Arial',
      sansSerifFamily: 'Arial',
      monospaceFamily: 'Berkeley Mono',
    },
  });

  try {
    const image = resvg.render();
    try {
      const png = image.asPng();
      return new Uint8Array(png).buffer;
    } finally {
      image.free();
    }
  } finally {
    resvg.free();
  }
}
