# MuseLink deterministic analyzer — reference implementation

Reference implementation of the deterministic analyzer that replaces the
paid chat (Muse IA, blocked by billing since 2026-11-28) as the brain that
compiles sessions into profiles and decides promotions / rollbacks.

**This code lives in the repo so the server can adopt it verbatim.** The
server is the MuseLink app (AI Studio / Cloud Run); it is not in this repo
and nothing here touches it. The analyzer runs on the server next to the
local acoustic compiler, which already updates the profile on every
incoming session, for free. No LLM is ever in the critical path.

Pure Python, standard library only. No I/O, no network, no randomness.
Every rule is a pure, testable function; every threshold is a named
constant in `analyzer.py`, taken from the specifications, never invented.

## Sources

- `refactorizacion-muselink-autonoma.md` §2.3 (deterministic analyzer),
  §4 (A/B machine, the 5 original promotion gates), §6 (v1.1 candidate).
- `evolucion-magistral-autonomia-completa.md` §§S2, S5 (via B), S6, S8,
  S9 (server side).
- Baseline fixed by the specs: spectral error RMS **10.46 dB** (v1.1
  profile), promotion target **≤ 8.37 dB** (0.8 × 10.46).

## Files

| File | What it is |
|---|---|
| `analyzer.py` | The rules. Pure functions + documented constants. |
| `test_analyzer.py` | `unittest` suite (stdlib only). Run `python3 -m unittest test_analyzer -v` from this directory. |
| `README.md` | This file. |

## Inputs / outputs

The analyzer consumes the **compiled 24-band ERB table** of the current
profile (one row per band: `errorMeanDb`, per-band coherence, and —
when available — `erbStdDb`), plus session metadata (`levelMeanDbfs`,
`levelMaxDbfs`, crest, `meanCoherence`, `spectralErrorRmsDb`). The 24
band centres are fixed: 82 … 14693 Hz (`ERB_CENTER_HZ`, verified against
`SessionAggregator.erbCenterHz`).

The main entry point is `compile_candidate(...)`, which runs the whole
pipeline and returns a dict with `regional` (§2.3), `s2` (S2 fit),
`coherence_guard` (S9), `split` (S6) and `ladder` (S5). Each rule is
also callable on its own; the server is free to call them separately.

## The rules

### §2.3 — Regional correction (the base the other rules build on)

`regional_correction(error_mean_db)`:

1. Regional error = arithmetic mean of `errorMeanDb` over the region's
   bands, on the compiled table (an error that survives aggregation is
   persistent, not an isolated chunk).
2. Correction = −(regional error); |error| < **1.0 dB** is noise and is
   not corrected (deadband).
3. Written to the motor slot with clamp **±2 dB** (v1.0 schema limit);
   whatever exceeds the clamp stays as documented `residual_db`.
4. Regions without a slot (subgraves, medios-graves, transición, aire)
   are documented in `unslotted_db`, never applied.
5. Net gain zero: `trim_db = −(sum of applied slots)`.
6. Operators do not change unless an EVO orders it.

`detect_resonances(...)`: a band is a resonance when `error_db ≥ +6 dB`
AND ≥ 3 dB above both neighbours (ERB-3 209 Hz, +9.86 dB, qualifies).
A resonance is cut by the normal rule and noted; never boosted.

Regions (band indices 1-based): graves 1 (60–120 Hz), medios-graves
2–6, medios 7–9, transición 10–12, brillo 13–16, presencia 17–20,
aire 21–24. Slots: `low_60_120` → graves, `mid_500_1k` → medios,
`high_2k_4k` → brillo, `presence_4k_8k` → presencia.

### S2 — Comb vs response (`fit_comb_vs_response`)

Fits `error(f)` with the model

```
error(f) = tilt·log2(f/1000) + modal_gain·modal(f)
           + 3.0 dB·cos(2π·f·2d/c)
```

by least squares over the 24 ERB bands, grid-searching `d` from 0.3 to
3.5 m in 0.01 m steps. `modal(f)` is a fixed shape: Gaussian bumps at
the axial modes **24.5 / 42.9 / 57.2 Hz** and their harmonics (weighted
1/k). Exactly **3 fitted parameters** (tilt, modal gain, d) for 24
points, per the spec, to avoid overfitting. The comb amplitude is
**fixed at 3.0 dB** — a documented modelling choice (the identifiable
part of a comb with unknown reflection coefficient), not a measured
value. The fit **publishes the fitted `d` and its residual**; no wall
distance is ever assumed.

Classification:

| Verdict | Condition |
|---|---|
| `comb` | comb fit cuts the smooth-only residual by **≥ 25%** AND mean `erbStdDb` is **≥ 1.0 dB higher** where the comb predicts nulls (cos < −0.5) than where it predicts peaks (cos > +0.5) |
| `response` | comb fit improves the smooth model by < 25% AND mean `erbStdDb` is low (< 1.0 dB) throughout — smooth deficit, not a comb |
| `inconclusive` | mixed evidence |
| `indeterminate` | `erbStdDb` missing — the rule refuses to guess (see pending data below) |

