use super::errno::{Errno, Result as DeviceResult, ENOSPC as DEVICE_ENOSPC};
use super::guest::GuestMemory;
use super::vring::read_desc;
use std::collections::HashMap;
use std::ffi::{CString, OsStr, OsString};
use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{DirBuilderExt, FileExt, FileTypeExt, MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::time::Instant;

pub(crate) const DEFAULT_MSIZE: usize = 8192;
pub(crate) const MAX_MSIZE: usize = 64 * 1024;
pub(crate) const MIN_MSIZE: usize = 4096;
const PATH_MAX: usize = 1024;
const HOST_PATH_MAX: usize = 4096;
const MAX_FIDS: usize = 65536;
const MAX_STAT_CACHE_ENTRIES: usize = 16384;
const TOP_PATH_COUNT: usize = 64;
const TOP_PATH_LEN: usize = 160;
const REPORT_TOP_COUNT: usize = 8;
const FILE_CACHE_MAX_FILE: u64 = 256 * 1024;
const FILE_CACHE_MAX_BYTES: usize = 16 * 1024 * 1024;
const MAX_WRITE_IOV: usize = 8;
const HEADER_LEN: usize = 7;
const ERROR_LEN: usize = 11;
#[cfg(test)]
const IO_HEADER_LEN: usize = 11;
const IO_UNIT_RESERVE: usize = 24;
#[cfg(test)]
const QID_LEN: usize = 13;
const DIRENT_FIXED_LEN: usize = 24;

const RLERROR: u8 = 7;
const TSTATFS: u8 = 8;
const TLOPEN: u8 = 12;
const TLCREATE: u8 = 14;
const TSYMLINK: u8 = 16;
const TMKNOD: u8 = 18;
const TREADLINK: u8 = 22;
const TGETATTR: u8 = 24;
const TSETATTR: u8 = 26;
const TREADDIR: u8 = 40;
const TFSYNC: u8 = 50;
const TMKDIR: u8 = 72;
const TRENAMEAT: u8 = 74;
const TUNLINKAT: u8 = 76;
const TVERSION: u8 = 100;
const TATTACH: u8 = 104;
const TWALK: u8 = 110;
const TREAD: u8 = 116;
const TWRITE: u8 = 118;
const TCLUNK: u8 = 120;

const QTSYMLINK: u8 = 0x02;
const QTDIR: u8 = 0x80;
const GETATTR_BASIC: u64 = 0x7ff;
const SETATTR_SIZE: u32 = 1 << 3;
const AT_REMOVEDIR: u32 = 0x200;

const S_IFMT: u32 = 0o170000;
const S_IFIFO: u32 = 0o010000;
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;

const DT_FIFO: u8 = 1;
const DT_CHR: u8 = 2;
const DT_DIR: u8 = 4;
const DT_BLK: u8 = 6;
const DT_REG: u8 = 8;
const DT_LNK: u8 = 10;
const DT_SOCK: u8 = 12;

const EPERM: u32 = 1;
const ENOENT: u32 = 2;
const EIO: u32 = 5;
const ENOTDIR: u32 = 20;
const EISDIR: u32 = 21;
const EINVAL: u32 = 22;
const ENOSPC: u32 = 28;
const EROFS: u32 = 30;
const ENAMETOOLONG: u32 = 36;
const ENOSYS: u32 = 38;
const ELOOP: u32 = 40;
const EMSGSIZE: u32 = 90;
const ENOTSUP: u32 = 95;

type P9Result<T> = core::result::Result<T, u32>;

#[cfg(target_os = "linux")]
fn linux_errno(host: i32) -> u32 {
  u32::try_from(host).unwrap_or(EIO)
}

#[cfg(not(target_os = "linux"))]
fn linux_errno(host: i32) -> u32 {
  match host {
    1..=10 | 12..=34 => host as u32,
    libc::EDEADLK => 35,
    libc::EAGAIN => 11,
    libc::ELOOP => ELOOP,
    libc::ENAMETOOLONG => ENAMETOOLONG,
    libc::ENOTEMPTY => 39,
    libc::ENOSYS => ENOSYS,
    libc::ENOLCK => 37,
    libc::EOVERFLOW => 75,
    libc::EDQUOT => 122,
    libc::ESTALE => 116,
    libc::ENOTSUP | libc::EOPNOTSUPP => ENOTSUP,
    _ => EIO,
  }
}

fn io_errno(err: &io::Error) -> u32 {
  linux_errno(err.raw_os_error().unwrap_or(libc::EIO))
}

fn hash(path: &[u8]) -> u64 {
  let mut h = 1469598103934665603u64;
  for &byte in path {
    h ^= u64::from(byte);
    h = h.wrapping_mul(1099511628211);
  }
  h.max(1)
}

pub(crate) fn path_bad(path: &[u8]) -> bool {
  if path.is_empty() {
    return false;
  }
  path.first() == Some(&b'/') || path.split(|&b| b == b'/').any(|part| part.is_empty() || part == b"." || part == b"..")
}

pub(crate) fn name_bad(name: &[u8]) -> bool {
  name.is_empty() || name.contains(&b'/') || name == b"." || name == b".."
}

pub(crate) fn symlink_target_bad(target: &[u8]) -> bool {
  target.is_empty() || path_bad(target)
}

fn join_rel(parent: &[u8], name: &[u8]) -> P9Result<Vec<u8>> {
  let mut out = Vec::with_capacity(parent.len() + 1 + name.len());
  if !parent.is_empty() {
    out.extend_from_slice(parent);
    out.push(b'/');
  }
  out.extend_from_slice(name);
  if out.len() >= PATH_MAX {
    return Err(ENAMETOOLONG);
  }
  if path_bad(&out) {
    return Err(EINVAL);
  }
  Ok(out)
}

fn checked_host(path: PathBuf) -> P9Result<PathBuf> {
  if path.as_os_str().len() >= HOST_PATH_MAX {
    return Err(ENAMETOOLONG);
  }
  Ok(path)
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct StatInfo {
  mode: u32,
  nlink: u64,
  size: u64,
  blocks: u64,
  atime: i64,
  mtime: i64,
  ctime: i64,
}

impl StatInfo {
  fn of(meta: &fs::Metadata) -> StatInfo {
    StatInfo {
      mode: meta.mode(),
      nlink: meta.nlink(),
      size: meta.size(),
      blocks: meta.blocks(),
      atime: meta.atime(),
      mtime: meta.mtime(),
      ctime: meta.ctime(),
    }
  }

  fn is_dir(&self) -> bool {
    self.mode & S_IFMT == S_IFDIR
  }

  fn is_symlink(&self) -> bool {
    self.mode & S_IFMT == S_IFLNK
  }

  fn is_file(&self) -> bool {
    self.mode & S_IFMT == S_IFREG
  }
}

fn qid_type(mode: u32) -> u8 {
  match mode & S_IFMT {
    S_IFDIR => QTDIR,
    S_IFLNK => QTSYMLINK,
    _ => 0,
  }
}

struct In<'a> {
  buf: &'a [u8],
  pos: usize,
}

impl<'a> In<'a> {
  fn new(buf: &'a [u8]) -> In<'a> {
    In { buf, pos: HEADER_LEN }
  }

  fn need(&self, len: usize) -> P9Result<()> {
    if self.buf.len() < len {
      return Err(EINVAL);
    }
    Ok(())
  }

  fn take(&mut self, len: usize) -> P9Result<&'a [u8]> {
    let end = self.pos.checked_add(len).ok_or(EINVAL)?;
    let bytes = self.buf.get(self.pos..end).ok_or(EINVAL)?;
    self.pos = end;
    Ok(bytes)
  }

  fn u16(&mut self) -> P9Result<u16> {
    let bytes = self.take(2)?;
    Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
  }

  fn u32(&mut self) -> P9Result<u32> {
    let bytes = self.take(4)?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
  }

  fn u64(&mut self) -> P9Result<u64> {
    Ok(u64::from(self.u32()?) | (u64::from(self.u32()?) << 32))
  }

  fn name(&mut self) -> P9Result<&'a [u8]> {
    let len = usize::from(self.u16()?);
    if len >= PATH_MAX {
      return Err(EINVAL);
    }
    let bytes = self.take(len)?;
    if bytes.contains(&0) {
      return Err(EINVAL);
    }
    Ok(bytes)
  }

  fn rest(&self) -> &'a [u8] {
    self.buf.get(self.pos..).unwrap_or(&[])
  }
}

