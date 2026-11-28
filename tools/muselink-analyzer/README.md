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
  S9 (server side), **S10 (residual freeze + EVO-003)**, **S15 (P0/D10
  covariates)**, **S16 (EVO-003 probe — verification only, does not
  trigger)**, **T3 ("sounding, not just installed" counting)**,
  **T6 (excursion cap + promotion brake)**,
  **T7 (anti-poisoning, before aggregation)**.
- Baseline fixed by the specs: spectral error RMS **10.46 dB** (v1.1
  profile), promotion target **≤ 8.37 dB** (0.8 × 10.46).

## Files

| File | What it is |
|---|---|
| `analyzer.py` | The rules. Pure functions + documented constants. |
| `test_analyzer.py` | `unittest` suite (stdlib only, 125 tests). Run `python3 -m unittest test_analyzer -v` from this directory, or `python3 -m unittest discover -s tools/muselink-analyzer` from the repo root. |
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
`coherence_guard` (S9), `s10` (S10 freeze), `split` (S6) and `ladder`
(S5), plus `t6` (T6 excursion verdict on the planned climb). Each rule
is also callable on its own; the server is free to call them separately.

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
| **S15 D10/P0 covariates** | when the telemetry exists on both sides, the candidate must not move the D10 action or the P0 by more than **±0.5 dB**. A move outside tolerance makes the verdict **`inconclusive`** — not a rollback: the candidate did not fail on its merits, the comparison itself is invalidated. When the telemetry is absent the gate is skipped + noted. A candidate that fails any §4/S8 gate is still a `rollback` even if it also moved D10/P0. See §S15 below. |

`evaluate_ab` returns `{"verdict": "promote" | "rollback" |
"inconclusive", "gates": {...}, "base": <summary>, "candidate":
<summary>}` with a per-gate `passed` flag and a human-readable
`detail` string.

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

### S10 — Residual > 4 dB: diagnose before climbing further (`check_residual_freeze`)

`check_residual_freeze(regional, mean_coherence=None,
deficit_confirmed=False)` implements the S10 rule on the output of
`regional_correction(...)`:

- A region **freezes** when its |residual| is **strictly > 4 dB**
  (`S10_RESIDUAL_FREEZE_DB`; exactly 4.0 dB does not trigger). Frozen
  regions propose **no EQ step**: their slots leave the S6 split and
  their ladder targets stay at the currently applied value, so the
  ladder does not climb there. Regions without a motor slot
  (transición, aire) have no step to freeze, but they still trigger
  the diagnostic — their residual is uncorrected error.
- A freeze **marks EVO-003 as the next required step**
  (`evo003_required: True`): the physical measurement (distance to
  walls / test by position) that decides whether the residual is a
  real room deficit or a measurement artefact.
- The freeze is **released** when the EVO-003 measurement confirms
  the deficit (`deficit_confirmed=True`, passed to
  `compile_candidate`), or when mean coherence rises to the S9 guard
  (`≥ 0.20`, reused here as "coherence has risen" — a modelling
  choice, not a measured value; the spec fixes no number for it).
  A released freeze still reports the regions that triggered, so the
  diagnostic signal is not lost.

**The triggering case is the current data itself.** With the v1.1
table at coherence 0%: residuals +7 dB in brillo/presencia (and +7 dB
in transición, +12 dB in aire) freeze slots `high_2k_4k` /
`presence_4k_8k`; the ladder plans only the unfrozen climb (one rung
v1.2: low −3 dB, mid +4 dB). With coherence 0.9 the freeze releases
and the ladder climbs to v1.5 again — where T6 still caps it at v1.3.
So S10 and T6 interact in this order: S10 stops the climb first
(diagnose), T6 stops it second (human limit).

**EVO-003 firing stays pending the S1 reference fix.**
`evo003_pending_reference_fix` is True exactly when EVO-003 is
required and mean coherence is below the guard: with coherence 0% by
instrumentation (S1 — the Organism path does not feed the reference),
the residual cannot be trusted as a room deficit, and a position
probe would measure the instrument, not the room. The analyzer marks
EVO-003 as the next required step, but its firing (the physical
measurement, S16) does **not** trigger until S1 is fixed — see
§S2/S16 below. Do not run the probe to "confirm" a residual that the
instrument itself produced.

