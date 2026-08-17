# antsay

A small CLI that exercises everything `ant compile` can pack into a standalone
executable: multi-file TypeScript, JSON and text imports, a CommonJS require,
a literal dynamic `import()`, and `child_process.fork` of an embedded module
via the `new URL("...", import.meta.url)` pattern.

## Run from source

```bash
ant main.ts say hello there
ant main.ts sysinfo
ant main.ts march 3
```

## Compile

```bash
ant compile main.ts -o antsay
```

While developing ant itself, point at a locally built runtime instead of
downloading one:

```bash
ant compile --runtime ../../build/ant-runtime main.ts -o antsay
```

Then run the binary from anywhere:

```bash
./antsay say packed into one file
./antsay sysinfo
./antsay march 3
```

`sysinfo` shows `process.execPath` pointing at the binary itself, and stack
traces inside a compiled binary use virtual `/$ant/...` paths instead of
build-machine paths.