struct Out<'a> {
  buf: &'a mut [u8],
  len: usize,
}

impl<'a> Out<'a> {
  fn new(buf: &'a mut [u8]) -> Out<'a> {
    Out { buf, len: HEADER_LEN }
  }

  fn room(&self) -> usize {
    self.buf.len().saturating_sub(self.len)
  }

  fn put(&mut self, bytes: &[u8]) -> P9Result<()> {
    let end = self.len.checked_add(bytes.len()).ok_or(ENOSPC)?;
    self.buf.get_mut(self.len..end).ok_or(ENOSPC)?.copy_from_slice(bytes);
    self.len = end;
    Ok(())
  }

  fn u8(&mut self, value: u8) -> P9Result<()> {
    self.put(&[value])
  }

  fn u16(&mut self, value: u16) -> P9Result<()> {
    self.put(&value.to_le_bytes())
  }

  fn u32(&mut self, value: u32) -> P9Result<()> {
    self.put(&value.to_le_bytes())
  }

  fn u64(&mut self, value: u64) -> P9Result<()> {
    self.put(&value.to_le_bytes())
  }

  fn qid(&mut self, kind: u8, path: &[u8]) -> P9Result<()> {
    self.u8(kind)?;
    self.u32(0)?;
    self.u64(hash(path))
  }

  fn spare(&mut self, len: usize) -> P9Result<&mut [u8]> {
    let end = self.len.checked_add(len).ok_or(ENOSPC)?;
    self.buf.get_mut(self.len..end).ok_or(ENOSPC)
  }

  fn patch_u16(&mut self, at: usize, value: u16) {
    if let Some(field) = self.buf.get_mut(at..at + 2) {
      field.copy_from_slice(&value.to_le_bytes());
    }
  }

  fn patch_u32(&mut self, at: usize, value: u32) {
    if let Some(field) = self.buf.get_mut(at..at + 4) {
      field.copy_from_slice(&value.to_le_bytes());
    }
  }

  fn finish(mut self, kind: u8, tag: u16) -> usize {
    let len = self.len;
    self.patch_u32(0, len as u32);
    if let Some(slot) = self.buf.get_mut(4) {
      *slot = kind;
    }
    self.patch_u16(5, tag);
    len
  }
}

fn write_error(resp: &mut [u8], tag: u16, ecode: u32) -> usize {
  let mut out = Out::new(resp);
  if out.u32(ecode).is_err() {
    return 0;
  }
  out.finish(RLERROR, tag)
}

#[derive(Clone, Default)]
struct PathStat {
  used: bool,
  hash: u64,
  label: String,
  count: u64,
  stat_hits: u64,
  stat_misses: u64,
  reads: u64,
  readdirs: u64,
  bytes: u64,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Touch {
  StatHit,
  StatMiss,
  Read,
  Readdir,
}

struct Stats {
  requests: u64,
  errors: u64,
  op_counts: [u64; 256],
  stat_hits: u64,
  stat_misses: u64,
  stat_cache_clears: u64,
  read_count: u64,
  read_bytes: u64,
  readdir_count: u64,
  readdir_bytes: u64,
  write_count: u64,
  write_bytes: u64,
  file_cache_hits: u64,
  file_cache_misses: u64,
  file_cache_bypasses: u64,
  total_ns: u64,
  max_ns: u64,
  max_type: u8,
  paths: Vec<PathStat>,
}

impl Default for Stats {
  fn default() -> Stats {
    Stats {
      requests: 0,
      errors: 0,
      op_counts: [0; 256],
      stat_hits: 0,
      stat_misses: 0,
      stat_cache_clears: 0,
      read_count: 0,
      read_bytes: 0,
      readdir_count: 0,
      readdir_bytes: 0,
      write_count: 0,
      write_bytes: 0,
      file_cache_hits: 0,
      file_cache_misses: 0,
      file_cache_bypasses: 0,
      total_ns: 0,
      max_ns: 0,
      max_type: 0,
      paths: vec![PathStat::default(); TOP_PATH_COUNT],
    }
  }
}

fn path_label(path: &[u8]) -> String {
  if path.is_empty() {
    return ".".to_string();
  }
  let text = String::from_utf8_lossy(path);
  if text.len() < TOP_PATH_LEN {
    return text.into_owned();
  }
  let keep = TOP_PATH_LEN - 4;
  let mut start = text.len() - keep;
  while !text.is_char_boundary(start) {
    start += 1;
  }
  format!("...{}", &text[start..])
}

fn type_name(kind: u8) -> &'static str {
  match kind {
    TVERSION => "version",
    TATTACH => "attach",
    TWALK => "walk",
    TGETATTR => "getattr",
    TLOPEN => "lopen",
    TREAD => "read",
    TWRITE => "write",
    TREADDIR => "readdir",
    TREADLINK => "readlink",
    TLCREATE => "lcreate",
    TMKDIR => "mkdir",
    TMKNOD => "mknod",
    TSYMLINK => "symlink",
    TSETATTR => "setattr",
    TFSYNC => "fsync",
    TRENAMEAT => "renameat",
    TUNLINKAT => "unlinkat",
    TSTATFS => "statfs",
    TCLUNK => "clunk",
    _ => "unknown",
  }
}

impl Stats {
  fn touch(&mut self, path: &[u8], what: Touch, bytes: u64) {
    let key = hash(path);
    let first = (key % TOP_PATH_COUNT as u64) as usize;
    let mut chosen = first;
    let mut lowest = u64::MAX;
    let mut found = false;
    for probe in 0..TOP_PATH_COUNT {
      let index = (first + probe) % TOP_PATH_COUNT;
      let Some(entry) = self.paths.get(index) else { continue };
      if !entry.used || entry.hash == key {
        chosen = index;
        found = entry.used;
        break;
      }
      if entry.count < lowest {
        lowest = entry.count;
        chosen = index;
      }
    }
    let Some(entry) = self.paths.get_mut(chosen) else { return };
    if !found {
      *entry = PathStat { used: true, hash: key, label: path_label(path), ..PathStat::default() };
    }
    entry.count += 1;
    match what {
      Touch::StatHit => entry.stat_hits += 1,
      Touch::StatMiss => entry.stat_misses += 1,
      Touch::Read => entry.reads += 1,
      Touch::Readdir => entry.readdirs += 1,
    }
    entry.bytes += bytes;
  }
}

type StatOutcome = P9Result<(StatInfo, PathBuf)>;

pub struct P9 {
  root: PathBuf,
  tag: CString,
  readonly: bool,
  msize: usize,
  capacity: usize,
  fids: HashMap<u32, Vec<u8>>,
  stat_cache: HashMap<Vec<u8>, StatOutcome>,
  file_cache: HashMap<Vec<u8>, Vec<u8>>,
  file_cache_bytes: usize,
  stats: Stats,
}

impl P9 {
  pub(crate) fn new(root: &[u8], tag: &[u8], readonly: bool, capacity: usize) -> Option<P9> {
    if !(MIN_MSIZE..=MAX_MSIZE).contains(&capacity) || root.is_empty() {
      return None;
    }
    Some(P9 {
      root: PathBuf::from(OsString::from_vec(root.to_vec())),
      tag: CString::new(tag).ok()?,
      readonly,
      msize: capacity.min(DEFAULT_MSIZE),
      capacity,
      fids: HashMap::new(),
      stat_cache: HashMap::new(),
      file_cache: HashMap::new(),
      file_cache_bytes: 0,
      stats: Stats::default(),
    })
  }

