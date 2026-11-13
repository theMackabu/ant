use super::*;

fn device(kind: Kind, queue_count: u16, queue_size: u16, device_config_len: u16) -> Device {
  Device::new(Params {
    kind,
    virtio_id: 2,
    subsystem_id: 2,
    class_code: 0x01,
    subclass: 0x80,
    bar0: 0x1004_0000,
    features: 0x244,
    queue_count,
    queue_size,
    device_config_len,
  })
}

fn read(dev: &mut Device, off: u64, size: u32) -> u64 {
  match dev.common_read(off, size) {
    Read::Value(value) => value,
    Read::DeviceConfig(_) => panic!("unexpected device config read"),
  }
}

fn enable_msix(dev: &mut Device, vector: u64, addr: u64, data: u32) {
  dev.pci_write(CAP_MSIX_POS as u32 + 2, 2, u64::from(PCI_MSIX_ENABLE));
  let base = MSIX_TABLE + vector * MSIX_ENTRY_SIZE;
  dev.common_write(base, 4, addr & 0xffff_ffff);
  dev.common_write(base + 4, 4, addr >> 32);
  dev.common_write(base + 8, 4, u64::from(data));
  dev.common_write(base + 12, 4, 0);
}

#[test]
fn kinds_round_trip() {
  assert_eq!(Params::block(0x1000).kind, Kind::Block);
  assert_eq!(Params::net(0x1000).queue_count, 2);
  assert_eq!(Params::rng(0x1000).device_config_len, 0);
  assert_eq!(Params::vsock(0x1000).queue_count, 3);
  assert_eq!(Params::share(0x1000, 5).device_config_len, 7);
  assert_eq!(Params::share(0x1000, usize::MAX).device_config_len, u16::MAX);
  assert!(device(Kind::Vsock, 3, 32, 8).vsock().is_some());
  let mut share = device(Kind::P9, 1, 32, 64);
  assert!(share.p9_mut().is_none() && share.p9_queue().is_none());
  share.set_p9(P9::new(b"/", b"share", true, crate::sandbox::p9::MAX_MSIZE).unwrap());
  assert_eq!(share.p9_mut().map(|p9| p9.tag().to_vec()), Some(b"share".to_vec()));
  assert_eq!(share.p9_queue().map(|(queue, p9)| (queue.size, p9.msize())), Some((32, crate::sandbox::p9::DEFAULT_MSIZE)));
  share.reset();
  let mut dev = device(Kind::Rng, 1, 32, 0);
  assert!(dev.vsock().is_none());
  assert_eq!(dev.kind(), Kind::Rng);
  dev.queue_mut(0).unwrap().last_avail = 9;
  assert_eq!(dev.queue(0).unwrap().last_avail, 9);
  assert!(dev.queue_mut(3).is_none());
}

#[test]
fn width_helpers() {
  assert_eq!(select_width(0x1122_3344_5566_7788, 0, 8), 0x1122_3344_5566_7788);
  assert_eq!(select_width(0x1122_3344, 1, 2), 0x2233);
  assert_eq!(select_width(0x1122_3344, 3, 1), 0x11);
  assert_eq!(select_width(0x1122_3344, 9, 1), 0);

  let mut word = 0xaabb_ccddu32;
  assign32(&mut word, 1, 1, 0x1ff);
  assert_eq!(word, 0xaabb_ffdd);
  assign32(&mut word, 3, 2, 0x1234);
  assert_eq!(word, 0x34bb_ffdd);
  assign32(&mut word, 0, 8, 0x1_0000_0001);
  assert_eq!(word, 1);

  let mut half = 0xaabbu16;
  assign16(&mut half, 1, 1, 0x1cc);
  assert_eq!(half, 0xccbb);
  assign16(&mut half, 1, 2, 0x1234);
  assert_eq!(half, 0x34bb);

  let mut wide = 0u64;
  assign64(&mut wide, 4, 4, 0xdead_beef);
  assert_eq!(wide, 0xdead_beef_0000_0000);
  assign64(&mut wide, 0, 8, 7);
  assert_eq!(wide, 7);
}

