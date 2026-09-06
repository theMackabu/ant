# Generated from packages/nix/src/version.ts; run ant packages/nix/generate.ts.
{ lib, gitRev ? "unknown" }:
let
  version = builtins.listToAttrs (builtins.map (line_1:
  let
    parts = lib.splitString "=" line_1;
in
  {
    name = lib.trim (lib.elemAt parts 0);
    value = lib.trim (lib.elemAt parts 1);
  }) (builtins.filter (line_2:
  let
    trimmed = lib.trim line_2;
in
  trimmed != "" && !((lib.hasPrefix "#" trimmed))) (lib.splitString "\n" (lib.fileContents ../../../meson/ant.version))));

  revision = if gitRev == "unknown" then gitRev else builtins.substring 0 8 gitRev;
in
"${version.major}.${version.minor}.${revision}.${version.patch}"
