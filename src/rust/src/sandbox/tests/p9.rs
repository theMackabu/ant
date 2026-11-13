use super::*;
use crate::sandbox::guest::testing::{TestMemory, BASE};
use crate::sandbox::vring::testing::{put_desc, DESC_BASE};
use crate::sandbox::vring::{DESC_F_NEXT, DESC_F_WRITE};
use std::os::unix::fs::PermissionsExt;
use std::sync::atomic::{AtomicU32, Ordering};

static NEXT_DIR: AtomicU32 = AtomicU32::new(0);

struct TempDir(PathBuf);

impl TempDir {
  fn new(label: &str) -> TempDir {
    let unique = format!("ant-9p-{label}-{}-{}", std::process::id(), NEXT_DIR.fetch_add(1, Ordering::Relaxed));
    let path = std::env::temp_dir().join(unique);
    fs::create_dir(&path).unwrap();
    TempDir(fs::canonicalize(path).unwrap())
  }

  fn join(&self, name: &str) -> PathBuf {
    self.0.join(name)
  }
}

impl Drop for TempDir {
  fn drop(&mut self) {
    let _ = fs::remove_dir_all(&self.0);
  }
}

fn server(root: &TempDir, readonly: bool) -> P9 {
  P9::new(root.0.as_os_str().as_bytes(), b"mount", readonly, MAX_MSIZE).unwrap()
}

fn request(kind: u8, body: &[u8]) -> Vec<u8> {
  let mut req = Vec::new();
  req.extend_from_slice(&((HEADER_LEN + body.len()) as u32).to_le_bytes());
  req.push(kind);
  req.extend_from_slice(&7u16.to_le_bytes());
  req.extend_from_slice(body);
  req
}

fn string(text: &[u8]) -> Vec<u8> {
  let mut out = (text.len() as u16).to_le_bytes().to_vec();
  out.extend_from_slice(text);
  out
}

fn call(p9: &mut P9, kind: u8, body: &[u8]) -> Vec<u8> {
  let mut resp = vec![0u8; p9.msize()];
  let len = p9.handle(&request(kind, body), &mut resp);
  resp.truncate(len);
  assert_eq!(u32::from_le_bytes([resp[0], resp[1], resp[2], resp[3]]) as usize, len);
  assert_eq!(u16::from_le_bytes([resp[5], resp[6]]), 7);
  resp
}

fn ok(p9: &mut P9, kind: u8, body: &[u8]) -> Vec<u8> {
  let resp = call(p9, kind, body);
  assert_eq!(resp[4], kind + 1, "request {kind} failed with errno {}", error_of(&resp));
  resp
}

fn error_of(resp: &[u8]) -> u32 {
  if resp[4] != RLERROR {
    return 0;
  }
  u32::from_le_bytes([resp[7], resp[8], resp[9], resp[10]])
}

fn fails(p9: &mut P9, kind: u8, body: &[u8]) -> u32 {
  let resp = call(p9, kind, body);
  assert_eq!(resp[4], RLERROR, "request {kind} unexpectedly succeeded");
  assert_eq!(resp.len(), ERROR_LEN);
  error_of(&resp)
}

fn attach(p9: &mut P9, fid: u32) {
  let mut body = fid.to_le_bytes().to_vec();
  body.extend_from_slice(&u32::MAX.to_le_bytes());
  body.extend_from_slice(&string(b""));
  body.extend_from_slice(&string(b""));
  body.extend_from_slice(&0u32.to_le_bytes());
  let resp = ok(p9, TATTACH, &body);
  assert_eq!((resp.len(), resp[7]), (20, QTDIR));
}

fn walk_body(fid: u32, newfid: u32, names: &[&[u8]]) -> Vec<u8> {
  let mut body = fid.to_le_bytes().to_vec();
  body.extend_from_slice(&newfid.to_le_bytes());
  body.extend_from_slice(&(names.len() as u16).to_le_bytes());
  for name in names {
    body.extend_from_slice(&string(name));
  }
  body
}

fn walk(p9: &mut P9, fid: u32, newfid: u32, names: &[&[u8]]) {
  let resp = ok(p9, TWALK, &walk_body(fid, newfid, names));
  assert_eq!(resp.len(), 9 + QID_LEN * names.len());
}

