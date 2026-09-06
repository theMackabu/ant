# Generated from packages/nix/src/version.ts; run ant packages/nix/generate.ts.
({ lib, gitRev ? ("unknown") }: (let
  __tsnix_0_lib = lib;
  __tsnix_1_gitRev = gitRev;
in (let
  __tsnix_6_version = (((builtins).listToAttrs) ((((builtins).map) ((__tsnix_4_line: (let
  __tsnix_5_parts = (((__tsnix_0_lib).splitString) ("=") (__tsnix_4_line));
in {
  name = (((__tsnix_0_lib).trim) ((((__tsnix_0_lib).elemAt) (__tsnix_5_parts) (0))));
  value = (((__tsnix_0_lib).trim) ((((__tsnix_0_lib).elemAt) (__tsnix_5_parts) (1))));
}))) ((((builtins).filter) ((__tsnix_2_line: (let
  __tsnix_3_trimmed = (((__tsnix_0_lib).trim) (__tsnix_2_line));
in (__tsnix_3_trimmed) != "" && !(((((__tsnix_0_lib).hasPrefix) ("#") (__tsnix_3_trimmed))))))) ((((__tsnix_0_lib).splitString) ("\n") ((((__tsnix_0_lib).fileContents) (../../../meson/ant.version))))))))));
  __tsnix_7_revision = (if (((__tsnix_1_gitRev) == ("unknown"))) then (__tsnix_1_gitRev) else ((((builtins).substring) (0) (8) (__tsnix_1_gitRev))));
in "${(__tsnix_6_version).major}.${(__tsnix_6_version).minor}.${__tsnix_7_revision}.${(__tsnix_6_version).patch}")))