#[test]
fn common_config_reflects_device_and_selected_queue() {
  let mut dev = device(Kind::Block, 1, 64, 24);
  assert_eq!(read(&mut dev, 4, 4), 0x244);
  dev.common_write(0, 4, 1);
  assert_eq!(read(&mut dev, 4, 4), 1);
  assert_eq!(read(&mut dev, 18, 2), 1);
  assert_eq!(read(&mut dev, 24, 2), 64);
  assert_eq!(read(&mut dev, 26, 2), 0xffff);
  assert_eq!(read(&mut dev, 30, 2), 0);

  dev.common_write(22, 2, 1);
  assert_eq!(read(&mut dev, 24, 2), 0);
  assert_eq!(read(&mut dev, 26, 2), 0xffff);
  dev.common_write(24, 2, 16);
  dev.common_write(28, 2, 1);
  dev.common_write(22, 2, 0);
  assert_eq!(read(&mut dev, 24, 2), 64);
  assert_eq!(read(&mut dev, 28, 2), 0);
}

#[test]
fn driver_features_are_written_in_halves() {
  let mut dev = device(Kind::Block, 1, 64, 24);
  dev.common_write(12, 4, 0x1234_5678);
  dev.common_write(8, 4, 1);
  dev.common_write(12, 4, 0x9);
  assert_eq!(read(&mut dev, 12, 4), 0x9);
  dev.common_write(8, 4, 0);
  assert_eq!(read(&mut dev, 12, 4), 0x1234_5678);
  dev.common_write(8, 4, 2);
  dev.common_write(12, 4, 0xffff_ffff);
  dev.common_write(8, 4, 0);
  assert_eq!(read(&mut dev, 12, 4), 0x1234_5678);
}

#[test]
fn queue_setup_validation_and_reset() {
  let mut dev = device(Kind::Vsock, 3, 32, 8);
  for bad in [0u64, 24, 64] {
    dev.common_write(24, 2, bad);
    assert_eq!(read(&mut dev, 24, 2), 32);
  }
  dev.common_write(24, 2, 16);
  assert_eq!(read(&mut dev, 24, 2), 16);

  dev.common_write(32, 4, 0x4000_1000);
  dev.common_write(36, 4, 0x1);
  dev.common_write(40, 8, 0x4000_2000);
  dev.common_write(48, 8, 0x4000_3000);
  assert!(!dev.queue(0).unwrap().ready());
  dev.common_write(28, 2, 1);
  let queue = dev.queue(0).unwrap();
  assert!(queue.ready());
  assert_eq!((queue.desc, queue.avail, queue.used, queue.size), (0x1_4000_1000, 0x4000_2000, 0x4000_3000, 16));
  assert_eq!(read(&mut dev, 32, 8), 0x1_4000_1000);

  dev.common_write(20, 1, 0x0f);
  assert_eq!(read(&mut dev, 20, 1), 0x0f);
  assert_eq!(dev.common_write(20, 1, 0), Effect::Reset);
  let queue = dev.queue(0).unwrap();
  assert!(!queue.ready());
  assert_eq!((queue.desc, queue.size, queue.last_avail), (0, 32, 0));
  assert_eq!(read(&mut dev, 20, 1), 0);
  assert!(dev.queue(3).is_none());
}

#[test]
fn region_dispatch() {
  let mut dev = device(Kind::Vsock, 3, 32, 8);
  assert_eq!(dev.common_write(NOTIFY_CFG, 2, 0), Effect::Notify(0));
  assert_eq!(dev.common_write(NOTIFY_CFG + 4, 2, 0), Effect::Notify(2));
  assert_eq!(dev.common_write(NOTIFY_CFG + 6, 2, 0), Effect::None);
  assert_eq!(dev.common_write(DEVICE_CFG, 4, 9), Effect::None);
  assert_eq!(dev.common_read(DEVICE_CFG + 7, 1), Read::DeviceConfig(7));
  assert_eq!(dev.common_read(DEVICE_CFG + 8, 1), Read::Value(0));
  assert_eq!(dev.common_read(0x5000, 4), Read::Value(0));
  assert_eq!(dev.common_read(u64::MAX, 8), Read::Value(0));
  assert_eq!(dev.common_write(u64::MAX, 8, 1), Effect::None);
}

#[test]
fn interrupt_sets_isr_and_uses_queue_vector() {
  let mut dev = device(Kind::Block, 1, 64, 24);
  assert_eq!(dev.interrupt(0), None);
  assert_eq!(read(&mut dev, ISR_CFG, 1), 1);
  assert_eq!(read(&mut dev, ISR_CFG, 1), 0);

  enable_msix(&mut dev, 1, 0x1_0000_2000, 77);
  dev.common_write(26, 2, 1);
  assert_eq!(dev.interrupt(0), Some(Msi { addr: 0x1_0000_2000, data: 77 }));
  assert_eq!(dev.interrupt(5), None);
  assert_eq!(read(&mut dev, MSIX_TABLE + MSIX_ENTRY_SIZE + 8, 4), 77);
}