fn read_body(fid: u32, offset: u64, count: u32) -> Vec<u8> {
  let mut body = fid.to_le_bytes().to_vec();
  body.extend_from_slice(&offset.to_le_bytes());
  body.extend_from_slice(&count.to_le_bytes());
  body
}

fn read(p9: &mut P9, fid: u32, offset: u64, count: u32) -> Vec<u8> {
  let resp = ok(p9, TREAD, &read_body(fid, offset, count));
  let got = u32::from_le_bytes([resp[7], resp[8], resp[9], resp[10]]) as usize;
  assert_eq!(resp.len(), IO_HEADER_LEN + got);
  resp[IO_HEADER_LEN..].to_vec()
}

fn named(fid: u32, name: &[u8], tail: &[u32]) -> Vec<u8> {
  let mut body = fid.to_le_bytes().to_vec();
  body.extend_from_slice(&string(name));
  for value in tail {
    body.extend_from_slice(&value.to_le_bytes());
  }
  body
}

#[test]
fn relative_path_policy() {
  for bad in [&b"/x"[..], b"../x", b"a//b", b"a/./b", b"a/../b", b"a/", b".", b".."] {
    assert!(path_bad(bad), "{}", String::from_utf8_lossy(bad));
  }
  for good in [&b""[..], b"a", b"a/b", b"..a", b"a..b/c"] {
    assert!(!path_bad(good), "{}", String::from_utf8_lossy(good));
  }
  assert!(name_bad(b"") && name_bad(b"a/b") && name_bad(b".") && name_bad(b".."));
  assert!(!name_bad(b"file") && !name_bad(b"..."));
  for bad in [&b""[..], b"/abs", b"../x", b"a/../b", b"a//b"] {
    assert!(symlink_target_bad(bad));
  }
  assert!(!symlink_target_bad(b"target") && !symlink_target_bad(b"dir/file"));

  let root = TempDir::new("policy");
  let p9 = server(&root, false);
  assert_eq!(p9.existing_path(b""), Ok(root.0.clone()));
  for bad in [&b"../x"[..], b"/x", b"a//b", b"a/./b", b"a/../b"] {
    assert_eq!(p9.host_path(bad), Err(ENOENT));
  }
  assert_eq!(p9.host_path(b"new"), Ok(root.join("new")));
  assert!(P9::new(b"", b"t", false, MAX_MSIZE).is_none());
  assert!(P9::new(b"/", b"t", false, MIN_MSIZE - 1).is_none());
  assert!(P9::new(b"/", b"t", false, MAX_MSIZE + 1).is_none());
}

#[test]
fn symlinks_cannot_escape_the_root() {
  let root = TempDir::new("root");
  let outside = TempDir::new("outside");
  fs::write(outside.join("secret"), b"host secret").unwrap();
  std::os::unix::fs::symlink(&outside.0, root.join("escape")).unwrap();
  std::os::unix::fs::symlink(outside.join("secret"), root.join("leak")).unwrap();

  let mut p9 = server(&root, false);
  assert_eq!(p9.existing_path(b"escape"), Err(EPERM));
  assert_eq!(p9.child_path(b"escape", b"new-file"), Err(EPERM));

  attach(&mut p9, 1);
  walk(&mut p9, 1, 2, &[b"escape"]);
  assert_eq!(fails(&mut p9, TLCREATE, &named(2, b"new-file", &[0, 0o644, 0])), EPERM);
  assert_eq!(fails(&mut p9, TMKDIR, &named(2, b"dir", &[0o755, 0])), EPERM);
  assert_eq!(fails(&mut p9, TWALK, &walk_body(1, 3, &[b"escape", b"secret"])), EPERM);
  assert!(!outside.join("new-file").exists());

  walk(&mut p9, 1, 4, &[b"leak"]);
  assert_eq!(fails(&mut p9, TREAD, &read_body(4, 0, 64)), ELOOP);
  let mut write = read_body(4, 0, 3);
  write.extend_from_slice(b"own");
  assert_eq!(fails(&mut p9, TWRITE, &write), EPERM);
  assert_eq!(fs::read(outside.join("secret")).unwrap(), b"host secret");

  let mut target = named(1, b"up", &[]);
  target.extend_from_slice(&string(b"../outside"));
  target.extend_from_slice(&0u32.to_le_bytes());
  assert_eq!(fails(&mut p9, TSYMLINK, &target), EPERM);
  walk(&mut p9, 1, 5, &[b"..", b"..", b".."]);
  assert_eq!(p9.fid_path(5), Ok(Vec::new()));
}

