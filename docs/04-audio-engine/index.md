# 04 — Audio Engine

**Domain:** IES-001 §04 · Audio Signal Processing.

**Master document:** [README.md](../../README.md) §04 (Audio Signal Processing).

## What lives here

Signal processing and the audio pipeline: the DSP chain order, ASRC, the servo, TIDAL playback, the assistant and OmniRoute, autonomous self-repair. This directory is the index for the audio-engine domain; the authoritative text is the README master until dedicated engine documents are written.

## Authoritative sources (existing)

- [TIDAL contracts (official program vs unofficial endpoint)](../TIDAL_CONTRACTS.md)
- [ASRC Moto G85 deep dive](../ASRC_MOTO_G85_IMPLEMENTATION_DEEP_DIVE.md)
- [ASRC software clock wiring](../ASRC_SOFTWARE_CLOCK_WIRING.md)
- [Route B daemon fix](../RUTA_B_DAEMON_FIX.md)
- [AUX Genezi route](../AUX_GENEZI_ROUTE.md)
- [Evolution pipeline (autonomous self-repair)](../../tools/evolution/PIPELINE.md)
- [Benchmarks](../BENCHMARKS.md)

## Measured (host)

- ASRC core: 25/25 tests; drift +0.000/+0.001/−0.011 ppm; THD+N −86.00/−84.08/−83.15 dB; underflow 0; 3.59% of one core.
- Route A ASRC: THD+N −138.8 dB at ratio 1.0 / −95.0 dB at +100 ppm.
