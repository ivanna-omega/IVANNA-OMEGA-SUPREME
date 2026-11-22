# 05 — Specifications

**Domain:** IES-001 §05 · Audio Performance & Specifications.

**Master document:** [README.md](../../README.md) §05 (Audio Performance & Specifications).

## What lives here

Technical specifications and measurements. Each capability is an engineering specification sheet: definition, measurement method, instrumentation, result obtained, acceptance criterion. This directory is the index for the specifications domain; the authoritative text is the README master until dedicated specification documents are written.

## Authoritative sources (existing)

- [Benchmarks](../BENCHMARKS.md)
- [Hardware validation (pre-release 2026-11-02)](../HARDWARE_VALIDATION_PRERELEASE_20261102.md)
- [Moto G85 hardware state](../HARDWARE_MOTO_G85_ESTADO.md)
- [Perceptual 5D audit (2026-10-19)](../AUDITORIA_5D_PERCEPTUAL_2026-10-19.md)
- [Performance data](../performance/) (directory)

## Measured (host, 48 kHz, unless stated)

| Capability | Result |
|:---|:---|
| ORBE | THD+N off −153.45 dB / on −139.37 dB @ 1 kHz; IMD −156.04 dB; 23× RT |
| HOLOS (retuned) | THD+N −94.0 dB @ 1 kHz, −79.6 dB @ 100 Hz; IMD −159.5 dB; 3.5× RT |
| Φ | Throughput 41–42× RT; amount 0 bit-exact |
| ASRC core | THD+N −86.00/−84.08/−83.15 dB; underflow 0; 3.59% core |
| NEU·Ω | E2 broadband −137.7 dB; E2/E3 tones −142 dB |
| Ω (v1) | 10/10 host tests; off ⇒ bit-exact; N1 cap +3 dB |

## Not measured (and not claimed)

No ABX blind test, no end-to-end device latency, no temperature in use, no Bluetooth verdict, no measured maximum of the Genezi DAC over USB.