#[test]
fn walking_up_stops_at_the_root() {
  let root = TempDir::new("up");
  fs::create_dir(root.join("sub")).unwrap();
  let mut p9 = server(&root, true);
  attach(&mut p9, 1);
  walk(&mut p9, 1, 2, &[b"sub", b"..", b"..", b".."]);
  assert_eq!(p9.fid_path(2), Ok(Vec::new()));
  assert_eq!(fails(&mut p9, TWALK, &walk_body(1, 3, &[b"sub/.."])), ENOENT);
  assert_eq!(fails(&mut p9, TWALK, &walk_body(1, 3, &[b"missing"])), ENOENT);
  assert_eq!(fails(&mut p9, TWALK, &walk_body(9, 3, &[])), ENOENT);
  assert_eq!(fails(&mut p9, TWALK, &walk_body(1, 3, &[b"a\0b"])), EINVAL);
}

#[test]
fn read_count_is_bounded_by_the_response_buffer() {
  for readonly in [true, false] {
    let root = TempDir::new("read");
    fs::write(root.join("payload"), vec![0x5au8; MAX_MSIZE + 13]).unwrap();
    let mut p9 = server(&root, readonly);
    let mut version = (MAX_MSIZE as u32).to_le_bytes().to_vec();
    version.extend_from_slice(&string(b"9P2000.L"));
    ok(&mut p9, TVERSION, &version);
    assert_eq!(p9.msize(), MAX_MSIZE);

    attach(&mut p9, 1);
    walk(&mut p9, 1, 42, &[b"payload"]);
    let mut resp = vec![0xa5u8; MAX_MSIZE + 32];
    let len = p9.handle(&request(TREAD, &read_body(42, 0, u32::MAX)), &mut resp[..MAX_MSIZE]);
    assert_eq!(len, MAX_MSIZE);
    assert_eq!(resp[4], TREAD + 1);
    assert_eq!(u32::from_le_bytes([resp[7], resp[8], resp[9], resp[10]]) as usize, MAX_MSIZE - IO_HEADER_LEN);
    assert!(resp[IO_HEADER_LEN..MAX_MSIZE].iter().all(|&b| b == 0x5a));
    assert!(resp[MAX_MSIZE..].iter().all(|&b| b == 0xa5));

    assert_eq!(read(&mut p9, 42, (MAX_MSIZE + 10) as u64, 100), vec![0x5a; 3]);
    assert!(read(&mut p9, 42, (MAX_MSIZE + 13) as u64, 100).is_empty());
    if readonly {
      assert!(read(&mut p9, 42, u64::MAX, 100).is_empty());
    } else {
      assert_eq!(fails(&mut p9, TREAD, &read_body(42, u64::MAX, 100)), EINVAL);
    }
  }
}

#[test]
fn walk_clones_a_fid_among_many() {
  let root = TempDir::new("walk");
  fs::create_dir(root.join("sub")).unwrap();
  let mut p9 = server(&root, true);
  attach(&mut p9, 0);
  walk(&mut p9, 0, 0, &[b"sub"]);
  for fid in 1..600 {
    attach(&mut p9, fid);
  }
  walk(&mut p9, 0, 4096, &[]);
  assert_eq!(p9.fid_path(4096), Ok(b"sub".to_vec()));

  ok(&mut p9, TCLUNK, &4096u32.to_le_bytes());
  assert_eq!(p9.fid_path(4096), Err(ENOENT));
  p9.reset();
  assert_eq!(p9.fid_path(0), Err(ENOENT));
}

#[test]
fn write_count_must_fit_the_request() {
  let root = TempDir::new("write");
  fs::write(root.join("payload"), b"").unwrap();
  let mut p9 = server(&root, false);
  attach(&mut p9, 1);
  walk(&mut p9, 1, 42, &[b"payload"]);
  assert_eq!(fails(&mut p9, TWRITE, &read_body(42, 0, u32::MAX)), EINVAL);
  let mut short = read_body(42, 0, 5);
  short.extend_from_slice(b"abcd");
  assert_eq!(fails(&mut p9, TWRITE, &short), EINVAL);
  assert_eq!(fs::metadata(root.join("payload")).unwrap().len(), 0);
}