### S15 — P0/D10 as A/B covariates (`check_d10_p0_covariates`)

`check_d10_p0_covariates(base_sessions, candidate_sessions,
tolerance_db=0.5)` keeps the A/B from mistaking "the field changed"
for "the EQ improved". P0 measures −6.065/−6.274 dB (ALL7 −14.18 dB)
without trim; the v1.1 `trim_db = −4.0` is nominal from the file, not
measured loudness; the D10 arbiter is legacy as gain (telemetry only)
and the LTI D10 kernel is still engaged.

- The analyzer **accepts** the optional chunk/session fields
  `d10_action`, `p0_db`, `trim_db_effective` when they arrive
  (`validate_chunk` checks them for finiteness only — no range is
  fixed for the D10 action or the P0 in any spec, so no range is
  invented) and **does not require them** when they do not arrive
  (retrocompatible: sessions without them are not rejected, and the
  covariate gate is skipped + noted).
- **Rule:** if the candidate moves the D10 action or the P0 outside
  **±0.5 dB** (`D10_P0_TOLERANCE_DB` — the spec's value, "a fijar con
  la primera medición", taken as the explicit default until the first
  measurement confirms or changes it), the A/B verdict is
  **`inconclusive`** even if the RMS improves. See the S8 table above
  for how `inconclusive` ranks against `rollback`.
- `trim_db_effective` is **reported, not gating**: its delta rides in
  the result, but the spec fixes no tolerance for the trim, and
  inventing one would be inventing a metric.
- Nothing here touches the arbiter or the kernel: telemetry + a rule,
  nothing else.

**Status today (verified in this worktree): the telemetry does not
arrive yet.** No D10/P0 field exists in `SessionChunk` /
`OrganismPayload` (`app/src/main/java/com/ivanna/omega/organism/`,
grep at Fase 6). Until the phone sends it, the S15 gate is
`unchecked` on every A/B and the verdict is unaffected — the rule is
in place for the day the fields land, not a claim that they do.

### T3 — "Sounding, not just installed" (`check_playing_evidence`, `count_playing_chunks`)

The spec's `POST /api/v1/muse/ivanna/profile/applied` only says
"installed". A profile installed with the daemon down, no music, or
the sensor stopped would generate 3 "empty" sessions that the server
could read as a failure of the candidate (or as a success through
silence). The server-side counting rule:

`check_playing_evidence(evidence, previous_beacon_frames=None)`
decides whether **one chunk counts** toward the A/B. It counts only
when **all four** evidence checks pass:

| Check | Rule |
|---|---|
| `daemon_ok` | must be `True` — it is what distinguishes "applied without a daemon" from "applied and sounding". Missing or `False` → does not count. |
| `frames_valid` | must be `True` (bool) or `> 0` (int count). Missing, `False` or `0` → does not count. |
| `snapshots_accepted` | must be `> 0` (snapshots the sensor consumer accepted). Missing or `0` → does not count. |
| `beacon advancing` | `effect_beacon_frames` must be **strictly greater** than the previous observation for this candidate. A first observation, with no previous value, cannot establish advance yet and does not count — the server keeps the previous value and waits for the next chunk. |

A chunk without evidence **does not count: it is neither a success
nor a failure**, it simply does not advance the A/B count.
`count_playing_chunks(evidences, previous_beacon_frames=None)` threads
the beacon state across an ordered list of chunk evidences and returns
how many counted (`counted` / `not_counted`, per-chunk decisions,
`last_beacon_frames`). This module never defines session boundaries —
the server owns them; it feeds ordered chunk evidences here and counts
its 3 A/B sessions from the chunks that count. The phone-side half of
T3 (exposing `daemonOk` in `OrganismController.State` and sending the
evidence in the confirmation / first chunk) is not in this module; the
pieces already exist on the phone (`framesValid` is in telemetry,
`snapshotsAccepted` in the consumer state, the beacon via
`DSPBridge.effectBeaconFrames()`).

### S2/S16 — What S2 concludes with the current data (measured, Fase 6)

Verified by running `fit_comb_vs_response` on the v1.1 table
(`regional` residuals above) with the data the server actually has:

| Fact | Value (measured) |
|---|---|
| `erbStdDb` in the compiled server profile | **missing** — the phone computes it (`SessionAggregator.kt:68,214`) but `OrganismPayload.kt:92-93` sends only `magnitudeDb`, `errorDb`, `coherence` per band (verified in this worktree, HEAD `d22885b`) |
| S2 verdict | **`indeterminate`**, reason `erb_std_db_missing` — the rule refuses to guess |
| Comb fit (published anyway) | distance `d = 3.11 m`, residual 1.42 dB vs smooth-only 1.56 dB, improvement **9.0%** — far below the 25% comb bar (`COMB_IMPROVEMENT_MIN`); the fit itself does not favour a comb |
| `std_contrast_db` | `None` (no dispersion data to contrast) |
| S1 premise (Fase 0 audit) | coherence 0% is **instrumentation**: the Organism path does not feed the reference, so `coherence = 0` in all 24 bands by design |

**Conclusion, exact: S1 + S2 do NOT point to the room, so S16 (the
physical EVO-003 measurement) does NOT trigger.** S1 says the 0% is
not room data, and S2 cannot distinguish comb from response without
`erbStdDb` — and even the fit it does run leans away from a comb
(9.0% < 25%). The next step is the S1 reference fix + `erbStdDb` in
the payload, not a position probe: a probe now would measure the
instrument, not the room. If, after the fix, S2 returns `comb` (or
coherence behaves like a room effect), S16 triggers then — the probe
protocol (3 labelled positions, same track/level) is specified in
evolucion-magistral-autonomia-completa.md §S16 and needs no change
here. Pinned by `TestS2S16Verification`, so a future data change
fails loudly instead of silently re-triggering the probe.

## How the server (AI Studio) adopts it

1. Copy `analyzer.py` verbatim next to the local acoustic compiler (it
   has no dependencies beyond the standard library).
2. Feed it the compiled 24-band table the compiler already maintains
   (`errorMeanDb`, per-band coherence, `meanCoherence`,
   `spectralErrorRmsDb`, `levelMeanDbfs`, `levelMaxDbfs` per chunk).
3. **T7 first: on every incoming session, call `gate_session(...)`
   BEFORE the session enters the aggregate** (see §T7 below). Only a
   session with decision `accepted` is aggregated; a `held` session
   waits in the quarantine state; a `rejected` session never enters.
   The server must persist, per token, the accepted-session timestamps,
   the seen session_ids / error tables (or their hashes), the
   quarantine state and the rejection log — all of it is passed in and
   returned by `gate_session`, never stored by this module.
4. On each new compiled profile, call `compile_candidate(...)`; publish
   the returned v1.1a / v1.1b as `candidate` versions through the
   existing `GET /api/v1/muse/ivanna/profile/current` endpoint.
   **Read the `s10` entry first:** if regions are frozen, do not
   publish EQ steps for them — EVO-003 is the next required step, and
   while `evo003_pending_reference_fix` is True its firing waits for
   the S1 reference fix (see §S10 above).
5. **T3 before counting:** on every chunk that arrives with a candidate
   applied, call `check_playing_evidence(...)` (threading the previous
   beacon value, or `count_playing_chunks(...)` over the ordered
   chunks). Only counted chunks advance the A/B count; an uncounted
   chunk is neither a success nor a failure. The 3 A/B sessions are
   counted from counted chunks only.
6. On the 3 counted A/B sessions of a candidate, call
   `evaluate_ab(base_3, candidate_3)`; publish `promoted`,
   `rolled_back`, or `inconclusive` from its verdict. `inconclusive`
   (S15: the candidate moved D10/P0) means **repeat the A/B, do not
   promote and do not roll back** — the comparison was invalidated.
   The S15 gate only fires when the D10/P0 telemetry exists on both
   sides; until then it is skipped + noted.
7. After a promotion, the next rung of `ladder` becomes the next
   candidate (each rung gets its own A/B; a failed rung rolls back to
   the previous rung, never to v1.0 directly unless the whole climb is
   abandoned).
8. **T6 before every promotion: call `gate_promotion(rung_slots,
   promotions_today)`** (see §T6 below). A promotion the gate refuses
   is not published; an excursion breach (`halt_needs_human`) stops
   the cycle until Esse decides — do not publish that rung or any
   later one. `compile_candidate` already reports the excursion
   verdict of the whole planned climb in its `"t6"` entry, so the
   server knows in advance which rung would breach the cap.

