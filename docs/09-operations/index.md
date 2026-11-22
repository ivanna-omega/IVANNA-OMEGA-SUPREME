# 09 — Operations

**Domain:** IES-001 §07 · Infrastructure & Deployment.

**Master document:** [README.md](../../README.md) §07 (Infrastructure & Deployment).

## What lives here

Deployment and operations: build, CI/CD, releases, environments. This directory is the index for the operations domain; the authoritative text is the README master until dedicated operations documents are written.

## Authoritative sources (existing)

- [Deployment instructions](../DEPLOYMENT_INSTRUCTIONS.md)
- [Changelog](../../CHANGELOG.md)
- [Fixes: connectivity](../FIXES_CONECTIVIDAD.md)
- [Diagnostic bridge](../diagnostic-bridge/) (repository directory; the live bridge is at `ivanna-diagnostic-bridge.floot.app`)

## Build (verified)

| Item | Value |
|:---|:---|
| minSdk / compile / target SDK | 28 / 35 / 35 (supported scope: Android 13+) |
| NDK / CMake / JDK | 27.2.12479018 / 3.30.3 / 17 |
| Language | C++26 (DSP) · Kotlin (app) |

```bash
./gradlew :app:assembleDebug
bash scripts/run_ctest.sh
```

## Releases

CI publishes to GitHub Releases on version bump (APK + Magisk zip + `SHA256SUMS.txt`). Identify APKs only by SHA256. The signing key is stable since v2.5.425 (in-place updates). The in-app updater (v2.5.444) checks at most once a day and verifies SHA-256; no silent installation.

## Version bump rule (6 sources must agree)

`version.properties` · `magisk_module/module.prop` · `magisk_module/update.json` · CHANGELOG · README · User-Agent in `TidalApiClient.kt`.