  pub(crate) fn tag(&self) -> &[u8] {
    self.tag.as_bytes()
  }

  pub(crate) fn msize(&self) -> usize {
    self.msize
  }

  pub(crate) fn reset(&mut self) {
    self.fids.clear();
  }

  fn clear_stat_cache(&mut self) {
    if !self.stat_cache.is_empty() {
      self.stats.stat_cache_clears += 1;
      self.stat_cache.clear();
    }
  }

  fn under_root(&self, path: &Path) -> bool {
    self.root == Path::new("/") || path.starts_with(&self.root)
  }

  fn raw_path(&self, rel: &[u8]) -> P9Result<PathBuf> {
    if path_bad(rel) {
      return Err(ENOENT);
    }
    let mut path = self.root.clone();
    if !rel.is_empty() {
      path.push(OsStr::from_bytes(rel));
    }
    checked_host(path)
  }

  fn existing_path(&self, rel: &[u8]) -> P9Result<PathBuf> {
    let raw = self.raw_path(rel)?;
    let resolved = fs::canonicalize(raw).map_err(|err| io_errno(&err))?;
    if !self.under_root(&resolved) {
      return Err(EPERM);
    }
    checked_host(resolved)
  }

  fn host_path(&self, rel: &[u8]) -> P9Result<PathBuf> {
    if rel.is_empty() {
      return self.existing_path(rel);
    }
    if path_bad(rel) {
      return Err(ENOENT);
    }
    let (parent, name) = match rel.iter().rposition(|&b| b == b'/') {
      Some(slash) => (&rel[..slash], &rel[slash + 1..]),
      None => (&rel[..0], rel),
    };
    if name_bad(name) {
      return Err(ENOENT);
    }
    let mut host = self.existing_path(parent)?;
    host.push(OsStr::from_bytes(name));
    checked_host(host)
  }