### T7 — Anti-poisoning: reject anomalous sessions before compiling

Closes ARTEX risk no. 2 (C17): a corrupt, replayed or malicious session
must not poison the profile that later self-applies. The A/B compares
against a baseline but cannot tell "a real room that changed" from
"false data", so the defence runs **before aggregation**, in this
order, via `gate_session(session, aggregate_error_db, token_history,
now_epoch, quarantine_state, rejection_log, ...)`:

| # | Rule | Function | Behaviour |
|---|---|---|---|
| 1 | Physical ranges | `validate_chunk` | Rejects a chunk with a non-finite value (NaN/inf) or a value outside its physical range: per-band `errorDb` ±40 dB, `magnitudeDb` −120…0 dBFS, coherence 0…1, `levelMean/levelMax` −120…0 dBFS, `spectralErrorRmsDb` 0…40 dB, `erbStdDb` 0…40 dB, `crest` 0…60 dB, `lufs` −70…0 dB. Every reason names the field, the band and the offending value. |
| 2 | Replay | `check_replay` | Rejects a `session_id` already accepted, or an error table bit-identical to an earlier accepted session (a genuine new session never reproduces 24 floats exactly). |
| 3 | Per-token rate | `check_token_rate` | Rejects when the token already contributed `max_sessions_per_hour` sessions inside the last hour (default **6/h**). The limit is a **parameter** of `gate_session` — the server passes its own value, because it is the one that knows the token. |
| 4 | Per-band outlier hold | `classify_session_outlier` + `quarantine_update` | A session deviating **> 6 dB** in any band from the aggregated profile is **held, not aggregated**, until **2 further sessions** confirm the same deviation (same band, same sign, > 3 dB). Confirmed → released and aggregated (the room really changed). Not confirmed by the next 2 sessions → discarded. A held session is logged as `held`, not as a rejection. |
| — | Audit | `log_rejection` / `rejection_summary` | Every rejection and every hold is recorded with `session_id`, `token`, `rule` (`chunk_out_of_range` / `replay` / `rate_limit` / `outlier_hold`), `decision` (`rejected` / `held`) and a human-readable `reason`. The module has no clock: the server stamps entries with its own timestamp when it persists them. |

**Thresholds — proposals, not measured data.** The T7 proposal leaves
the thresholds to the implementer when Esse does not fix them. Every
number above is a reasoned proposal from the known data, and each one
is a named constant in `analyzer.py` so the server can tune it from
measured data without touching the rules:

- **Error ±40 dB:** the v1.1 measured band errors span −12…+9.86 dB; a
  ±40 dB error in one ERB band means 100× energy excess/deficit and is
  not a measurement of this system.
- **Magnitude −120…0 dBFS:** 0 dBFS is digital full scale (not a
  proposal); −120 dBFS is far below the 16-bit noise floor (−96 dBFS).
- **Coherence 0…1, crest ≥ 0:** by definition, not proposals.
- **Rate 6/h:** an A/B needs 3 sessions per side and a phone flushes at
  most a few listening sessions per hour in real use; 6/h is generous
  for one token and still blocks a replay flood.
- **Outlier > 6 dB, confirm > 3 dB:** 6 dB is well above the 1 dB
  correction deadband and above any plausible single-band drift of a
  real room change, while a genuine change (mic/speakers moved)
  reappears in the next sessions and confirms; the confirmation bar is
  half the hold bar so a real but weaker repeat still counts.