#[test]
fn masked_vectors_pend_until_unmasked() {
  let mut dev = device(Kind::Block, 1, 64, 24);
  enable_msix(&mut dev, 2, 0x8000, 5);
  dev.common_write(26, 2, 2);
  let control = MSIX_TABLE + 2 * MSIX_ENTRY_SIZE + 12;
  dev.common_write(control, 4, 1);
  assert_eq!(dev.interrupt(0), None);
  assert_eq!(read(&mut dev, MSIX_PBA, 4), 0b100);
  assert_eq!(dev.common_write(control, 4, 0), Effect::Msi(Msi { addr: 0x8000, data: 5 }));
  assert_eq!(read(&mut dev, MSIX_PBA, 4), 0);
  assert_eq!(dev.common_write(control, 4, 0), Effect::None);

  dev.pci_write(CAP_MSIX_POS as u32 + 2, 2, u64::from(PCI_MSIX_ENABLE | PCI_MSIX_MASK_ALL));
  assert_eq!(dev.interrupt(0), None);
  assert_eq!(read(&mut dev, MSIX_PBA, 4), 0b100);
}

#[test]
fn pci_config_space() {
  let mut dev = device(Kind::Block, 1, 64, 24);
  assert_eq!(dev.pci_read32(0), 0x1042_1af4);
  assert_eq!(dev.pci_read32(0x08), 0x0180_0001);
  assert_eq!(dev.pci_read32(0x10), 0x1004_0000);
  assert_eq!(dev.pci_read32(0x2c), 0x0002_1af4);
  assert_eq!(dev.pci_read32(0x34) & 0xff, CAP_COMMON_POS as u32);
  assert_eq!(dev.pci_read32(CAP_COMMON_POS as u32), 0x0110_5009);
  assert_eq!(dev.pci_read32(CAP_NOTIFY_POS as u32), 0x0214_7009);
  assert_eq!(dev.pci_read32(CAP_NOTIFY_POS as u32 + 12), 2);
  assert_eq!(dev.pci_read32(CAP_ISR_POS as u32), 0x0310_8009);
  assert_eq!(dev.pci_read32(CAP_DEVICE_POS as u32 + 12), 24);
  assert_eq!(dev.pci_read32(CAP_MSIX_POS as u32), 0x0003_0011);
  assert_eq!(dev.pci_read32(0x100), 0);
  assert_eq!(dev.pci_read32(u32::MAX), 0);

  let bare = device(Kind::Rng, 1, 32, 0);
  assert_eq!((bare.pci_read32(CAP_ISR_POS as u32) >> 8) & 0xff, CAP_MSIX_POS as u32);
  assert_eq!(bare.pci_read32(CAP_DEVICE_POS as u32), 0);

  dev.pci_write(PCI_CONFIG_COMMAND, 2, u64::MAX);
  assert_eq!(dev.pci_read32(PCI_CONFIG_COMMAND) & 0xffff, 7);
  dev.pci_write(PCI_CONFIG_COMMAND + 2, 2, u64::MAX);
  assert_eq!(dev.pci_read32(PCI_CONFIG_COMMAND), 0x0010_0007);

  dev.pci_write(PCI_CONFIG_BAR0, 4, u64::from(u32::MAX));
  assert_eq!(dev.bar0(), u32::MAX);
  assert_eq!(dev.pci_read32(PCI_CONFIG_BAR0), 0xffff_f000);
  dev.pci_write(PCI_CONFIG_BAR0, 4, 0x1004_1fff);
  assert_eq!(dev.bar0(), 0x1004_1000);
  dev.pci_write(PCI_CONFIG_BAR0, 4, 0);
  for byte in 0..4 {
    dev.pci_write(PCI_CONFIG_BAR0 + byte, 1, 0xff);
  }
  assert_eq!(dev.bar0(), u32::MAX);

  dev.pci_write(CAP_MSIX_POS as u32 + 2, 2, u64::MAX);
  assert_eq!(dev.pci_read32(CAP_MSIX_POS as u32) >> 16, 0xc003);
  dev.pci_write(CAP_MSIX_POS as u32 + 3, 1, 0);
  assert_eq!(dev.pci_read32(CAP_MSIX_POS as u32) >> 16, 0x0003);
}
