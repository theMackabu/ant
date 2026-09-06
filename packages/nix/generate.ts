import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import flake, { project } from "./src/flake.ts";
import packageDefinition from "./src/package.ts";
import shell from "./src/shell.ts";
import toolchain from "./src/toolchain.ts";
import vendor from "./src/vendor.ts";
import version from "./src/version.ts";

const args = process.argv.slice(2);
if (args.some((arg) => arg !== "--check")) {
  throw new Error("Usage: ant packages/nix/generate.ts [--check]");
}

const check = args.includes("--check");
const outputs = [
  ["../../flake.nix", "flake.ts", project.outputsFrom("./packages/nix/generated/flake.nix")],
  ["./generated/flake.nix", "flake.ts", flake],
  ["./generated/package.nix", "package.ts", packageDefinition],
  ["./generated/shell.nix", "shell.ts", shell],
  ["./generated/toolchain.nix", "toolchain.ts", toolchain],
  ["./generated/vendor.nix", "vendor.ts", vendor],
  ["./generated/version.nix", "version.ts", version],
] as const;

if (!check) {
  mkdirSync(fileURLToPath(new URL("./generated/", import.meta.url)), { recursive: true });
}

for (const [relative, source, expression] of outputs) {
  const filename = fileURLToPath(new URL(relative, import.meta.url));
  const header = `# Generated from packages/nix/src/${source}; run ant packages/nix/generate.ts.\n`;
  const text = header + expression.render();

  if (check) {
    if (readFileSync(filename, "utf8") !== text) {
      throw new Error(`Stale generated file: ${filename}`);
    }
  } else {
    writeFileSync(filename, text);
  }
}