  fn child_path(&self, parent_rel: &[u8], name: &[u8]) -> P9Result<PathBuf> {
    if name_bad(name) {
      return Err(EINVAL);
    }
    let mut host = self.existing_path(parent_rel)?;
    let meta = fs::symlink_metadata(&host).map_err(|err| io_errno(&err))?;
    if !meta.is_dir() {
      return Err(ENOTDIR);
    }
    host.push(OsStr::from_bytes(name));
    checked_host(host)
  }

  fn stat_cached(&mut self, rel: &[u8]) -> StatOutcome {
    if path_bad(rel) {
      return Err(ENOENT);
    }
    if let Some(cached) = self.stat_cache.get(rel) {
      let cached = cached.clone();
      self.stats.stat_hits += 1;
      self.stats.touch(rel, Touch::StatHit, 0);
      return cached;
    }

    let outcome = self.host_path(rel).and_then(|host| {
      let meta = fs::symlink_metadata(&host).map_err(|err| io_errno(&err))?;
      Ok((StatInfo::of(&meta), host))
    });
    self.stats.stat_misses += 1;
    self.stats.touch(rel, Touch::StatMiss, 0);
    if self.stat_cache.len() >= MAX_STAT_CACHE_ENTRIES {
      self.clear_stat_cache();
    }
    self.stat_cache.insert(rel.to_vec(), outcome.clone());
    outcome
  }

  fn stat(&mut self, rel: &[u8]) -> P9Result<StatInfo> {
    self.stat_cached(rel).map(|(info, _)| info)
  }

  fn walk(&mut self, base: &[u8], name: &[u8]) -> P9Result<Vec<u8>> {
    if name == b"." {
      return Ok(base.to_vec());
    }
    if name == b".." {
      if base.is_empty() {
        return Ok(Vec::new());
      }
      let parent = match base.iter().rposition(|&b| b == b'/') {
        Some(slash) => base[..slash].to_vec(),
        None => Vec::new(),
      };
      self.stat(&parent)?;
      return Ok(parent);
    }
    if name_bad(name) {
      return Err(ENOENT);
    }
    let mut next = base.to_vec();
    if !next.is_empty() {
      next.push(b'/');
    }
    next.extend_from_slice(name);
    if next.len() >= PATH_MAX {
      return Err(ENAMETOOLONG);
    }
    self.stat(&next)?;
    Ok(next)
  }

  fn fid_path(&self, fid: u32) -> P9Result<Vec<u8>> {
    self.fids.get(&fid).cloned().ok_or(ENOENT)
  }

  fn set_fid(&mut self, fid: u32, path: Vec<u8>, full: u32) -> P9Result<()> {
    if !self.fids.contains_key(&fid) && self.fids.len() >= MAX_FIDS {
      return Err(full);
    }
    self.fids.insert(fid, path);
    Ok(())
  }

  fn rewrite_fids_after_rename(&mut self, old_rel: &[u8], new_rel: &[u8]) {
    self.fids.retain(|_, path| {
      let suffix = if path.as_slice() == old_rel {
        Some(&path[path.len()..])
      } else if !old_rel.is_empty() && path.starts_with(old_rel) && path.get(old_rel.len()) == Some(&b'/') {
        Some(&path[old_rel.len()..])
      } else {
        None
      };
      let Some(suffix) = suffix else { return true };
      let mut renamed = new_rel.to_vec();
      renamed.extend_from_slice(suffix);
      if renamed.len() >= PATH_MAX {
        return false;
      }
      *path = renamed;
      true
    });
  }

  fn cached_read(&mut self, rel: &[u8], host: &Path, info: &StatInfo, offset: u64, out: &mut [u8]) -> Option<usize> {
    if !self.readonly || !info.is_file() || info.size > FILE_CACHE_MAX_FILE {
      self.stats.file_cache_bypasses += 1;
      return None;
    }
    if !self.file_cache.contains_key(rel) {
      let size = info.size as usize;
      if size > FILE_CACHE_MAX_BYTES || self.file_cache_bytes > FILE_CACHE_MAX_BYTES - size {
        self.stats.file_cache_bypasses += 1;
        return None;
      }
      self.stats.file_cache_misses += 1;
      let file = OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW).open(host).ok()?;
      let mut data = vec![0u8; size];
      file.read_exact_at(&mut data, 0).ok()?;
      self.file_cache_bytes += size;
      self.file_cache.insert(rel.to_vec(), data);
    } else {
      self.stats.file_cache_hits += 1;
    }

