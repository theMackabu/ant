import type { Child } from 'hono/jsx';
import type { CrashReport } from './schema';

function formatBytes(value: number | null): string {
  if (!value || !Number.isFinite(value)) return 'unknown';
  if (value >= 1024 * 1024) return `${Math.round(value / 1024 / 1024)}mb`;
  if (value >= 1024) return `${Math.round(value / 1024)}kb`;
  return `${value}b`;
}

const Shell = ({ title, children }: { title: string; children: Child }) => (
  <html lang="en">
    <head>
      <meta charset="utf-8" />
      <meta name="viewport" content="width=device-width,initial-scale=1" />
      <link rel="icon" type="image/x-icon" href="/favicon.ico" />
      <link rel="stylesheet" href="/assets/report.css" />
      <title>{title}</title>
    </head>
    <body>{children}</body>
  </html>
);

const Logo = () => (
  <a href="/" id="logo">
    <img src="/assets/ant.png" alt="Home" />
  </a>
);

const BlankPage = () => (
  <Shell title="js.report">
    <Logo />
    <p>
      <b>404.</b> <ins>That's an error.</ins>
    </p>
    <p>If you were given a report link, check the URL and try again.</p>
  </Shell>
);

const ReportPage = ({ report, url }: { report: CrashReport; url: string }) => (
  <Shell title={`${report.reason} at ${report.addr}`}>
    <Logo />
    <p>
      <b>{report.reason}.</b>{' '}
      <ins>
        {report.code} at {report.addr}
      </ins>
      <br />
      <span>Ant crashed and sent a redacted report.</span>
    </p>

    <div class="meta">
      <p>
        <span class="label">Runtime:</span> Ant {report.version}
      </p>
      <p>
        <span class="label">Platform:</span> {report.os} {report.arch}
      </p>
      <p>
        <span class="label">Target:</span> <code>{report.target}</code>
      </p>
      <p>
        <span class="label">Elapsed:</span> {report.elapsedMs ?? 'unknown'}ms
      </p>
      <p>
        <span class="label">Peak RSS:</span> {formatBytes(report.peakRss)}
      </p>
    </div>

    <div class="meta">
      <i>Native backtrace:</i>
      <ol class="frames">
        {report.frames.length ? (
          report.frames.map(frame => (
            <li>
              <code>{frame}</code>
            </li>
          ))
        ) : (
          <li>
            <ins>No native frames were captured.</ins>
          </li>
        )}
      </ol>
    </div>

    <p class="url">
      <span class="label">This report URL:</span> <a href={url}>{url}</a>
    </p>
  </Shell>
);

export function renderBlank(): string {
  return `<!doctype html>${(<BlankPage />)}`;
}

export function renderReport(report: CrashReport, url: string): string {
  return `<!doctype html>${(<ReportPage report={report} url={url} />)}`;
}
