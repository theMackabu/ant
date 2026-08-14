import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import urllib.request

RUSTUP_HOSTS = {
  ("Darwin", "arm64"): "aarch64-apple-darwin",
  ("Darwin", "x86_64"): "x86_64-apple-darwin",
  ("Linux", "aarch64"): "aarch64-unknown-linux-gnu",
  ("Linux", "x86_64"): "x86_64-unknown-linux-gnu",
  ("Windows", "AMD64"): "x86_64-pc-windows-msvc",
  ("Windows", "ARM64"): "aarch64-pc-windows-msvc",
}

parser = argparse.ArgumentParser()
parser.add_argument("--crate", required=True)
parser.add_argument("--temporal-capi", required=True)
parser.add_argument("--cache", required=True)
parser.add_argument("--target", required=True)
parser.add_argument("--out", required=True)
parser.add_argument("--features", default="")
args = parser.parse_args()

host = RUSTUP_HOSTS.get((platform.system(), platform.machine()))
if host is None:
  sys.exit(f"temporal.py: unsupported host {platform.system()}/{platform.machine()}")

cache = os.path.abspath(args.cache)
rustup_home = os.path.join(cache, "rustup")
cargo_home = os.path.join(cache, "cargo")
exe = ".exe" if platform.system() == "Windows" else ""
cargo = os.path.join(cargo_home, "bin", "cargo" + exe)

env = os.environ.copy()
env["RUSTUP_HOME"] = rustup_home
env["CARGO_HOME"] = cargo_home
env["CARGO_TARGET_DIR"] = os.path.join(cache, "target")
env["PATH"] = os.path.join(cargo_home, "bin") + os.pathsep + env.get("PATH", "")
env["RUSTFLAGS"] = "-Zunstable-options -Cpanic=immediate-abort"

if not os.path.exists(cargo):
  os.makedirs(cache, exist_ok=True)
  rustup_init = os.path.join(cache, "rustup-init" + exe)
  url = f"https://static.rust-lang.org/rustup/dist/{host}/rustup-init{exe}"
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

if args.features:
  build += ["--features", args.features]
subprocess.run(build, cwd=args.crate, env=env, check=True)

lib_name = "ant_temporal.lib" if "windows" in args.target else "libant_temporal.a"
built = os.path.join(env["CARGO_TARGET_DIR"], args.target, "release", lib_name)
shutil.copy2(built, args.out)