    let data = self.file_cache.get(rel)?;
    let start = usize::try_from(offset).unwrap_or(usize::MAX).min(data.len());
    let chunk = data.get(start..)?;
    let len = chunk.len().min(out.len());
    out.get_mut(..len)?.copy_from_slice(chunk.get(..len)?);
    Some(len)
  }

  fn op_version(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(13)?;
    let msize = req.u32()? as usize;
    let version = req.name_bytes()?;
    if 13 + version.len() > out.buf.len() {
      return Err(EINVAL);
    }
    self.msize = msize.max(MIN_MSIZE).min(self.capacity);
    out.u32(self.msize as u32)?;
    out.u16(version.len() as u16)?;
    out.put(version)
  }

  fn op_attach(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(15)?;
    let fid = req.u32()?;
    self.set_fid(fid, Vec::new(), EINVAL)?;
    out.qid(QTDIR, b"")
  }

  fn op_walk(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(17)?;
    let fid = req.u32()?;
    let newfid = req.u32()?;
    let nwname = req.u16()?;
    let mut path = self.fid_path(fid)?;
    if !self.fids.contains_key(&newfid) && self.fids.len() >= MAX_FIDS {
      return Err(ENOENT);
    }
    out.u16(nwname)?;
    for _ in 0..nwname {
      let name = req.name()?;
      path = self.walk(&path, name)?;
      let mode = self.stat(&path).map_or(0, |info| info.mode);
      out.qid(qid_type(mode), &path)?;
    }
    self.set_fid(newfid, path, ENOENT)
  }

  fn op_getattr(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(19)?;
    let path = self.fid_path(req.u32()?)?;
    let info = self.stat(&path)?;
    out.u64(GETATTR_BASIC)?;
    out.qid(qid_type(info.mode), &path)?;
    out.u32(info.mode)?;
    out.u32(0)?;
    out.u32(0)?;
    out.u64(info.nlink.max(1))?;
    out.u64(0)?;
    out.u64(info.size)?;
    out.u64(4096)?;
    out.u64(info.blocks)?;
    out.u64(info.atime as u64)?;
    out.u64(0)?;
    out.u64(info.mtime as u64)?;
    out.u64(0)?;
    out.u64(info.ctime as u64)?;
    out.put(&[0u8; 23])
  }

  fn io_unit(&self) -> u32 {
    (self.msize - IO_UNIT_RESERVE) as u32
  }

  fn op_lopen(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(15)?;
    let path = self.fid_path(req.u32()?)?;
    let info = self.stat(&path).map_err(|_| ENOENT)?;
    out.qid(qid_type(info.mode), &path)?;
    out.u32(self.io_unit())
  }

