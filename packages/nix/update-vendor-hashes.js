#!/usr/bin/env ant

import fs from "node:fs";
import path from "node:path";
import { devNull } from "node:os";
import { createHash } from "node:crypto";
import { promisify } from "node:util";
import { fileURLToPath } from "node:url";
import { execFile, execFileSync } from "node:child_process";

const run = promisify(execFile);
const copyOptions = { recursive: true, verbatimSymlinks: true };

export function narHash(root, entries) {
  const digest = createHash("sha256");
  const chunk = Buffer.allocUnsafe(1024 * 1024);
  const padding = (size) => digest.update(Buffer.alloc((8 - (size % 8)) % 8));

  const integer = (value) => {
    const bytes = Buffer.alloc(8);
    bytes.writeBigUInt64LE(BigInt(value));
    digest.update(bytes);
  };

  const string = (value) => {
    const bytes = Buffer.isBuffer(value) ? value : Buffer.from(value);
    integer(bytes.length);
    digest.update(bytes);
    padding(bytes.length);
  };

  function node(filename, children) {
    string("(");
    string("type");
    const stat = filename === null ? null : fs.lstatSync(filename);
    if (stat === null || stat.isDirectory()) {
      string("directory");
      children ??= new Map(
        fs
          .readdirSync(filename, { encoding: "buffer" })
          .map((name) => [name, Buffer.concat([Buffer.from(filename), Buffer.from(path.sep), name])]),
      );
      const sorted = [...children].map(([name, child]) => [Buffer.from(name), child]);
      sorted.sort(([a], [b]) => Buffer.compare(a, b));
      for (const [name, child] of sorted) {
        for (const token of ["entry", "(", "name", name, "node"]) string(token);
        node(child);
        string(")");
      }
    } else if (stat.isSymbolicLink()) {
      string("symlink");
      string("target");
      string(fs.readlinkSync(filename, { encoding: "buffer" }));
    } else if (stat.isFile()) {
      string("regular");
      if (stat.mode & 0o100) {
        string("executable");
        string("");
      }
      string("contents");
      integer(stat.size);
      const fd = fs.openSync(filename, "r");
      try {
        let size;
        while ((size = fs.readSync(fd, chunk, 0, chunk.length, null)) > 0) {
          digest.update(chunk.subarray(0, size));
        }
      } finally {
        fs.closeSync(fd);
      }
      padding(stat.size);
    } else {
      throw new Error(`Unsupported file type: ${filename}`);
    }
    string(")");
  }
  string("nix-archive-1");
  node(root, entries);
  return `sha256-${digest.digest("base64")}`;
}

function git(root, ...args) {
  return execFileSync("git", ["-C", root, ...args], { encoding: "utf8" });
}

export function snapshotInputs(root, destination) {
  const paths = ["vendor/packagefiles", ":(top,glob)vendor/*.wrap"];
  const untracked = git(root, "ls-files", "--others", "--exclude-standard", "--", ...paths);
  if (untracked) throw new Error(`git add these vendor inputs first:\n${untracked}`);

  for (const name of git(root, "ls-files", "-z", "--", ...paths)
    .split("\0")
    .filter(Boolean)) {
    const source = path.join(root, name);
    if (!fs.lstatSync(source, { throwIfNoEntry: false })) continue;
    const target = path.join(destination, name);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.cpSync(source, target, copyOptions);
  }
}

