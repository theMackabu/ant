import type { CrashReport } from './schema';
import { Fragment, type Child } from 'hono/jsx';
import { crashDetail, formatBytes } from './format';

function renderFrames(frames: string[]): Child {
  if (!frames.length) return 'No native frames were captured.';
  return frames.map((frame, index) => (
    <Fragment>
      <span class="frame-index">{index + 1}.</span> {frame}
      {index < frames.length - 1 ? '\n' : ''}
    </Fragment>
  ));
}

type Meta = {
  url: string;
  title: string;
  description: string;
  image?: string;
};

const Shell = ({ title, meta, children }: { title: string; meta: Meta; children: Child }) => (
  <html lang="en">
    <head>
      <meta charset="utf-8" />
      <meta name="viewport" content="width=device-width,initial-scale=1" />
      <meta property="og:url" content={meta.url} />
      <meta property="og:type" content="website" />
      <meta property="og:title" content={meta.title} />
      <meta property="og:description" content={meta.description} />
      {meta.image ? (
        <>
          <meta property="og:image" content={meta.image} />
          <meta property="og:image:type" content="image/png" />
          <meta property="og:image:width" content="1200" />
          <meta property="og:image:height" content="630" />
        </>
      ) : null}
      <meta name="twitter:card" content="summary_large_image" />
      <meta property="twitter:domain" content="js.report" />
      <meta property="twitter:url" content={meta.url} />
      <meta name="twitter:title" content={meta.title} />
      <meta name="twitter:description" content={meta.description} />
      {meta.image ? <meta name="twitter:image" content={meta.image} /> : null}
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
    <ins> · </ins>
    <a href="/privacy">Privacy</a>
  </p>
);

const BlankPage = () => (
  <Shell
    title="js.report"
    meta={{
      url: 'https://js.report',
      title: 'js.report',
      description: 'Crash reports for the Ant runtime.',
    }}
  >
    <Logo />
    <p>
      <b>404.</b> <ins>That's an error.</ins>
    </p>
    <p style="margin-top: -10px">If you were given a report link, check the URL and try again.</p>
    <p class="footer">
      <a href="https://github.com/theMackabu/ant">Ant on GitHub</a>
      <ins> · </ins>
      <a href="/privacy">Privacy</a>
    </p>
  </Shell>
);

const PrivacyPage = () => (
  <Shell
    title="Privacy | js.report"
    meta={{
      url: 'https://js.report/privacy',
      title: 'Privacy | js.report',
      description: 'What js.report stores for Ant crash reports.',
    }}
  >
    <Logo />
    <p>
      <b>Privacy.</b> <ins>Crash report storage policy.</ins>
    </p>
    <div class="policy">
      <p>
        The database stores the crash report JSON submitted by Ant, plus server metadata needed to
        render, deduplicate, and expire reports. Crash report dumps are retained for 30 days, then
        deleted.
      </p>
      <p>
        Ant does not upload personal information. It also does not store IP addresses in its
        database. It uploads only the crash fields listed below.
      </p>
      <p>
        <span class="label">Submitted report fields:</span>
        <br />
        <code>schema</code>, <code>runtime</code>, <code>version</code>, <code>target</code>,{' '}
        <code>os</code>, <code>arch</code>, <code>kind</code>, <code>code</code>,{' '}
        <code>reason</code>, <code>addr</code>, <code>elapsedMs</code>, <code>peakRss</code>,{' '}
        <code>frames</code>
      </p>
      <p>
        <span class="label">Stored server fields:</span>
        <br />
        <code>id</code>, <code>trace</code>, <code>firstSeenAt</code>, <code>lastSeenAt</code>,{' '}
        <code>hitCount</code>, <code>expiresAt</code>, and per-frame hashes used for deduplication.
      </p>
      <p>
        Crash report links are public to anyone with the URL. Raw JSON is available by adding{' '}
        <code>.json</code> to a report URL.
      </p>
    </div>
    <p class="footer">
      <a href="https://github.com/theMackabu/ant/tree/master/docs/reports">js.report source</a>
      <ins> · </ins>
      <a href="https://github.com/theMackabu/ant">Ant on GitHub</a>
    </p>
  </Shell>
);

const ReportPage = ({
  report,
  url,
  imageUrl,
}: {
  report: CrashReport;
  url: string;
  imageUrl: string;
}) => {
  const detail = crashDetail(report.code);

  return (
    <Shell
      title={`${detail} | js.report`}
      meta={{
        url,
        title: `${report.reason}. ${detail} at ${report.addr}`,
        description: `Ant ${report.version} crashed on ${report.os} ${report.arch}.`,
        image: imageUrl,
      }}
    >
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

export function renderPrivacy(): string {
  return `<!doctype html>${(<PrivacyPage />)}`;
}

export function renderReport(report: CrashReport, url: string, imageUrl: string): string {
  return `<!doctype html>${(<ReportPage report={report} url={url} imageUrl={imageUrl} />)}`;
}
