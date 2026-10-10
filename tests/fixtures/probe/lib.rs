//! probe - the thorn-rs acceptance fixture.
//!
//! A minimal `#![no_std]` crate exporting a stable C-ABI surface:
//! `thorn_rs_probe()` and `thorn_rs_add(i32, i32)`. The panic handler
//! is in-tree because Phase 0 has no Cargo to supply one.

#![no_std]
#![crate_name = "probe"]

use core::panic::PanicInfo;

#[panic_handler]
fn probe_panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn thorn_rs_probe() -> i32 {
    42
}

#[no_mangle]
pub extern "C" fn thorn_rs_add(a: i32, b: i32) -> i32 {
    a.wrapping_add(b)
}