export function wrapInputs(wrap, vendor) {
  const values = Object.create(null);
  let section, previous, previousIndent;
  for (const line of fs.readFileSync(wrap, "utf8").split(/\r?\n/)) {
    const text = line.trim();
    if (!text || text.startsWith("#") || text.startsWith(";")) continue;
    const heading = text.match(/^\[([^\]]+)\]$/);
    if (heading) {
      if (section) break;
      section = heading[1];
      continue;
    }
    const indent = line.length - line.trimStart().length;
    if (previous && indent > previousIndent) {
      values[previous] += `\n${text}`;
      continue;
    }
    const property = text.match(/^([^=:]+?)\s*[:=]\s*(.*)$/);
    if (!section || !property) throw new Error(`Invalid wrap: ${wrap}`);
    const key = property[1].trim().toLowerCase();
    if (Object.hasOwn(values, key)) throw new Error(`Duplicate ${key} in ${wrap}`);
    values[key] = property[2];
    previous = key;
    previousIndent = indent;
  }
  if (!["wrap-file", "wrap-git"].includes(section)) throw new Error(`Unsupported wrap type in ${wrap}: ${section}`);
  const name = path.basename(wrap);
  const directory = values.directory ?? path.basename(wrap, ".wrap");
  if (!directory || path.basename(directory) !== directory || [".", ".."].includes(directory)) {
    throw new Error(`Invalid wrap directory: ${directory}`);
  }
  const inputs = new Map([[name, wrap]]);
  const names = (values.diff_files ?? "")
    .split(",")
    .map((name) => name.trim())
    .filter(Boolean);
  if (values.patch_directory) names.push(values.patch_directory);
  for (const key of ["source", "patch"]) {
    if (values[`${key}_filename`] && !values[`${key}_url`]) names.push(values[`${key}_filename`]);
  }
  const packagefiles = fs.realpathSync(path.join(vendor, "packagefiles"));
  for (const name of names) {
    const filename = path.resolve(packagefiles, name);
    if (!fs.existsSync(filename)) throw new Error(`Missing packagefile: ${name}`);
    const relative = path.relative(packagefiles, fs.realpathSync(filename));
    if (relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
      throw new Error(`Packagefile escapes vendor/packagefiles: ${name}`);
    }
    inputs.set(name, filename);
  }
  return { directory, inputs, values };
}

function isDirectory(filename) {
  return fs.statSync(filename, { throwIfNoEntry: false })?.isDirectory() ?? false;
}