#[test]
fn create_write_read_rename_and_remove() {
  let root = TempDir::new("rw");
  let mut p9 = server(&root, false);
  attach(&mut p9, 1);

  let resp = ok(&mut p9, TMKDIR, &named(1, b"dir", &[0o750, 0]));
  assert_eq!((resp.len(), resp[7]), (20, QTDIR));
  assert_eq!(fs::metadata(root.join("dir")).unwrap().permissions().mode() & 0o777, 0o750);

  walk(&mut p9, 1, 2, &[b"dir"]);
  let resp = ok(&mut p9, TLCREATE, &named(2, b"file", &[0, 0o640, 0]));
  assert_eq!(resp.len(), 24);
  let io_unit = u32::from_le_bytes([resp[20], resp[21], resp[22], resp[23]]) as usize;
  assert_eq!(io_unit, p9.msize() - 24);
  assert!(HEADER_LEN + 16 + io_unit <= p9.msize());
  let mut resp = [0u8; 32];
  assert_eq!(p9.reject_oversized(&request(TWRITE, &[0u8; 16]), &mut resp), ERROR_LEN);
  assert_eq!((resp[4], resp[5], error_of(&resp)), (RLERROR, 7, EMSGSIZE));
  assert_eq!(p9.fid_path(2), Ok(b"dir/file".to_vec()));
  assert_eq!(fails(&mut p9, TLCREATE, &named(1, b"dir", &[0, 0o640, 0])), 17);

  let mut write = read_body(2, 0, 11);
  write.extend_from_slice(b"hello world");
  let resp = ok(&mut p9, TWRITE, &write);
  assert_eq!(u32::from_le_bytes([resp[7], resp[8], resp[9], resp[10]]), 11);
  assert_eq!(read(&mut p9, 2, 6, 100), b"world");

  let mut getattr = 2u32.to_le_bytes().to_vec();
  getattr.extend_from_slice(&GETATTR_BASIC.to_le_bytes());
  let resp = ok(&mut p9, TGETATTR, &getattr);
  assert_eq!(resp.len(), 143);
  assert_eq!(u32::from_le_bytes([resp[28], resp[29], resp[30], resp[31]]) & S_IFMT, S_IFREG);
  assert_eq!(u64::from_le_bytes(resp[56..64].try_into().unwrap()), 11);

  let mut setattr = 2u32.to_le_bytes().to_vec();
  setattr.extend_from_slice(&SETATTR_SIZE.to_le_bytes());
  setattr.extend_from_slice(&[0u8; 12]);
  setattr.extend_from_slice(&5u64.to_le_bytes());
  setattr.extend_from_slice(&[0u8; 32]);
  ok(&mut p9, TSETATTR, &setattr);
  assert_eq!(fs::read(root.join("dir/file")).unwrap(), b"hello");
  setattr[4..8].copy_from_slice(&1u32.to_le_bytes());
  assert_eq!(fails(&mut p9, TSETATTR, &setattr), ENOTSUP);

  let mut link = named(1, b"link", &[]);
  link.extend_from_slice(&string(b"dir/file"));
  link.extend_from_slice(&0u32.to_le_bytes());
  let resp = ok(&mut p9, TSYMLINK, &link);
  assert_eq!(resp[7], QTSYMLINK);
  walk(&mut p9, 1, 3, &[b"link"]);
  let resp = ok(&mut p9, TREADLINK, &3u32.to_le_bytes());
  assert_eq!(&resp[9..], b"dir/file");

  let mut rename = named(1, b"dir", &[1]);
  rename.extend_from_slice(&string(b"moved"));
  ok(&mut p9, TRENAMEAT, &rename);
  assert_eq!(p9.fid_path(2), Ok(b"moved/file".to_vec()));
  assert_eq!(read(&mut p9, 2, 0, 100), b"hello");

  let mut readdir = read_body(1, 0, 4096);
  let resp = ok(&mut p9, TREADDIR, &readdir);
  let listing = &resp[IO_HEADER_LEN..];
  let mut names = Vec::new();
  let mut at = 0;
  let mut last_offset = 0;
  while at < listing.len() {
    last_offset = u64::from_le_bytes(listing[at + 13..at + 21].try_into().unwrap());
    let len = usize::from(u16::from_le_bytes([listing[at + 22], listing[at + 23]]));
    names.push((listing[at + 24..at + 24 + len].to_vec(), listing[at + 21]));
    at += DIRENT_FIXED_LEN + len;
  }
  names.sort();
  assert_eq!(names, [(b"link".to_vec(), DT_LNK), (b"moved".to_vec(), DT_DIR)]);
  assert_eq!(last_offset, 2);
  readdir[4..12].copy_from_slice(&2u64.to_le_bytes());
  assert_eq!(ok(&mut p9, TREADDIR, &readdir).len(), IO_HEADER_LEN);
  readdir[4..12].copy_from_slice(&0u64.to_le_bytes());
  readdir[12..16].copy_from_slice(&30u32.to_le_bytes());
  let one = ok(&mut p9, TREADDIR, &readdir).len() - IO_HEADER_LEN - DIRENT_FIXED_LEN;
  assert!(one == 4 || one == 5, "expected exactly one entry, got {one} name bytes");
  assert_eq!(fails(&mut p9, TREADDIR, &read_body(2, 0, 4096)), ENOTDIR);

  assert_eq!(fails(&mut p9, TUNLINKAT, &named(1, b"moved", &[AT_REMOVEDIR])), 39);
  walk(&mut p9, 1, 4, &[b"moved"]);
  ok(&mut p9, TUNLINKAT, &named(4, b"file", &[0]));
  ok(&mut p9, TUNLINKAT, &named(1, b"moved", &[AT_REMOVEDIR]));
  ok(&mut p9, TUNLINKAT, &named(1, b"link", &[0]));
  assert_eq!(fs::read_dir(&root.0).unwrap().count(), 0);

  assert_eq!(ok(&mut p9, TSTATFS, &1u32.to_le_bytes()).len(), 61);
  let mut fsync = 1u32.to_le_bytes().to_vec();
  fsync.extend_from_slice(&0u32.to_le_bytes());
  assert_eq!(ok(&mut p9, TFSYNC, &fsync).len(), HEADER_LEN);
  assert_eq!(fails(&mut p9, 200, &[]), ENOSYS);
}