  fn op_read(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(23)?;
    let fid = req.u32()?;
    let offset = req.u64()?;
    let count = req.u32()? as usize;
    let path = self.fid_path(fid)?;
    let (info, host) = self.stat_cached(&path)?;
    if info.is_symlink() {
      return Err(ELOOP);
    }
    if !info.is_file() {
      return Err(EISDIR);
    }

    out.u32(0)?;
    let room = out.room().min(count);
    let payload = out.spare(room)?;
    let got = match self.cached_read(&path, &host, &info, offset, payload) {
      Some(got) => got,
      None => {
        let file =
          OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW).open(&host).map_err(|err| io_errno(&err))?;
        file.read_at(payload, offset).map_err(|err| io_errno(&err))?
      }
    };
    let got = got.min(room);
    out.len += got;
    out.patch_u32(HEADER_LEN, got as u32);
    self.stats.read_count += 1;
    self.stats.read_bytes += got as u64;
    self.stats.touch(&path, Touch::Read, got as u64);
    Ok(())
  }

  fn writable(&mut self) -> P9Result<()> {
    if self.readonly {
      return Err(EROFS);
    }
    self.clear_stat_cache();
    Ok(())
  }

  fn op_write(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(23)?;
    self.writable()?;
    let fid = req.u32()?;
    let offset = req.u64()?;
    let count = req.u32()? as usize;
    let data = req.rest().get(..count).ok_or(EINVAL)?;
    let path = self.fid_path(fid)?;
    let host = self.existing_path(&path)?;
    let file = OpenOptions::new().write(true).open(&host).map_err(|err| io_errno(&err))?;
    let written = file.write_at(data, offset).map_err(|err| io_errno(&err))?;
    self.stats.write_count += 1;
    self.stats.write_bytes += written as u64;
    self.clear_stat_cache();
    out.u32(written as u32)
  }

  fn op_lcreate(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(21)?;
    self.writable()?;
    let fid = req.u32()?;
    let parent = self.fid_path(fid)?;
    let name = req.name()?;
    let _flags = req.u32()?;
    let mode = req.u32()?;
    let _gid = req.u32()?;
    let host = self.child_path(&parent, name)?;
    OpenOptions::new().read(true).write(true).create_new(true).mode(mode & 0o777).open(&host).map_err(|err| io_errno(&err))?;
    self.clear_stat_cache();
    let rel = join_rel(&parent, name)?;
    self.fids.insert(fid, rel.clone());
    let info = self.stat(&rel)?;
    out.qid(qid_type(info.mode), &rel)?;
    out.u32(self.io_unit())
  }

  fn created(&mut self, parent: &[u8], name: &[u8], out: &mut Out) -> P9Result<()> {
    self.clear_stat_cache();
    let rel = join_rel(parent, name)?;
    let info = self.stat(&rel)?;
    out.qid(qid_type(info.mode), &rel)
  }

  fn op_mkdir(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(17)?;
    self.writable()?;
    let parent = self.fid_path(req.u32()?)?;
    let name = req.name()?;
    let mode = req.u32()?;
    let _gid = req.u32()?;
    let host = self.child_path(&parent, name)?;
    fs::DirBuilder::new().mode(mode & 0o777).create(&host).map_err(|err| io_errno(&err))?;
    self.created(&parent, name, out)
  }

  fn op_mknod(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(25)?;
    self.writable()?;
    let parent = self.fid_path(req.u32()?)?;
    let name = req.name()?;
    let mode = req.u32()?;
    req.take(12)?;
    let host = self.child_path(&parent, name)?;
    let perm = mode & 0o777;
    match mode & S_IFMT {
      S_IFREG => {
        OpenOptions::new().write(true).create_new(true).mode(perm).open(&host).map_err(|err| io_errno(&err))?;
      }
      S_IFIFO => {
        let path = CString::new(host.as_os_str().as_bytes()).map_err(|_| EINVAL)?;
        if unsafe { libc::mkfifo(path.as_ptr(), perm as libc::mode_t) } != 0 {
          return Err(io_errno(&io::Error::last_os_error()));
        }
      }
      S_IFDIR => fs::DirBuilder::new().mode(perm).create(&host).map_err(|err| io_errno(&err))?,
      _ => return Err(ENOTSUP),
    }
    self.created(&parent, name, out)
  }

  fn op_symlink(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(17)?;
    self.writable()?;
    let parent = self.fid_path(req.u32()?)?;
    let name = req.name()?;
    let target = req.name()?;
    let _gid = req.u32()?;
    if symlink_target_bad(target) {
      return Err(EPERM);
    }
    let host = self.child_path(&parent, name)?;
    std::os::unix::fs::symlink(OsStr::from_bytes(target), &host).map_err(|err| io_errno(&err))?;
    self.created(&parent, name, out)
  }

  fn op_readlink(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(11)?;
    let path = self.fid_path(req.u32()?)?;
    let (_, host) = self.stat_cached(&path)?;
    let target = fs::read_link(&host).map_err(|err| io_errno(&err))?;
    let bytes = target.as_os_str().as_bytes();
    let bytes = bytes.get(..PATH_MAX).unwrap_or(bytes);
    out.u16(bytes.len() as u16)?;
    out.put(bytes)
  }

  fn op_setattr(&mut self, req: &mut In) -> P9Result<()> {
    req.need(63)?;
    self.writable()?;
    let fid = req.u32()?;
    let valid = req.u32()?;
    req.take(12)?;
    let size = req.u64()?;
    let path = self.fid_path(fid)?;
    if valid & !SETATTR_SIZE != 0 {
      return Err(ENOTSUP);
    }
    let host = self.existing_path(&path)?;
    if valid & SETATTR_SIZE != 0 {
      let file = OpenOptions::new().write(true).open(&host).map_err(|err| io_errno(&err))?;
      file.set_len(size).map_err(|err| io_errno(&err))?;
    }
    self.clear_stat_cache();
    Ok(())
  }

  fn op_fsync(&mut self, req: &mut In) -> P9Result<()> {
    req.need(15)?;
    let path = self.fid_path(req.u32()?)?;
    let (_, host) = self.stat_cached(&path)?;
    if let Ok(file) = OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW).open(&host) {
      let _ = file.sync_all();
    }
    Ok(())
  }

  fn op_renameat(&mut self, req: &mut In) -> P9Result<()> {
    req.need(17)?;
    self.writable()?;
    let old_dir = self.fid_path(req.u32()?)?;
    let old_name = req.name()?;
    let new_dir = self.fid_path(req.u32()?)?;
    let new_name = req.name()?;
    let old_host = self.child_path(&old_dir, old_name)?;
    let new_host = self.child_path(&new_dir, new_name)?;
    let old_rel = join_rel(&old_dir, old_name)?;
    let new_rel = join_rel(&new_dir, new_name)?;
    fs::rename(&old_host, &new_host).map_err(|err| io_errno(&err))?;
    self.rewrite_fids_after_rename(&old_rel, &new_rel);
    self.clear_stat_cache();
    Ok(())
  }

  fn op_unlinkat(&mut self, req: &mut In) -> P9Result<()> {
    req.need(17)?;
    self.writable()?;
    let parent = self.fid_path(req.u32()?)?;
    let name = req.name()?;
    let flags = req.u32()?;
    let host = self.child_path(&parent, name)?;
    let removed = if flags & AT_REMOVEDIR != 0 { fs::remove_dir(&host) } else { fs::remove_file(&host) };
    removed.map_err(|err| io_errno(&err))?;
    self.clear_stat_cache();
    Ok(())
  }

  fn op_readdir(&mut self, req: &mut In, out: &mut Out) -> P9Result<()> {
    req.need(23)?;
    let fid = req.u32()?;
    let offset = req.u64()?;
    let count = req.u32()? as usize;
    let path = self.fid_path(fid)?;
    let (info, host) = self.stat_cached(&path)?;
    if !info.is_dir() {
      return Err(ENOTDIR);
    }
    let entries = fs::read_dir(&host).map_err(|err| io_errno(&err))?;

    out.u32(0)?;
    let limit = out.len + out.room().min(count);
    let start = out.len;
    let mut index = 0u64;
    for entry in entries.flatten() {
      index += 1;
      if index <= offset {
        continue;
      }
      let name = entry.file_name();
      let name = name.as_bytes();
      let Ok(child) = join_rel(&path, name) else { continue };
      let Ok(kind) = entry.file_type() else { continue };
      let dtype = if kind.is_dir() {
        DT_DIR
      } else if kind.is_symlink() {
        DT_LNK
      } else if kind.is_char_device() {
        DT_CHR
      } else if kind.is_block_device() {
        DT_BLK
      } else if kind.is_fifo() {
        DT_FIFO
      } else if kind.is_socket() {
        DT_SOCK
      } else {
        DT_REG
      };
      if name.len() > usize::from(u16::MAX) || out.len + DIRENT_FIXED_LEN + name.len() > limit {
        break;
      }
      out.qid(if kind.is_dir() { QTDIR } else { 0 }, &child)?;
      out.u64(index)?;
      out.u8(dtype)?;
      out.u16(name.len() as u16)?;
      out.put(name)?;
    }

    let used = out.len - start;
    out.patch_u32(HEADER_LEN, used as u32);
    self.stats.readdir_count += 1;
    self.stats.readdir_bytes += used as u64;
    self.stats.touch(&path, Touch::Readdir, used as u64);
    Ok(())
  }

  fn dispatch(&mut self, kind: u8, req: &mut In, out: &mut Out) -> P9Result<()> {
    match kind {
      TVERSION => self.op_version(req, out),
      TATTACH => self.op_attach(req, out),
      TWALK => self.op_walk(req, out),
      TGETATTR => self.op_getattr(req, out),
      TLOPEN => self.op_lopen(req, out),
      TREAD => self.op_read(req, out),
      TWRITE => self.op_write(req, out),
      TLCREATE => self.op_lcreate(req, out),
      TMKDIR => self.op_mkdir(req, out),
      TMKNOD => self.op_mknod(req, out),
      TSYMLINK => self.op_symlink(req, out),
      TREADLINK => self.op_readlink(req, out),
      TSETATTR => self.op_setattr(req),
      TFSYNC => self.op_fsync(req),
      TRENAMEAT => self.op_renameat(req),
      TUNLINKAT => self.op_unlinkat(req),
      TREADDIR => self.op_readdir(req, out),
      TSTATFS => {
        out.u32(0x0102_1997)?;
        out.u32(4096)?;
        out.put(&[0u8; 42])?;
        out.u32(255)
      }
      TCLUNK => {
        if let Ok(fid) = req.u32() {
          self.fids.remove(&fid);
        }
        Ok(())
      }
      _ => Err(ENOSYS),
    }
  }

  pub(crate) fn handle(&mut self, req: &[u8], resp: &mut [u8]) -> usize {
    if req.len() < HEADER_LEN || resp.len() < ERROR_LEN {
      return 0;
    }
    let kind = req[4];
    let tag = u16::from_le_bytes([req[5], req[6]]);
    self.stats.requests += 1;
    self.stats.op_counts[usize::from(kind)] += 1;

    let outcome = {
      let mut out = Out::new(resp);
      self.dispatch(kind, &mut In::new(req), &mut out).map(|()| out.len)
    };
    match outcome {
      Ok(len) => Out { buf: resp, len }.finish(kind.wrapping_add(1), tag),
      Err(ecode) => {
        self.stats.errors += 1;
        write_error(resp, tag, ecode)
      }
    }
  }

  pub(crate) fn reject_oversized(&mut self, req: &[u8], resp: &mut [u8]) -> usize {
    self.stats.requests += 1;
    self.stats.errors += 1;
    let tag = req.get(5..7).map_or(0, |tag| u16::from_le_bytes([tag[0], tag[1]]));
    write_error(resp, tag, EMSGSIZE)
  }

  pub(crate) fn serve(&mut self, req: &[u8], resp: &mut [u8]) -> usize {
    let started = Instant::now();
    let mut len = self.handle(req, resp);
    let elapsed = started.elapsed().as_nanos().min(u128::from(u64::MAX)) as u64;
    self.stats.total_ns += elapsed;
    if elapsed > self.stats.max_ns {
      self.stats.max_ns = elapsed;
      self.stats.max_type = req.get(4).copied().unwrap_or(0);
    }
    if len == 0 {
      self.stats.errors += 1;
      len = write_error(resp, 0, EIO);
    }
    len
  }

  pub(crate) fn report(&self, index: usize, mut log: impl FnMut(&str)) {
    let stats = &self.stats;
    if stats.requests == 0 {
      return;
    }
    log(&format!(
      "9p[{index}] summary tag={} root={} requests={} errors={} read={}/{}B readdir={}/{}B write={}/{}B stat_cache={} hit/{} miss clears={} file_cache={} hit/{} miss/{} bypass entries={}/{} bytes={} avg={}us max={}us({})",
      self.tag.to_string_lossy(),
      self.root.display(),
      stats.requests,
      stats.errors,
      stats.read_count,
      stats.read_bytes,
      stats.readdir_count,
      stats.readdir_bytes,
      stats.write_count,
      stats.write_bytes,
      stats.stat_hits,
      stats.stat_misses,
      stats.stat_cache_clears,
      stats.file_cache_hits,
      stats.file_cache_misses,
      stats.file_cache_bypasses,
      self.file_cache.len(),
      self.file_cache.capacity(),
      self.file_cache_bytes,
      stats.total_ns / stats.requests / 1000,
      stats.max_ns / 1000,
      type_name(stats.max_type),
    ));
    let op = |kind: u8| stats.op_counts[usize::from(kind)];
    log(&format!(
      "9p[{index}] ops walk={} getattr={} lopen={} read={} readdir={} readlink={} clunk={}",
      op(TWALK),
      op(TGETATTR),
      op(TLOPEN),
      op(TREAD),
      op(TREADDIR),
      op(TREADLINK),
      op(TCLUNK),
    ));

    let mut hot: Vec<&PathStat> = stats.paths.iter().filter(|entry| entry.used && entry.count > 0).collect();
    hot.sort_by(|a, b| b.count.cmp(&a.count));
    for (rank, entry) in hot.iter().take(REPORT_TOP_COUNT).enumerate() {
      log(&format!(
        "9p[{index}] hot[{rank}] count={} stat={}/{} read={} readdir={} bytes={} path={}",
        entry.count, entry.stat_hits, entry.stat_misses, entry.reads, entry.readdirs, entry.bytes, entry.label,
      ));
    }
  }
}