function stripGit(tree) {
  for (const entry of fs.readdirSync(tree, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const filename = path.join(tree, entry.name);
    if (entry.name === ".git") fs.rmSync(filename, { recursive: true, force: true });
    else stripGit(filename);
  }
}

export async function resolveWrap(wrap, vendor, cache, mesonVersion, root) {
  const { directory, inputs, values } = wrapInputs(wrap, vendor);
  const key = createHash("sha256")
    .update(`v1\0${mesonVersion}\0${narHash(null, inputs)}`)
    .digest("hex");
  const cached = path.join(cache, "trees", key);
  const name = path.basename(wrap, ".wrap");
  if (isDirectory(cached)) {
    console.log(`Cached ${name}`);
    return [directory, path.join(cached, directory)];
  }
  console.log(`Fetching ${name}...`);
  const work = fs.mkdtempSync(path.join(cache, "trees", "tmp-"));
  try {
    const staged = path.join(work, "vendor");
    fs.mkdirSync(staged);
    fs.copyFileSync(wrap, path.join(staged, path.basename(wrap)));
    fs.cpSync(path.join(vendor, "packagefiles"), path.join(staged, "packagefiles"), copyOptions);
    fs.writeFileSync(path.join(work, "meson.build"), "project('ant-vendor', subproject_dir: 'vendor')\n");
    for (const key of ["source_filename", "patch_filename"]) {
      const filename = values[key];
      if (filename && path.basename(filename) === filename) {
        const existing = path.join(root, "vendor/packagecache", filename);
        const target = path.join(cache, "downloads", filename);
        if (fs.statSync(existing, { throwIfNoEntry: false })?.isFile() && !fs.existsSync(target)) {
          fs.copyFileSync(existing, target);
        }
      }
    }
    try {
      await run("meson", ["subprojects", "download", "--sourcedir", work], {
        maxBuffer: 16 * 1024 * 1024,
        env: {
          ...process.env,
          MESON_PACKAGE_CACHE_DIR: path.join(cache, "downloads"),
          GIT_CONFIG_NOSYSTEM: "1",
          GIT_CONFIG_GLOBAL: devNull,
          GIT_TERMINAL_PROMPT: "0",
          LC_ALL: "C",
          NO_COLOR: "1",
        },
      });
    } catch (error) {
      const boringssl =
        name === "boringssl" &&
        error.stdout?.includes("  -> Subproject exists but has no meson.build file.") &&
        fs.existsSync(path.join(staged, directory, "CMakeLists.txt"));
      if (!boringssl) throw new Error(`${path.basename(wrap)}:\n${error.stdout ?? ""}${error.stderr ?? error.message}`);
    }
    const tree = path.join(staged, directory);
    if (!isDirectory(tree)) throw new Error(`${name}: download produced no source directory`);
    stripGit(tree);
    const completed = path.join(work, "complete");
    fs.mkdirSync(completed);
    fs.renameSync(tree, path.join(completed, directory));
    try {
      fs.renameSync(completed, cached);
    } catch (error) {
      if (!isDirectory(cached)) throw error;
    }
    return [directory, path.join(cached, directory)];
  } finally {
    fs.rmSync(work, { recursive: true, force: true });
  }
}

export async function main() {
  const root = git(process.cwd(), "rev-parse", "--show-toplevel").trim();
  const version = execFileSync("meson", ["--version"], { encoding: "utf8" }).trim();
  const cache = path.join(root, ".cache/nix-vendor");
  for (const name of ["trees", "downloads"]) fs.mkdirSync(path.join(cache, name), { recursive: true });
  const snapshot = fs.mkdtempSync(path.join(cache, "tmp-"));
  try {
    snapshotInputs(root, snapshot);
    const vendor = path.join(snapshot, "vendor");
    const wraps = fs
      .readdirSync(vendor)
      .filter((name) => name.endsWith(".wrap"))
      .sort()
      .map((name) => path.join(vendor, name));
    if (!wraps.length) throw new Error("No vendor wraps found");
    const directories = wraps.map((wrap) => wrapInputs(wrap, vendor).directory);
    const reserved = new Set([...wraps.map((wrap) => path.basename(wrap)), "packagefiles", ".wraplock"]);
    if (new Set(directories).size !== directories.length || directories.some((name) => reserved.has(name))) {
      throw new Error("Vendor wrap directories overlap");
    }
    const entries = new Map(wraps.map((wrap) => [path.basename(wrap), wrap]));
    entries.set("packagefiles", path.join(vendor, "packagefiles"));
    fs.writeFileSync(path.join(vendor, ".wraplock"), "");
    entries.set(".wraplock", path.join(vendor, ".wraplock"));
    let next = 0;
    const workers = Array.from({ length: 4 }, async () => {
      while (next < wraps.length) {
        const wrap = wraps[next++];
        const [directory, filename] = await resolveWrap(wrap, vendor, cache, version, root);
        entries.set(directory, filename);
      }
    });
    const results = await Promise.allSettled(workers);
    const failure = results.find((result) => result.status === "rejected");
    if (failure) throw failure.reason;
    const newHash = narHash(null, entries);
    const vendorNix = path.join(root, "packages/nix/vendor.nix");
    const original = fs.readFileSync(vendorNix, "utf8");
    let count = 0;
    const updated = original.replace(/^(\s*outputHash\s*=\s*)"[^"]+"/gm, (_, prefix) => {
      count++;
      return `${prefix}"${newHash}"`;
    });
    if (count !== 1) throw new Error("Expected exactly one outputHash in packages/nix/vendor.nix");
    if (updated !== original) fs.writeFileSync(vendorNix, updated);
    console.log(`packages/nix/vendor.nix -> ${newHash}`);
  } finally {
    fs.rmSync(snapshot, { recursive: true, force: true });
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url))
  main().catch((error) => {
    console.error(`FATAL: ${error.message}`);
    process.exitCode = 1;
  });