- **2 confirmations:** fixed by the T7 proposal itself ("hasta que se
  confirme en 2 sesiones más").

**What the server must do on each `gate_session` result:**

- `accepted` → aggregate the session (and any `released_sessions`
  returned in the decision), then record `now_epoch` in the token's
  history, the `session_id` in the seen set and the error table in the
  seen tables (or their hashes).
- `held` → aggregate nothing yet; keep the returned
  `quarantine_state` for the next sessions.
- `rejected` → aggregate nothing; the reason is already in the
  returned `rejection_log` — persist it (with the server's timestamp)
  and surface it in triage (`rejection_summary` counts by rule).

**Cold start:** with no aggregate profile yet, the outlier rule has
nothing to compare against and accepts the session; the range, replay
and rate rules still apply.

The phone side (ProfileFetcher / ProfileApplier, next release) is
unchanged by this module except for one obligation it already carries
in the spec: reject a candidate whose `mean_coherence < 0.20` unless it
is marked `ab_measurement_only`.

### T6 — Cumulative excursion cap + promotion brake (antichurn)

Successive +2 dB rungs can walk a slot to +6/+8 dB over several
versions without any human ever listening — the per-version clamp
(±2 dB) does not limit the accumulated drift. And without a brake, a
server publishing a new version at every poll can keep the phone
applying profiles non-stop: the A/B never completes its 3 stable
sessions, the motor never settles, and a rollback can be reversed at
the next poll. The server-side half of T6 is two pure gates, run
before a promotion is published, via
`gate_promotion(candidate_slots_db, promotions_today)`:

| # | Rule | Function | Behaviour |
|---|---|---|---|
| 1 | Cumulative excursion cap | `check_cumulative_excursion` | No slot's cumulative value vs the factory baseline (0.0 dB in every slot — the factory profile carries no EQ correction) may exceed **±4 dB**. Exactly at the cap is allowed. A breach **HALTS the cycle** (`halt_needs_human`): the server stops publishing and asks Esse — it does not silently truncate the climb. `compile_candidate` reports the same verdict for the whole planned climb in its `"t6"` entry, with `first_breaching_rung` naming the first rung that would breach it. |
| 2 | Promotion brake | `check_promotion_brake` | At most **1 promotion per calendar day**. The server counts how many promotions it already published today and passes the count. A breach is a **WAIT** (publish again tomorrow), not a halt. |

**Values — proposals, not measured data.** The T6 proposal leaves
the cap and the brake to Esse ("solo él puede fijar ese límite"), and
gives the cap only as an example ("p. ej. ±4 dB"). The values above
are that example taken as the explicit default until Esse confirms or
changes them: `CUMULATIVE_EXCURSION_CAP_DB = 4.0`,
`MAX_PROMOTIONS_PER_DAY = 1`. They are named constants so the server
can tune them — and Esse can fix them with one number each — without
touching the rules. The phone-side half of T6 (dwell of 2 polls, max
2 automatic applies per day, 24 h veto of a rolled-back version, A/B
timeout of 4 h without 3 sessions, master switch also stopping the
poll) lives in `OrganismController.kt`, not here: this module never
sees the phone.

**What the server must do on each `gate_promotion` result:**

- `allowed` → publish the promotion; count it in today's promotions.
- `allowed` False with `halt_needs_human` False → promotion brake:
  do not publish today; the rung stays queued for tomorrow.
- `halt_needs_human` True → stop the cycle: do not publish this rung
  or any later one, and surface the excursion reason to Esse for a
  human decision. The climb resumes only when he fixes a new cap (or
  accepts the excursion explicitly).

**Honest note on the v1.1 climb:** the v1.1 ladder as planned (targets
+4 dB mid, +9 dB brillo/presencia) breaches the ±4 dB cap at rung
v1.3 — the T6 gate says the climb stops there unless Esse raises the
cap. That is the proposal working as designed: +9 dB in a slot is a
change no one should make without him listening first.

## Pending data from the server (not invented here)

1. **`erbStdDb` per band of the compiled profile — MISSING.** The phone
   computes it (`SessionChunk.erbStdDb`, `SessionAggregator.kt:68,214`)
   but `OrganismPayload.kt` (lines 92–93, verified in this worktree)
   sends only `magnitudeDb`, `errorDb` and `coherence` per band —
   `erbStdDb` never reaches the server. Until the payload and the
   compiler retain it, S2 returns `indeterminate` with reason
   `erb_std_db_missing` (the fit still publishes `d` and its residual;
   only the classification refuses to guess). S2 is specified as
   pending on exactly this. **Measured consequence (Fase 6):** with
   the v1.1 table the fit gives `d = 3.11 m` and only 9.0% improvement
   over the smooth model — below the 25% comb bar — so even the fit
   leans away from a comb; S1+S2 do not point to the room and S16
   does not trigger (see §S2/S16 above).
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
6. **S15 telemetry (`d10_action`, `p0_db`, `trim_db_effective`) —
   MISSING.** No D10/P0 field exists in `SessionChunk` /
   `OrganismPayload` today (verified by grep in `organism/` at Fase 6).
   The analyzer accepts the fields when they arrive and does not
   require them until then; the S15 gate is `unchecked` on every A/B
   meanwhile. Adding the fields is phone-side work (S15's "dato que
   falta"); the server-side rule is already in place.
7. **T3 phone-side wiring — PENDING.** The evidence pieces exist on
   the phone (`framesValid` already in telemetry,
   `snapshotsAccepted` in the consumer state, the beacon via
   `DSPBridge.effectBeaconFrames()`), but `daemonOk` is not yet
   exposed in `OrganismController.State` and the evidence is not yet
   sent in the confirmation / first chunk after applying. Until that
   lands, the server has no evidence to count from and every chunk is
   `not_counted` — which is the safe default (no A/B runs on
   silence), not a failure.

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
- **T7 thresholds are proposals, not measured data** (see §T7 above):
  the T7 proposal leaves them to the implementer when Esse does not
  fix them. Error ±40 dB, magnitude −120…0 dBFS, rate 6/h, outlier
  > 6 dB with confirmation > 3 dB in 2 sessions — each is a named
  constant (`CHUNK_*`, `DEFAULT_MAX_SESSIONS_PER_HOUR`,
  `OUTLIER_DEVIATION_DB`, `OUTLIER_CONFIRM_DB`,
  `OUTLIER_CONFIRM_SESSIONS`) for Esse to confirm or adjust. The
  rate limit is a parameter of `gate_session`, not a buried constant.
  The 0 dBFS / coherence 0…1 / crest ≥ 0 bounds follow from the
  definitions and are not proposals.
- **T6 values are proposals too** (see §T6 above): the proposal leaves
  the cap and the brake to Esse and gives the cap only as an example
  ("p. ej. ±4 dB"). The defaults (`CUMULATIVE_EXCURSION_CAP_DB =
  4.0`, `MAX_PROMOTIONS_PER_DAY = 1`; phone side: dwell 2 polls, max
  2 auto-applies/day, 24 h veto, 4 h A/B timeout) are the proposal's
  own numbers taken as explicit defaults until Esse confirms or
  changes them. The 0.0 dB factory baseline is not a proposal: the
  factory profile carries no EQ correction.
- **S8(a) transition bands (ERB 10–12)** belong to no named S8 region
  group in the spec; they are covered only by the global RMS gate.
  This is documented in the code, not silently assigned to a group.
- **S10 release threshold is a modelling choice, not a measured
  value.** The spec says the freeze lifts "hasta que la coherencia
  suba o la medición confirme el déficit" but fixes no number for
  "suba". This module reads it as `mean_coherence >= COHERENCE_GUARD`
  (0.20), reusing the S9 guard for "this table can be trusted", and
  the other release is explicit evidence (`deficit_confirmed=True`
  from the EVO-003 measurement). The trigger itself (residual
  strictly > 4 dB) is the spec's own number. The interaction is
  measured, not assumed: with the v1.1 table at coherence 0% the
  freeze stops the ladder at one rung (v1.2, low −3 / mid +4); at
  coherence 0.9 it releases and T6 caps the climb at v1.3.
- **S15 tolerance (±0.5 dB) is the spec's value, pending the first
  measurement.** The spec says it is "a fijar con la primera
  medición"; 0.5 dB is taken as the explicit default until that
  measurement confirms or changes it (`D10_P0_TOLERANCE_DB`, a
  parameter of `check_d10_p0_covariates`). `trim_db_effective` is
  reported but not gated — the spec fixes no trim tolerance — and the
  S15 fields carry no range bound in `validate_chunk` (finiteness
  only): no scale for the D10 action or the P0 is fixed anywhere, so
  inventing a range would be inventing a metric.
- **T3 first observation does not count.** "Beacon advancing" needs
  two observations to establish; the first chunk after applying, with
  no previous beacon value, is `not_counted` by design (it waits for
  the next chunk). The server must persist the previous beacon value
  per candidate. All four evidence fields are required for a chunk to
  count — a missing field is missing evidence, not a pass.
