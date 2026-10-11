#![no_std]
#![crate_name = "safe"]

use core::panic::PanicInfo;

#[panic_handler]
fn ph(_: &PanicInfo) -> ! { loop {} }

#[no_mangle]
pub extern "C" fn probe_answer() -> i32 {
    42
}
