//! probe2 - a second no_std fixture for multi-crate build tests.

#![no_std]
#![crate_name = "probe2"]

use core::panic::PanicInfo;

#[panic_handler]
fn probe2_panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn thorn_rs2_add(a: i32, b: i32) -> i32 {
    a.wrapping_add(b).wrapping_add(1)
}
