use super::errno::{Result, EINVAL, ELOOP, ENOSPC};
use super::guest::GuestMemory;

pub(crate) const DESC_F_NEXT: u16 = 1;
pub(crate) const DESC_F_WRITE: u16 = 2;
const DESC_BYTES: u64 = 16;
const USED_ELEM_BYTES: u64 = 8;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Desc {
  pub addr: u64,
  pub len: u32,
  pub flags: u16,
  pub next: u16,
}

impl Desc {
  pub(crate) fn writable(&self) -> bool {
    self.flags & DESC_F_WRITE != 0
  }

  pub(crate) fn has_next(&self) -> bool {
    self.flags & DESC_F_NEXT != 0
  }
}

#[derive(Default)]
pub struct Queue {
  pub desc: u64,
  pub avail: u64,
  pub used: u64,
  pub size: u16,
  pub max_size: u16,
  pub last_avail: u16,
  pub msix_vector: u16,
  pub notify_off: u16,
  pub enabled: bool,
}

impl Queue {
  pub(crate) fn ready(&self) -> bool {
    self.enabled && self.desc != 0 && self.avail != 0 && self.used != 0
  }
}

pub(crate) fn read_desc<M: GuestMemory>(mem: &M, desc_base: u64, index: u16) -> Result<Desc> {
  let raw: [u8; 16] = mem.read_array(desc_base.wrapping_add(u64::from(index) * DESC_BYTES))?;
  Ok(Desc {
    addr: u64::from_le_bytes([raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]]),
    len: u32::from_le_bytes([raw[8], raw[9], raw[10], raw[11]]),
    flags: u16::from_le_bytes([raw[12], raw[13]]),
    next: u16::from_le_bytes([raw[14], raw[15]]),
  })
}

pub(crate) fn add_used<M: GuestMemory>(mem: &M, used_base: u64, queue_size: u32, head: u16, used_len: u32) -> Result<()> {
  if queue_size == 0 {
    return Err(EINVAL);
  }
  let idx_addr = used_base.wrapping_add(2);
  let used_idx = mem.read_u16(idx_addr)?;
  let slot = u64::from(u32::from(used_idx) % queue_size);
  let mut elem = [0u8; USED_ELEM_BYTES as usize];
  elem[..4].copy_from_slice(&u32::from(head).to_le_bytes());
  elem[4..].copy_from_slice(&used_len.to_le_bytes());
  mem.write(used_base.wrapping_add(4).wrapping_add(slot * USED_ELEM_BYTES), &elem)?;
  mem.write(idx_addr, &used_idx.wrapping_add(1).to_le_bytes())
}

pub(crate) fn read_chain<M: GuestMemory>(mem: &M, desc_base: u64, head: u16, queue_size: u32, out: &mut [u8]) -> Result<u32> {
  let mut index = head;
  let mut total = 0usize;
  for _ in 0..queue_size {
    let desc = read_desc(mem, desc_base, index)?;
    if desc.writable() {
      return Err(EINVAL);
    }
    let end = total.checked_add(desc.len as usize).ok_or(ENOSPC)?;
    let chunk = out.get_mut(total..end).ok_or(ENOSPC)?;
    mem.read(desc.addr, chunk)?;
    total = end;
    if !desc.has_next() {
      return u32::try_from(total).map_err(|_| ENOSPC);
    }
    index = desc.next;
  }
  Err(ELOOP)
}

pub(crate) fn write_chain<M: GuestMemory>(mem: &M, desc_base: u64, head: u16, queue_size: u32, data: &[u8]) -> Result<u32> {
  let mut index = head;
  let mut rest = data;
  for _ in 0..queue_size {
    if rest.is_empty() {
      break;
    }
    let desc = read_desc(mem, desc_base, index)?;
    if !desc.writable() {
      return Err(EINVAL);
    }
    let (chunk, after) = rest.split_at(rest.len().min(desc.len as usize));
    mem.write(desc.addr, chunk)?;
    rest = after;
    if rest.is_empty() {
      return u32::try_from(data.len()).map_err(|_| ENOSPC);
    }
    if !desc.has_next() {
      break;
    }
    index = desc.next;
  }
  Err(ENOSPC)
}

pub(crate) fn next_head<M: GuestMemory>(mem: &M, queue: &Queue) -> Result<Option<u16>> {
  let avail_idx = mem.read_u16(queue.avail.wrapping_add(2))?;
  if queue.last_avail == avail_idx {
    return Ok(None);
  }
  let slot = queue.last_avail.checked_rem(queue.size).ok_or(EINVAL)?;
  Ok(Some(mem.read_u16(queue.avail.wrapping_add(4).wrapping_add(u64::from(slot) * 2))?))
}

pub(crate) fn drain<M: GuestMemory>(
  mem: &M,
  queue: &mut Queue,
  mut request: impl FnMut(u64, u16, u16) -> Result<u32>,
) -> Result<bool> {
  if !queue.ready() {
    return Ok(false);
  }
  while let Some(head) = next_head(mem, queue)? {
    if head >= queue.size {
      return Err(EINVAL);
    }
    let used_len = request(queue.desc, queue.size, head)?;
    add_used(mem, queue.used, u32::from(queue.size), head, used_len)?;
    queue.last_avail = queue.last_avail.wrapping_add(1);
  }
  Ok(true)
}

#[cfg(test)]
#[path = "tests/vring_support.rs"]
pub(crate) mod testing;

#[cfg(test)]
#[path = "tests/vring.rs"]
mod tests;