#[test]
fn readonly_mounts_reject_mutation() {
  let root = TempDir::new("ro");
  fs::write(root.join("file"), b"data").unwrap();
  let mut p9 = server(&root, true);
  attach(&mut p9, 1);
  walk(&mut p9, 1, 2, &[b"file"]);
  let mut write = read_body(2, 0, 1);
  write.push(b'x');
  assert_eq!(fails(&mut p9, TWRITE, &write), EROFS);
  assert_eq!(fails(&mut p9, TLCREATE, &named(1, b"new", &[0, 0o644, 0])), EROFS);
  assert_eq!(fails(&mut p9, TMKDIR, &named(1, b"new", &[0o755, 0])), EROFS);
  assert_eq!(fails(&mut p9, TMKNOD, &named(1, b"new", &[S_IFREG | 0o644, 0, 0, 0])), EROFS);
  assert_eq!(fails(&mut p9, TUNLINKAT, &named(1, b"file", &[0])), EROFS);
  let mut rename = named(1, b"file", &[1]);
  rename.extend_from_slice(&string(b"other"));
  assert_eq!(fails(&mut p9, TRENAMEAT, &rename), EROFS);
  assert_eq!(read(&mut p9, 2, 0, 100), b"data");
  assert_eq!(read(&mut p9, 2, 1, 2), b"at");
  assert_eq!(p9.stats.file_cache_hits, 1);
  assert_eq!(fs::read(root.join("file")).unwrap(), b"data");
}