Physical basis (from S2): a comb with 85.75 Hz spacing inside an ERB
band (width > 1 kHz above ~4 kHz) averages out and cannot produce a
−7 to −24 dB mean ERB deficit between 1.4 and 14.7 kHz; it does produce
high intra-band dispersion. So high dispersion at predicted nulls +
low dispersion where the deficit is smooth = comb; smooth deficit with
low dispersion = response (target curve / Genezi / mic position).

### S6 — Cuts first, boosts after (`split_cut_boost`, `precheck_headroom`)

`split_cut_boost(proposed_db, level_max_dbfs, exciter_amount=0.3,
v1_1a_promoted=False, room_corr_cut_db=None, room_corr_available=False)`:

- **v1.1a (cut-only):** negative corrections only, clamped to −2 dB;
  `trim_db` compensates only the cuts (net gain 0). The ERB-3 cut via
  `roomCorrDb` belongs here, but only when `room_corr_available` is
  True — i.e. once S3 has wired the `roomCorrDb` JNI slot. Until then
  v1.1a uses only `low_60_120` and the ERB-3 cut is reported as pending
  S3.
- **v1.1b (boost):** positive corrections only, clamped to +2 dB.
  Emitted **only if v1.1a promoted** (it must have won its A/B and freed
  headroom first). Otherwise it is `None` with the reason recorded.

**Pre-check (deterministic, before emitting any candidate):**

```
levelMaxDbfs + maxBoost ≤ −0.1 dBFS        (APEX ceiling 0.9885531)
```

If it fails, the boost is scaled down deterministically until it
passes. If the live level is already at/above the ceiling even with
zero boost, the `exciter_amount` is also zeroed and emission is
**blocked** (never let the limiter eat the signal). The harness
measured the sweep at −0.10 dBFS / TP −0.11 dBTP with overCeil 0: there
is no headroom left, which is why this check exists at all.

### S8 — Hardened A/B gates (`evaluate_ab`)

Over **3 candidate sessions vs 3 base sessions**. Each session carries
`spectral_error_rms_db`, 24-band `error_mean_db`, `mean_coherence`,
`level_max_dbfs` (worst chunk), `level_mean_dbfs`; `crest_db` and
`lufs` are optional (used when present, skipped + noted when absent).

All gates must pass; any failure → `rollback`:

| Gate | Rule |
|---|---|
| §4-1 ERB improvement | `(RMS_base − RMS_cand) / RMS_base ≥ 20%` (target ≤ 8.37 dB from 10.46 dB) |
| §4-2 Coherence | `meanCoherence_cand ≥ meanCoherence_base − 0.05` (with a 0% base this does not discriminate; kept so it does when S1/S2 move coherence off 0) |
| §4-3 No new resonance | no band ≥ +6 dB in the candidate that was not already ≥ +6 dB in the base |
| §4-4 No clip | no chunk with `levelMaxDbfs ≥ 0 dBFS` |
| **S8(a) Region RMS** | no region worsens by **> 1 dB**, even if the global RMS improves. Groups: graves/medios-graves (ERB 1–6), medios (7–9), brillo (13–16), presencia (17–20), aire (21–24). Transition bands (10–12) belong to no S8 group and are covered only by the global gate. |
| **S8(b) APEX not working harder** | crest must not fall **> 0.5 dB**, and the candidate's `levelMaxDbfs` must not sit pegged at −0.1 dBFS (within 0.05 dB) while the base did not |
| **S8(c) Loudness matching** | `|levelMean_cand − levelMean_base| ≤ 3 dB` (§4 session-equivalence tolerance); when `lufs()` exists on both sides, `|LUFS_cand − LUFS_base| ≤ 1 dB` — a louder candidate is not a better candidate |

`evaluate_ab` returns `{"verdict": "promote" | "rollback", "gates":
{...}, "base": <summary>, "candidate": <summary>}` with a per-gate
`passed` flag and a human-readable `detail` string.

### S5 via B — Deterministic +2 dB ladder (`emit_ladder`)

`emit_ladder(current_applied_db, target_db, base_version="v1.1")` closes
a residual as a ladder of **+2 dB per version rung** (v1.1 → v1.2 →
v1.3 …), each rung with its own A/B. The +7 dB residual in
brillo/presencia closes in 4 rungs (v1.2 +4, v1.3 +6, v1.4 +8,
v1.5 +9). Every rung recomputes `trim_db = −(sum of slots)` (net gain
0), is marked `requires_ab: True` and `reversible: True`, and **never
widens the clamp in one jump** — a bigger clamp would widen exactly the
damage the fail-closed validation (C2) is closing. Via B needs no
format decision from Esse (via A, schema v1.1 with ±12 dB slots, does).

### S9 — Coherence guard, server side (`coherence_guard`)

`coherence_guard(mean_coherence, per_band_coherence, corrections_by_band,
ab_measurement_only=False)`:

