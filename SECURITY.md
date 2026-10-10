# Security Policy

## Supported Versions

Security updates are applied to the active release stream.

| Version | Supported |
| ------- | --------- |
| 0.1.x   | Yes       |
| < 0.1.0 | No        |

## Reporting a Vulnerability

The thorn-rs project takes security vulnerabilities seriously. If you discover a vulnerability or security issue, please do not file a public issue on GitHub.

Please report security vulnerabilities through GitHub Private Vulnerability Reporting:
Navigate to the Security tab of the repository on GitHub, select "Report a vulnerability", and provide the details.

### What to Include in Your Report

To help us triage and resolve the issue quickly, please include:
- A clear description of the vulnerability and its potential impact.
- Affected component: the crate inspector (`src/scan.c`), the command planner (`src/plan.c`), the fragment emitters (`src/emit_ninja.c`, `src/emit_make.c`), or the CLI driver (`src/main.c`).
- Step-by-step reproduction instructions, including the crate root or flag sequence that triggers the issue.
- Target platform and architecture (e.g. Linux x86_64, macOS aarch64) and the `rustc` version in use.
- Any proposed mitigations or proof-of-concept files.

## Response and Disclosure Process

1. **Acknowledgment**: We aim to acknowledge receipt of security reports within 48 hours.
2. **Investigation & Triage**: We will confirm the vulnerability, determine its severity, and provide regular progress updates.
3. **Patch Development**: Fixes are developed in private branches and tested against the thorn-rs acceptance suite.
4. **Coordinated Disclosure**: Once a fix is verified and ready for release, we will coordinate public disclosure and publish a security advisory with credit to the reporter.

## Security Architecture & Invariants

thorn-rs is designed with several defensive security principles in mind:
- **Bounded ingestion**: crate roots are read with a fixed size cap and scanned through a comment/string-aware normalizer; hostile input fails with a diagnostic, never with memory unsafety.
- **Whitespace path rejection**: every path consumed or emitted is whitespace-free, enforced at plan time (Make-style depfiles cannot represent whitespace paths).
- **No execution at plan time**: emitting fragments never invokes `rustc`; the only subprocess is a best-effort `rustc --version` diagnostic probe.
- **Deterministic emission**: identical inputs and environment produce byte-identical fragments. `RUSTFLAGS`, `CARGO_*`, and `RUSTC_WRAPPER` are scrubbed and never read; no timestamps are embedded.
- **No absolute paths in output**: toolchain references are emitted as user-provided names; resolved absolute paths appear in stderr diagnostics only.
- **Trust model**: the crate root is a trusted project input, exactly like a `Makefile`. thorn-rs never fetches or evaluates files from outside the declared crate.
- **No Cargo**: no `Cargo.toml` parsing, no crates.io access, no dependency resolution — the planner constructs exactly one explicit `rustc` invocation.

## Authorship & Review

The majority of the code in this repository was written by an AI. All
architectural design was made by a human, and every change was
reviewed by both a human and an AI for flaws before it landed.
Security-sensitive components (crate ingestion, command planning, and
fragment emission) receive additional review scrutiny.
