/// An MMIO register which can only be read from.
#[derive(Default)]
#[repr(transparent)]
pub struct ReadOnly<T: Copy>(pub(crate) T);

impl<T: Copy> ReadOnly<T> {
    /// Construct a new instance for testing.
    pub const fn new(value: T) -> Self {
        Self(value)
    }
}

/// An MMIO register which can only be written to.
#[derive(Default)]
#[repr(transparent)]
pub struct WriteOnly<T: Copy>(pub(crate) T);

/// An MMIO register which may be both read and written.
#[derive(Default)]
#[repr(transparent)]
pub struct Volatile<T: Copy>(T);

impl<T: Copy> Volatile<T> {
    /// Construct a new instance for testing.
    pub const fn new(value: T) -> Self {
        Self(value)
    }
}

/// A trait implemented by MMIO registers which may be read from.
pub trait VolatileReadable<T> {
    /// Performs a volatile read from the MMIO register.
    unsafe fn vread(self) -> T;
}

#[cfg(not(target_arch = "aarch64"))]
impl<T: Copy> VolatileReadable<T> for *const ReadOnly<T> {
    unsafe fn vread(self) -> T {
        self.read_volatile().0
    }
}

#[cfg(not(target_arch = "aarch64"))]
impl<T: Copy> VolatileReadable<T> for *const Volatile<T> {
    unsafe fn vread(self) -> T {
        self.read_volatile().0
    }
}

/// A trait implemented by MMIO registers which may be written to.
pub trait VolatileWritable<T> {
    /// Performs a volatile write to the MMIO register.
    unsafe fn vwrite(self, value: T);
}

#[cfg(not(target_arch = "aarch64"))]
impl<T: Copy> VolatileWritable<T> for *mut WriteOnly<T> {
    unsafe fn vwrite(self, value: T) {
        (self as *mut T).write_volatile(value)
    }
}

#[cfg(not(target_arch = "aarch64"))]
impl<T: Copy> VolatileWritable<T> for *mut Volatile<T> {
    unsafe fn vwrite(self, value: T) {
        (self as *mut T).write_volatile(value)
    }
}

#[cfg(target_arch = "aarch64")]
mod aarch64_mmio {
    use super::{ReadOnly, Volatile, VolatileReadable, VolatileWritable, WriteOnly};
    use crate::transport::DeviceStatus;
    use core::arch::asm;

    macro_rules! asm_mmio_write {
        ($t:ty, $assembly:literal) => {
            impl VolatileWritable<$t> for *mut WriteOnly<$t> {
                unsafe fn vwrite(self, value: $t) {
                    asm!(
                        $assembly,
                        value = in(reg) value,
                        ptr = in(reg) (self as *mut $t),
                    );
                }
            }

            impl VolatileWritable<$t> for *mut Volatile<$t> {
                unsafe fn vwrite(self, value: $t) {
                    asm!(
                        $assembly,
                        value = in(reg) value,
                        ptr = in(reg) (self as *mut $t),
                    );
                }
            }
        };
    }

    macro_rules! asm_mmio_read {
        ($t:ty, $assembly:literal) => {
            impl VolatileReadable<$t> for *const ReadOnly<$t> {
                unsafe fn vread(self) -> $t {
                    let value;
                    asm!(
                        $assembly,
                        value = out(reg) value,
                        ptr = in(reg) (self as *const $t),
                    );
                    value
                }
            }

            impl VolatileReadable<$t> for *const Volatile<$t> {
                unsafe fn vread(self) -> $t {
                    let value;
                    asm!(
                        $assembly,
                        value = out(reg) value,
                        ptr = in(reg) (self as *const $t),
                    );
                    value
                }
            }
        };
    }

    asm_mmio_write!(u8, "strb {value:w}, [{ptr}]");
    asm_mmio_write!(u16, "strh {value:w}, [{ptr}]");
    asm_mmio_write!(u32, "str {value:w}, [{ptr}]");
    asm_mmio_write!(u64, "str {value:x}, [{ptr}]");

    impl VolatileWritable<DeviceStatus> for *mut WriteOnly<DeviceStatus> {
        unsafe fn vwrite(self, value: DeviceStatus) {
            let value: u32 = value.bits();
            asm!(
                "str {value:w}, [{ptr}]",
                value = in(reg) value,
                ptr = in(reg) (self as *mut u32),
            );
        }
    }

    impl VolatileWritable<DeviceStatus> for *mut Volatile<DeviceStatus> {
        unsafe fn vwrite(self, value: DeviceStatus) {
            let value: u32 = value.bits();
            asm!(
                "str {value:w}, [{ptr}]",
                value = in(reg) value,
                ptr = in(reg) (self as *mut u32),
            );
        }
    }

    asm_mmio_read!(u8, "ldrb {value:w}, [{ptr}]");
    asm_mmio_read!(u16, "ldrh {value:w}, [{ptr}]");
    asm_mmio_read!(u32, "ldr {value:w}, [{ptr}]");
    asm_mmio_read!(u64, "ldr {value:x}, [{ptr}]");

    impl VolatileReadable<DeviceStatus> for *const ReadOnly<DeviceStatus> {
        unsafe fn vread(self) -> DeviceStatus {
            let value: u32;
            asm!(
                "ldr {value:w}, [{ptr}]",
                value = out(reg) value,
                ptr = in(reg) (self as *const u32),
            );
            DeviceStatus::from_bits_retain(value)
        }
    }

    impl VolatileReadable<DeviceStatus> for *const Volatile<DeviceStatus> {
        unsafe fn vread(self) -> DeviceStatus {
            let value: u32;
            asm!(
                "ldr {value:w}, [{ptr}]",
                value = out(reg) value,
                ptr = in(reg) (self as *const u32),
            );
            DeviceStatus::from_bits_retain(value)
        }
    }
}

/// Performs a volatile read from the given field of pointer to a struct representing an MMIO region.
///
/// # Usage
/// ```compile_fail
/// # use core::ptr::NonNull;
/// # use virtio_drivers::volatile::{ReadOnly, volread};
/// struct MmioDevice {
///   field: ReadOnly<u32>,
/// }
///
/// let device: NonNull<MmioDevice> = NonNull::new(0x1234 as *mut MmioDevice).unwrap();
/// let value = unsafe { volread!(device, field) };
/// ```
macro_rules! volread {
    ($nonnull:expr, $field:ident) => {
        $crate::volatile::VolatileReadable::vread((&raw const (*$nonnull.as_ptr()).$field))
    };
}

/// Performs a volatile write to the given field of pointer to a struct representing an MMIO region.
///
/// # Usage
/// ```compile_fail
/// # use core::ptr::NonNull;
/// # use virtio_drivers::volatile::{WriteOnly, volread};
/// struct MmioDevice {
///   field: WriteOnly<u32>,
/// }
///
/// let device: NonNull<MmioDevice> = NonNull::new(0x1234 as *mut MmioDevice).unwrap();
/// unsafe { volwrite!(device, field, 42); }
/// ```
macro_rules! volwrite {
    ($nonnull:expr, $field:ident, $value:expr) => {
        $crate::volatile::VolatileWritable::vwrite((&raw mut (*$nonnull.as_ptr()).$field), $value)
    };
}

pub(crate) use volread;
pub(crate) use volwrite;