- `mean_coherence ≥ 0.20` → every correction allowed (`mode: "full"`).
- `mean_coherence < 0.20` → only corrections in bands whose own
  coherence is `> 0.20` are allowed (`mode: "partial"`); the rest are
  blocked. With no band above 0.20, nothing is emitted (`mode:
  "none"`).
- `mean_coherence < 0.20` and no per-band coherence available → nothing
  is emitted (cannot tell which bands are trustworthy).
- `ab_measurement_only=True` → everything is allowed but marked
  `mode: "measurement_only"`: the A/B runs as a measurement, never as a
  path to promotion. (The phone-side `ProfileApplier` must reject
  candidates with `mean_coherence < 0.20` unless they carry this mark.)

With today's coherence (0%), the guard is active: the v1.1 candidate
emits nothing for auto-application unless marked measurement-only, and
EVO-003 runs in parallel to fix coherence itself.

**Known interaction, out of scope here:** S10 says a regional residual
> 4 dB should trigger EVO-003 first and freeze EQ steps in that region
until coherence rises or a measurement confirms the deficit. This
module plans the ladder (S5) but does not implement the S10 freeze —
that is a separate proposal needing Esse's decision + a physical
measurement.

## How the server (AI Studio) adopts it

1. Copy `analyzer.py` verbatim next to the local acoustic compiler (it
   has no dependencies beyond the standard library).
2. Feed it the compiled 24-band table the compiler already maintains
   (`errorMeanDb`, per-band coherence, `meanCoherence`,
   `spectralErrorRmsDb`, `levelMeanDbfs`, `levelMaxDbfs` per chunk).
3. On each new compiled profile, call `compile_candidate(...)`; publish
   the returned v1.1a / v1.1b as `candidate` versions through the
   existing `GET /api/v1/muse/ivanna/profile/current` endpoint.
4. On the 3 A/B sessions of a candidate, call `evaluate_ab(base_3,
   candidate_3)`; publish `promoted` or `rolled_back` from its verdict.
5. After a promotion, the next rung of `ladder` becomes the next
   candidate (each rung gets its own A/B; a failed rung rolls back to
   the previous rung, never to v1.0 directly unless the whole climb is
   abandoned).

The phone side (ProfileFetcher / ProfileApplier, next release) is
unchanged by this module except for one obligation it already carries
in the spec: reject a candidate whose `mean_coherence < 0.20` unless it
is marked `ab_measurement_only`.

## Pending data from the server (not invented here)

1. **`erbStdDb` per band of the compiled profile — MISSING.** The phone
   computes it (`SessionChunk.erbStdDb`, `SessionAggregator.kt:68,214`)
   but `OrganismPayload.kt` (lines 60–68) sends only `magnitudeDb`,
   `errorDb` and `coherence` per band — `erbStdDb` never reaches the
   server. Until the payload and the compiler retain it, S2 returns
   `indeterminate` with reason `erb_std_db_missing` (the fit still
   publishes `d` and its residual; only the classification refuses to
   guess). S2 is specified as pending on exactly this.
2. **Full 24-band ERB table export.** The v1.1 profile documents only
   ERB-3 (+9.86 dB); the other 23 bands "live on the server". The
   analyzer needs the whole table; verify it is exportable from the
   compiled profile.
3. **`lufs()` telemetry.** S8(c) uses it when present; it does not exist
   yet (`FusedTonal.lufs()` is new telemetry). Until then the loudness
   gate runs on `levelMeanDbfs` alone and notes the skip.
4. **`crest_db` per chunk.** S8(b) uses it when present; skipped + noted
   when absent. (The v1.1 live-session profile has no measured crest —
   it is a file-measurement field.)
5. **S3 (`roomCorrDb` JNI slot).** Until S3 lands, v1.1a cannot carry
   the ERB-3 cut and uses only `low_60_120`; the module reports this
   instead of pretending the cut is applied.

## Discrepancies noted

- **None between the two specs.** §2.3 and §§S2/S5/S6/S8/S9 agree on
  every threshold used here. Two gaps are in the *data*, not the specs
  (items 1–2 above), and one threshold had to be fixed as a modelling
  choice rather than taken from a spec: the S2 comb amplitude (3.0 dB,
  fixed — see S2 above), because the spec fixes only the 3 fitted
  parameters (tilt, modal gain, d) and a comb needs an amplitude.
- **S8(b) crest threshold (0.5 dB) and S8(c) LUFS threshold (1.0 dB)**
  are the implementer's fixed values for "the limiter works more" and
  "louder is not better": the spec names the proxies (crest drop,
  levelMax pegged at −0.1 dBFS, `lufs()`) but gives no numbers for
  them. They are named constants (`AB_CREST_DROP_MAX_DB`,
  `AB_LUFS_MATCH_DB`) so the server can tune them from measured data.
- **S8(a) transition bands (ERB 10–12)** belong to no named S8 region
  group in the spec; they are covered only by the global RMS gate.
  This is documented in the code, not silently assigned to a group.
