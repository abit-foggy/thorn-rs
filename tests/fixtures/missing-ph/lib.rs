//! missing-ph - negative fixture: `#![no_std]` without a panic handler.
//!
//! The commented-out handler below must NOT satisfy the crate
//! inspector: comment and string contents are blanked before
//! attribute matching, so this crate fails fast at plan time instead
//! of reaching `rustc` (which would fail with "#[panic_handler]
//! function required, but not found").

#![no_std]

// #[panic_handler]
// fn fake_handler(_info: &core::panic::PanicInfo) -> ! { loop {} }

#[no_mangle]
pub extern "C" fn thorn_rs_missing_ph() -> i32 {
    1
}
