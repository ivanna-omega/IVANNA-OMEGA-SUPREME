# 06 — Development

**Domain:** IES-001 §06 · Software Architecture & APIs.

**Master document:** [README.md](../../README.md) §06 (Software Architecture & APIs).

## What lives here

Developer handbook and APIs: modules, contracts, dependencies, interfaces and services. This directory is the index for the development domain; the authoritative text is the README master until a dedicated developer handbook is written.

## Authoritative sources (existing)

- [Engineering notes](../ENGINEERING.md)
- [Evolution pipeline (autonomous self-repair)](../../tools/evolution/PIPELINE.md)
- [Evolution motor README](../../tools/evolution/README.md)
- [Wiring matrix](../WIRING_MATRIX.md)
- [UI wiring audit](../UI_WIRING_AUDIT.md)
- [Complete controls guide (Español)](../GUIA_CONTROLES_COMPLETA_ES.md)
- [Contributing](../../CONTRIBUTING.md)

## Stack (verified)

Kotlin + Jetpack Compose (app, minSdk 28, target SDK 35) · C++26 + NDK 27.2.12479018 + CMake 3.30.3 + JDK 17 (DSP core) · OmniRoute gateway v3.8.53 (VM) + Gemini (assistant) · Floot diagnostic bridge.

## Rules

- One commit per change, format `type(area): description`, direct to `main`.
- Never delete code (mark it legacy). Never fabricate metrics. Verify before trusting.
- A new telemetry field updates its test in the same commit; a changed directive value updates the tests asserting it in the same commit.
- Never touch: `app/build.gradle.kts`, `app/src/main/cpp/CMakeLists.txt`, `app/src/main/java/com/ivanna/omega/security/`, `.github/workflows/`, `tools/security/`, keystores, secrets.