#[test]
fn version_clamps_msize_and_short_requests_fail() {
  let root = TempDir::new("version");
  let mut p9 = server(&root, true);
  assert_eq!(p9.msize(), DEFAULT_MSIZE);
  for (asked, expected) in [(100u32, MIN_MSIZE), (16384, 16384), (u32::MAX, MAX_MSIZE)] {
    let mut body = asked.to_le_bytes().to_vec();
    body.extend_from_slice(&string(b"9P2000.L"));
    let mut resp = vec![0u8; MAX_MSIZE];
    let len = p9.handle(&request(TVERSION, &body), &mut resp);
    assert_eq!(len, 13 + 8);
    assert_eq!(u32::from_le_bytes([resp[7], resp[8], resp[9], resp[10]]) as usize, expected);
    assert_eq!(&resp[13..len], b"9P2000.L");
    assert_eq!(p9.msize(), expected);
  }

  let mut resp = [0u8; 64];
  assert_eq!(p9.handle(&[0u8; 6], &mut resp), 0);
  assert_eq!(p9.handle(&request(TATTACH, &[]), &mut resp[..10]), 0);
  assert_eq!(p9.serve(&[0u8; 6], &mut resp), ERROR_LEN);
  assert_eq!((resp[4], error_of(&resp)), (RLERROR, EIO));
  for kind in [TVERSION, TATTACH, TWALK, TGETATTR, TLOPEN, TREAD, TREADDIR, TREADLINK, TFSYNC] {
    assert_eq!(fails(&mut p9, kind, &[1, 0]), EINVAL, "kind {kind}");
  }
}

#[test]
fn stat_cache_is_bounded() {
  let root = TempDir::new("cache");
  let mut p9 = server(&root, true);
  attach(&mut p9, 1);
  for index in 0..MAX_STAT_CACHE_ENTRIES + 10 {
    let name = format!("missing-{index}");
    assert_eq!(p9.stat(name.as_bytes()), Err(ENOENT));
  }
  assert!(p9.stat_cache.len() <= MAX_STAT_CACHE_ENTRIES);
  assert_eq!(p9.stats.stat_cache_clears, 1);
  let mut lines = Vec::new();
  p9.report(0, |line| lines.push(line.to_string()));
  assert!(lines[0].starts_with("9p[0] summary tag=mount root="));
  assert!(lines.len() >= 3 && lines.len() <= 2 + REPORT_TOP_COUNT);
}

#[test]
fn host_errors_use_linux_numbers() {
  assert_eq!(linux_errno(libc::ENOENT), 2);
  assert_eq!(linux_errno(libc::ENOTEMPTY), 39);
  assert_eq!(linux_errno(libc::ELOOP), 40);
  assert_eq!(linux_errno(libc::ENAMETOOLONG), 36);
  assert_eq!(linux_errno(libc::EAGAIN), 11);
  assert_eq!(linux_errno(libc::ENOTSUP), 95);
  assert_eq!(linux_errno(-1), EIO);
}

#[test]
fn request_chains_split_readable_and_writable_descriptors() {
  let mem = TestMemory::new(0x8000);
  mem.put(BASE + 0x2000, b"abcd");
  mem.put(BASE + 0x2100, b"efg");
  put_desc(&mem, 0, BASE + 0x2000, 4, DESC_F_NEXT, 1);
  put_desc(&mem, 1, BASE + 0x2100, 3, DESC_F_NEXT, 2);
  put_desc(&mem, 2, BASE + 0x3000, 4, DESC_F_WRITE | DESC_F_NEXT, 3);
  put_desc(&mem, 3, BASE + 0x3100, 8, DESC_F_WRITE, 0);
  let chain = read_chain(&mem, DESC_BASE, 0, 32, 64).unwrap();
  assert_eq!(chain.request, b"abcdefg");
  assert_eq!(chain.writes, [(BASE + 0x3000, 4), (BASE + 0x3100, 8)]);
  let clipped = read_chain(&mem, DESC_BASE, 0, 32, 6).unwrap();
  assert!(clipped.oversized);
  assert_eq!(clipped.request, b"abcdef");
  assert_eq!(clipped.writes.len(), 2);
  assert!(!chain.oversized);

  assert_eq!(write_response(&mem, &chain.writes, b"0123456789"), Ok(()));
  assert_eq!(mem.get::<4>(BASE + 0x3000), *b"0123");
  assert_eq!(mem.get::<6>(BASE + 0x3100), *b"456789");
  assert_eq!(write_response(&mem, &chain.writes, &[0u8; 13]), Err(DEVICE_ENOSPC));

  for index in 0..10u16 {
    put_desc(&mem, index, BASE + 0x3000, 1, DESC_F_WRITE | DESC_F_NEXT, index + 1);
  }
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 32, 64).err(), Some(E2BIG));
}
