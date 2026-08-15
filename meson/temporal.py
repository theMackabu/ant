import argparse
import json
import os
import shutil
import subprocess
import sys
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("--crate", required=True)
parser.add_argument("--temporal-capi", required=True)
parser.add_argument("--cache", required=True)
parser.add_argument("--rustup-host", required=True)
parser.add_argument("--target", required=True)
parser.add_argument("--out", required=True)
parser.add_argument("--features", default="")
args = parser.parse_args()

cache = os.path.join(os.path.abspath(args.cache), args.rustup_host)
rustup_home = os.path.join(cache, "rustup")
cargo_home = os.path.join(cache, "cargo")
exe = ".exe" if "windows" in args.rustup_host else ""
provided_cargo = os.environ.get("ANT_TEMPORAL_CARGO")
cargo = provided_cargo or os.path.join(cargo_home, "bin", "cargo" + exe)

env = os.environ.copy()
env["CARGO_TARGET_DIR"] = os.path.join(cache, "target")
env["RUSTFLAGS"] = "-Zunstable-options -Cpanic=immediate-abort"

if provided_cargo:
  if not os.path.isfile(cargo) or not os.access(cargo, os.X_OK):
    raise SystemExit(f"temporal.py: ANT_TEMPORAL_CARGO is not executable: {cargo}")
else:
  env["RUSTUP_HOME"] = rustup_home
  env["CARGO_HOME"] = cargo_home
  env["PATH"] = os.path.join(cargo_home, "bin") + os.pathsep + env.get("PATH", "")

if not provided_cargo and not os.path.exists(cargo):
  os.makedirs(cache, exist_ok=True)
  rustup_init = os.path.join(cache, "rustup-init" + exe)
  url = f"https://static.rust-lang.org/rustup/dist/{args.rustup_host}/rustup-init{exe}"
  print(f"temporal.py: fetching {url}", file=sys.stderr)
  urllib.request.urlretrieve(url, rustup_init)
  os.chmod(rustup_init, 0o755)
  subprocess.run(
    [rustup_init, "-y", "--quiet", "--no-modify-path",
     "--profile", "minimal", "--default-toolchain", "none"],
    env=env, check=True,
  )

# the rustup shim reads rust-toolchain.toml from the crate dir and installs
# the pinned nightly (with rust-src) on first use
build = [
  cargo,
  "--quiet",
  "--config", "patch.crates-io.temporal_capi.path=" + json.dumps(os.path.abspath(args.temporal_capi)),
  "build", "--release", "--target", args.target,
  "--locked",
  "-Zbuild-std=std,panic_abort", "-Zbuild-std-features=optimize_for_size",
]

if provided_cargo:
  build += ["--offline"]
if args.features:
  build += ["--features", args.features]
subprocess.run(build, cwd=args.crate, env=env, check=True)

lib_name = "ant_temporal.lib" if args.target.endswith("-windows-msvc") else "libant_temporal.a"
built = os.path.join(env["CARGO_TARGET_DIR"], args.target, "release", lib_name)
shutil.copy2(built, args.out)
