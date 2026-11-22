# 07 — Validation

**Domain:** IES-001 §08 · Verification & Validation.

**Master document:** [README.md](../../README.md) §08 (Verification & Validation).

## What lives here

Testing and quality assurance: test layers, measured results, the open-defects register, and the perceptual validation plan (ABX + VISCERAL index). This directory is the index for the validation domain; the authoritative text is the README master until dedicated validation documents are written.

## Measured results (current)

| Suite | Result |
|:---|:---|
| CTest (DSP host suite) | **1008/1008** (CI, v2.5.444) |
| ASRC core | **25/25** (host, v2.5.439) |
| Daemon command responder | **28/28** (host, v2.5.440) |
| Council (multi-model) | **14/14** (Python unit tests, v2.5.445) |
| GitHub bridge | **26/26** (Python unit tests, v2.5.445) |
| SOFA inventory | **239/239 readable** |

## Open defects (see README §08.3)

1. Sound still "bofo feo" after the v2.5.441 ISO 226 fix (Route A problem).
2. Route B couples but does not process (fusion_active=0).
3. Tronidos in TIDAL — attribution pending.
4. Assistant canned lines — fixed in v2.5.445, pending release + live confirmation.
5. OmniRoute tunnel unstable (URL rotates; needs a named tunnel).

## Authoritative sources (existing)

- [Moto G85 10-day acceptance plan](../PLAN_10D_MOTO_G85_ACCEPTANCE.md)
- [Termux device validation](../TERMUX_DEVICE_VALIDATION.md)
- [Device self-audit](../DEVICE_SELF_AUDIT.md)
- [Repair release notes](../REPAIR_RELEASE_NOTES.md)
- [Perceptual 5D audit (2026-10-19)](../AUDITORIA_5D_PERCEPTUAL_2026-10-19.md)
- [10-day perceptual roadmap (2026-10-19)](../ROADMAP_10D_PERCEPTUAL_2026-10-19.md)