impl<'a> In<'a> {
  fn name_bytes(&mut self) -> P9Result<&'a [u8]> {
    let len = usize::from(self.u16()?);
    self.take(len)
  }
}

pub(crate) struct Chain {
  pub request: Vec<u8>,
  pub writes: Vec<(u64, u32)>,
  pub oversized: bool,
}

const E2BIG: Errno = Errno(libc::E2BIG);

pub(crate) fn read_chain<M: GuestMemory>(mem: &M, desc_base: u64, head: u16, queue_size: u16, req_cap: usize) -> DeviceResult<Chain> {
  let mut chain = Chain { request: Vec::new(), writes: Vec::new(), oversized: false };
  let mut index = head;
  for _ in 0..queue_size {
    let desc = read_desc(mem, desc_base, index)?;
    if desc.writable() {
      if chain.writes.len() >= MAX_WRITE_IOV {
        return Err(E2BIG);
      }
      chain.writes.push((desc.addr, desc.len));
    } else {
      let start = chain.request.len();
      let wanted = desc.len as usize;
      let take = wanted.min(req_cap - start.min(req_cap));
      chain.oversized |= take < wanted;
      chain.request.resize(start + take, 0);
      mem.read(desc.addr, &mut chain.request[start..])?;
    }
    if !desc.has_next() {
      break;
    }
    index = desc.next;
  }
  Ok(chain)
}

