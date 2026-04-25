import type { Child } from 'hono/jsx';
import type { CrashReport } from './schema';

function formatBytes(value: number | null): string {
  if (!value || !Number.isFinite(value)) return 'unknown';
  if (value >= 1024 * 1024) return `${Math.round(value / 1024 / 1024)}mb`;
  if (value >= 1024) return `${Math.round(value / 1024)}kb`;
  return `${value}b`;
}

function crashDetail(code: string): string {
  switch (code) {
    case 'SIGSEGV':
    case 'EXCEPTION_ACCESS_VIOLATION':
      return 'Invalid memory access';
    case 'SIGBUS':
      return 'Bus error';
    case 'SIGFPE':
      return 'Floating point exception';
    case 'SIGILL':
    case 'EXCEPTION_ILLEGAL_INSTRUCTION':
      return 'Illegal instruction';
    case 'SIGABRT':
      return 'Abort';
    case 'EXCEPTION_STACK_OVERFLOW':
      return 'Stack overflow';
    default:
      return 'Fatal error';
  }
}

function renderFrames(frames: string[]): Child {
  if (!frames.length) return 'No native frames were captured.';
  return frames.map((frame, index) => (
    <>
      <span class="frame-index">{index + 1}.</span> {frame}
      {index < frames.length - 1 ? '\n' : ''}
    </>
  ));
}

const Shell = ({ title, children }: { title: string; children: Child }) => (
  <html lang="en">
    <head>
      <meta charset="utf-8" />
      <meta name="viewport" content="width=device-width,initial-scale=1" />
      <link rel="icon" type="image/x-icon" href="/favicon.ico" />
      <link rel="stylesheet" href="/assets/report.css" />
      <script src="/assets/report.js" defer></script>
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

const ReportFooter = () => (
  <p class="footer">
    <a href="https://github.com/theMackabu/ant">Ant</a>
    <ins> · </ins>
    <a href="https://github.com/theMackabu/ant/issues/new">Report this on GitHub</a>
  </p>
);

const BlankPage = () => (
  <Shell title="js.report">
    <Logo />
    <p>
      <b>404.</b> <ins>That's an error.</ins>
    </p>
    <p style="margin-top: -10px">If you were given a report link, check the URL and try again.</p>
    <p class="footer">
      <a href="https://github.com/theMackabu/ant">Ant on GitHub</a>
    </p>
  </Shell>
);

const ReportPage = ({ report, url }: { report: CrashReport; url: string }) => {
  const detail = crashDetail(report.code);

  return (
    <Shell title={`Ant crash report | ${detail}`}>
      <Logo />
      <p>
        <b>{report.reason}.</b>{' '}
        <ins>
          {detail} at {report.addr}
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
        <pre class="frames" tabindex={0}>
          <code>{renderFrames(report.frames)}</code>
        </pre>
      </div>

      <p class="url">
        <span class="label">This report URL:</span>
        <br />
        <a href={url} class="copy-url" data-copy-url={url}>
          {url}
        </a>
      </p>

      <ReportFooter />
    </Shell>
  );
};

export function renderBlank(): string {
  return `<!doctype html>${(<BlankPage />)}`;
}

export function renderReport(report: CrashReport, url: string): string {
  return `<!doctype html>${(<ReportPage report={report} url={url} />)}`;
}