pub(crate) fn write_response<M: GuestMemory>(mem: &M, writes: &[(u64, u32)], resp: &[u8]) -> DeviceResult<()> {
  let mut rest = resp;
  for &(addr, len) in writes {
    if rest.is_empty() {
      break;
    }
    let (chunk, after) = rest.split_at(rest.len().min(len as usize));
    mem.write(addr, chunk)?;
    rest = after;
  }
  if rest.is_empty() {
    Ok(())
  } else {
    Err(DEVICE_ENOSPC)
  }
}

#[cfg(not(test))]
pub(crate) mod ffi {
  use super::*;
  use crate::sandbox::errno::Result;
  use crate::sandbox::guest::ffi::{Vm, VmMemory};
  use crate::sandbox::host;
  use crate::sandbox::virtio::ffi::{ant_hvf_virtio_interrupt, with_device, Handle};
  use crate::sandbox::bus::bus_of;
  use crate::sandbox::virtio::{Device, Kind};
  use crate::sandbox::vring::{add_used, next_head};
  use core::ffi::c_int;


  fn serve_queue(mem: &VmMemory, device: &mut Device) -> Result<bool> {
    let Some((queue, p9)) = device.p9_queue() else { return Ok(false) };
    if !queue.ready() {
      return Ok(false);
    }
    let mut resp = vec![0u8; MAX_MSIZE];
    while let Some(head) = next_head(mem, queue)? {
      let msize = p9.msize();
      let chain = read_chain(mem, queue.desc, head, queue.size, msize)?;
      let Some(resp) = resp.get_mut(..msize) else { break };
      let len = if chain.oversized { p9.reject_oversized(&chain.request, resp) } else { p9.serve(&chain.request, resp) };
      let Some(reply) = resp.get(..len) else { break };
      write_response(mem, &chain.writes, reply)?;
      add_used(mem, queue.used, u32::from(queue.size), head, len as u32)?;
      queue.last_avail = queue.last_avail.wrapping_add(1);
    }
    Ok(true)
  }

  pub(crate) unsafe fn notify(vm: *mut Vm, dev: *mut Handle) -> c_int {
    let mem = VmMemory(vm);
    match with_device(dev, |device| serve_queue(&mem, device)) {
      Some(Ok(true)) => ant_hvf_virtio_interrupt(vm, dev, 0),
      Some(Ok(false)) | None => 0,
      Some(Err(Errno(code))) => -code,
    }
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_9p_request(dev: *mut Handle, req: *const u8, req_len: usize, resp: *mut u8, resp_cap: usize) -> u32 {
    if req.is_null() || resp.is_null() {
      return 0;
    }
    let req = core::slice::from_raw_parts(req, req_len);
    let resp = core::slice::from_raw_parts_mut(resp, resp_cap);
    with_device(dev, |device| device.p9_mut().map_or(0, |p9| p9.handle(req, resp))).unwrap_or(0) as u32
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_9p_report(vm: *mut Vm) {
    let Some(bus) = bus_of(vm) else { return };
    for (index, dev) in bus.devices(Kind::P9).enumerate() {
      with_device(dev, |device| {
        if let Some(p9) = device.p9_mut() {
          p9.report(index, |line| host::log(vm, line));
        }
      });
    }
  }
}

#[cfg(test)]
#[path = "tests/p9.rs"]
mod tests;
