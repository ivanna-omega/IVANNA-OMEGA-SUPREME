"""Deterministic MuseLink analyzer — reference implementation (release 510).

Replaces the paid chat (Muse IA, blocked by billing) as the brain that
compiles sessions into profiles and decides promotions / rollbacks. This
module is the REFERENCE implementation that lives in the IVANNA-OMEGA-SUPREME
repo so the server (MuseLink app, AI Studio) can adopt it verbatim.

Pure Python, standard library only, no I/O, no network, no randomness.
Every rule is a pure, testable function; every threshold is a named,
documented constant taken from the specifications, never invented.

Specifications implemented:
  * ``refactorizacion-muselink-autonoma.md`` §2.3 (deterministic analyzer),
    §4 (A/B machine: the 5 original promotion gates).
  * ``evolucion-magistral-autonomia-completa.md`` §§S2, S5 (via B), S6, S8,
    S9 (server side), T6 (cumulative excursion cap + promotion brake),
    T7 (anti-poisoning, before aggregation).

Baseline fixed by the specs: spectral error RMS 10.46 dB (v1.1 profile),
promotion target <= 8.37 dB (0.8 x 10.46).

Public API (all pure functions):
  * S2  -- :func:`fit_comb_vs_response` (comb vs response discrimination)
  * S6  -- :func:`split_cut_boost`, :func:`precheck_headroom`
  * S8  -- :func:`evaluate_ab`
  * S5  -- :func:`emit_ladder`
  * S9  -- :func:`coherence_guard`
  * S10 -- :func:`check_residual_freeze` (residual > 4 dB freezes EQ
    steps in the region and marks EVO-003 as the next required step)
  * S15 -- :func:`check_d10_p0_covariates` (D10 action / P0 as A/B
    covariates; an out-of-tolerance move makes the A/B inconclusive)
  * T3  -- :func:`check_playing_evidence`, :func:`count_playing_chunks`
    (a chunk only counts toward the A/B when there is evidence that
    it is actually sounding)
  * T7  -- :func:`validate_chunk`, :func:`check_token_rate`,
    :func:`check_replay`, :func:`classify_session_outlier`,
    :func:`quarantine_update`, :func:`log_rejection`,
    :func:`rejection_summary`, :func:`gate_session`
  * T6  -- :func:`check_cumulative_excursion`,
    :func:`check_promotion_brake`, :func:`gate_promotion`
    (and the ``"t6"`` entry of :func:`compile_candidate`).
  * §2.3 -- :func:`regional_correction`, :func:`detect_resonances`,
    :func:`region_rms`, :func:`compile_candidate`
"""

import math

__all__ = [
    # Constants
    "ERB_CENTER_HZ",
    "REGIONS",
    "AB_REGIONS",
    "EQ_SLOTS",
    "EQ_CLAMP_DB",
    "APEX_CEILING_LINEAR",
    "APEX_CEILING_DBFS",
    "BASELINE_RMS_DB",
    "TARGET_RMS_DB",
    "AB_MIN_IMPROVEMENT",
    "AB_MAX_REGION_WORSENING_DB",
    "AB_COHERENCE_SLACK",
    "AB_LEVEL_MATCH_DB",
    "AB_LUFS_MATCH_DB",
    "AB_CREST_DROP_MAX_DB",
    "AB_APEX_PEG_TOLERANCE_DB",
    "RESONANCE_MIN_DB",
    "RESONANCE_NEIGHBOUR_DB",
    "CORRECTION_DEADBAND_DB",
    "COHERENCE_GUARD",
    "LADDER_STEP_DB",
    "COMB_AMPLITUDE_DB",
    "COMB_FIT_D_MIN_M",
    "COMB_FIT_D_MAX_M",
    "COMB_IMPROVEMENT_MIN",
    "COMB_STD_CONTRAST_MIN_DB",
    "AXIAL_MODES_HZ",
    "SPEED_OF_SOUND_M_S",
    # T7 — anti-poisoning (proposals; see README §T7)
    "CHUNK_ERROR_DB_MIN",
    "CHUNK_ERROR_DB_MAX",
    "CHUNK_MAGNITUDE_DB_MIN",
    "CHUNK_MAGNITUDE_DB_MAX",
    "CHUNK_COHERENCE_MIN",
    "CHUNK_COHERENCE_MAX",
    "CHUNK_LEVEL_DBFS_MIN",
    "CHUNK_LEVEL_DBFS_MAX",
    "CHUNK_RMS_DB_MAX",
    "CHUNK_ERB_STD_DB_MIN",
    "CHUNK_ERB_STD_DB_MAX",
    "CHUNK_CREST_DB_MIN",
    "CHUNK_CREST_DB_MAX",
    "CHUNK_LUFS_MIN_DB",
    "CHUNK_LUFS_MAX_DB",
    "DEFAULT_MAX_SESSIONS_PER_HOUR",
    "RATE_WINDOW_SECONDS",
    "OUTLIER_DEVIATION_DB",
    "OUTLIER_CONFIRM_DB",
    "OUTLIER_CONFIRM_SESSIONS",
    # T6 — cumulative excursion cap + promotion brake (proposals; see
    # README §T6)
    "CUMULATIVE_EXCURSION_CAP_DB",
    "MAX_PROMOTIONS_PER_DAY",
    # S10 — residual freeze (see README §S10)
    "S10_RESIDUAL_FREEZE_DB",
    # S15 — D10/P0 covariates (see README §S15)
    "D10_P0_TOLERANCE_DB",
    # §2.3 helpers
    "region_error",
    "region_rms",
    "detect_resonances",
    "regional_correction",
    # Rules
    "fit_comb_vs_response",
    "precheck_headroom",
    "split_cut_boost",
    "evaluate_ab",
    "emit_ladder",
    "coherence_guard",
    "compile_candidate",
    # S10 — residual freeze
    "check_residual_freeze",
    # S15 — D10/P0 covariates
    "check_d10_p0_covariates",
    # T3 — "sounding, not just installed" counting
    "check_playing_evidence",
    "count_playing_chunks",
    # T7 — anti-poisoning, before aggregation
    "validate_chunk",
    "check_token_rate",
    "check_replay",
    "classify_session_outlier",
    "quarantine_update",
    "log_rejection",
    "rejection_summary",
    "gate_session",
    # T6 — cumulative excursion cap + promotion brake
    "check_cumulative_excursion",
    "check_promotion_brake",
    "gate_promotion",
]

# ---------------------------------------------------------------------------
# Fixed data: the 24 ERB band centres (Hz), verified against
# SessionAggregator.erbCenterHz (60 Hz - 16 kHz).
# ---------------------------------------------------------------------------

ERB_CENTER_HZ = (
    82, 141, 209, 290, 385, 497, 630, 787, 973, 1193, 1453, 1761,
    2125, 2555, 3064, 3666, 4378, 5221, 6217, 7395, 8789, 10437,
    12387, 14693,
)
N_BANDS = 24

# Regions of §2.3 (1-based ERB indices, inclusive). "Subgraves (<60 Hz)" is
# outside the sensor's ERB scale and has no band.
REGIONS = {
    "graves": (1, 1),            # 60-120 Hz
    "medios_graves": (2, 6),     # 120-500 Hz
    "medios": (7, 9),            # 500-1000 Hz
    "transicion": (10, 12),      # 1-2 kHz
    "brillo": (13, 16),          # 2-4 kHz
    "presencia": (17, 20),       # 4-8 kHz
    "aire": (21, 24),            # 8-16 kHz
}

# S8(a) region groups: the spec names five groups. "graves/medios-graves"
# is one group (ERB 1-6); the transition bands (ERB 10-12) belong to no
# S8 group and are covered only by the global RMS gate.
AB_REGIONS = {
    "graves_medios_graves": (1, 6),
    "medios": (7, 9),
    "brillo": (13, 16),
    "presencia": (17, 20),
    "aire": (21, 24),
}

# Motor v1.0 EQ slots (§2.3) and the region each one serves.
EQ_SLOTS = ("low_60_120", "mid_500_1k", "high_2k_4k", "presence_4k_8k")
SLOT_REGION = {
    "low_60_120": "graves",
    "mid_500_1k": "medios",
    "high_2k_4k": "brillo",
    "presence_4k_8k": "presencia",
}

# ---------------------------------------------------------------------------
# Thresholds (all from the specifications; see README for the source of each)
# ---------------------------------------------------------------------------

# v1.0 schema clamp: each eq_bands_db slot in [-2, +2] dB (§2.3 rule 3).
EQ_CLAMP_DB = 2.0

# APEX limiter ceiling (EqStack.kt / PlaybackCaptureService.kt): hard clip
# at 0.9885531 linear == -0.1 dBFS. S6 pre-check: levelMaxDbfs + maxBoost
# must stay <= -0.1 dBFS.
APEX_CEILING_LINEAR = 0.9885531
APEX_CEILING_DBFS = -0.1

# A/B baseline and target (§4 / §6): baseline RMS 10.46 dB (v1.1 profile),
# promotion target <= 0.8 * 10.46 = 8.368 -> 8.37 dB.
BASELINE_RMS_DB = 10.46
TARGET_RMS_DB = 8.37

# §4 gate 1: ERB improvement >= 20% over the 3 base sessions.
AB_MIN_IMPROVEMENT = 0.20
# §4 gate 2: coherence may not fall more than 0.05 below base.
AB_COHERENCE_SLACK = 0.05
# §4 gate 3: a band at >= +6 dB is a resonance (see RESONANCE_* below).
# §4 gate 4: no chunk with levelMaxDbfs >= 0 dBFS (clip).
# S8(a): no region may worsen by more than 1 dB even if the global RMS
# improves.
AB_MAX_REGION_WORSENING_DB = 1.0
# S8(c): loudness matching. levelMeanDbfs within +/-3 dB (§4, "3 sesiones
# identicas"); lufs() within +/-1 dB when that telemetry exists.
AB_LEVEL_MATCH_DB = 3.0
AB_LUFS_MATCH_DB = 1.0
# S8(b): APEX working harder. A crest drop larger than this, or the
# candidate's levelMax pegged at the -0.1 dBFS ceiling while the base was
# not, means the limiter is doing more work than before.
AB_CREST_DROP_MAX_DB = 0.5
AB_APEX_PEG_TOLERANCE_DB = 0.05

# §2.3 resonance: error >= +6 dB AND >= 3 dB above both neighbours.
RESONANCE_MIN_DB = 6.0
RESONANCE_NEIGHBOUR_DB = 3.0

# §2.3 rule 2: |regional error| < 1.0 dB is noise; do not correct it.
CORRECTION_DEADBAND_DB = 1.0

# §2.3 coherence guard / S9: below this mean coherence the analyzer does
# not emit EQ for auto-application.
COHERENCE_GUARD = 0.20

# S5 via B: deterministic ladder, +2 dB per version rung.
LADDER_STEP_DB = 2.0
LADDER_MAX_RUNGS = 10  # safety cap; +7 dB needs 4 rungs, never near this.

# S2: axial room modes of the listening room (4.0 x 7.0 x 3.0 m),
# fundamentals in Hz; harmonics are generated from these.
AXIAL_MODES_HZ = (24.5, 42.9, 57.2)
SPEED_OF_SOUND_M_S = 343.0

# S2 model: error(f) = tilt * log2(f/1000) + modal_gain * modal(f)
#                    + COMB_AMPLITUDE_DB * cos(2*pi*f*2d/c)
# Only 3 fitted parameters (tilt, modal gain, d) for 24 points, per the
# spec, to avoid overfitting. The comb amplitude is FIXED at 3.0 dB: a
# cosine of fixed amplitude is the identifiable part of a comb with an
# unknown reflection coefficient; the fitted d and the residual are what
# get published, not an assumed wall distance. Documented modelling
# choice, not a measured value.
COMB_AMPLITUDE_DB = 3.0
# Grid search range for d: plausible wall distances in a 4.0 x 7.0 x 3.0 m
# room (0.3 m = mic almost against a wall, 3.5 m = half the room length).
COMB_FIT_D_MIN_M = 0.3
COMB_FIT_D_MAX_M = 3.5
COMB_FIT_D_STEP_M = 0.01
# S2 classification thresholds.
# The comb model must cut the smooth-only residual by at least this
# fraction, AND the intra-band dispersion (erbStdDb) must be at least this
# much higher where the comb predicts nulls than where it predicts peaks.
COMB_IMPROVEMENT_MIN = 0.25
COMB_STD_CONTRAST_MIN_DB = 1.0
# A band counts as "near a comb null" when cos(2*pi*f*2d/c) < -0.5 and
# "near a comb peak" when > +0.5.
COMB_NULL_COS = -0.5
COMB_PEAK_COS = 0.5

# ---------------------------------------------------------------------------
# T7 — anti-poisoning thresholds (evolucion-magistral-autonomia-completa.md
# T7). PROPOSAL, not measured data: the proposal leaves the thresholds to
# the implementer when Esse does not fix them. Each value below is a
# reasoned proposal from the known data (see README §T7); Esse confirms
# or adjusts them. They are named constants so the server can tune them
# from measured data without touching the rules.
# ---------------------------------------------------------------------------

# T7 rule 1: physical ranges for a chunk. A value outside these bounds
# cannot be a measurement of this system (digital full scale, coherence
# 0..1, room/speaker response of the known profile) and is rejected.
# Known data the proposals start from: v1.1 measured band errors span
# -12..+9.86 dB; magnitude is a digital level (<= 0 dBFS); 16-bit floor
# is -96 dBFS; coherence is 0..1 by definition.
CHUNK_ERROR_DB_MIN = -40.0     # per-band errorDb; proposal
CHUNK_ERROR_DB_MAX = 40.0      # per-band errorDb; proposal
CHUNK_MAGNITUDE_DB_MIN = -120.0  # per-band magnitudeDb, dBFS; proposal
CHUNK_MAGNITUDE_DB_MAX = 0.0     # digital full scale; not a proposal
CHUNK_COHERENCE_MIN = 0.0      # coherence, by definition
CHUNK_COHERENCE_MAX = 1.0      # coherence, by definition
CHUNK_LEVEL_DBFS_MIN = -120.0  # levelMean/levelMax, dBFS; proposal
CHUNK_LEVEL_DBFS_MAX = 0.0     # digital full scale; not a proposal
CHUNK_RMS_DB_MAX = 40.0        # spectralErrorRmsDb (<= max |error|); proposal
CHUNK_ERB_STD_DB_MIN = 0.0     # erbStdDb is a dispersion; by definition
CHUNK_ERB_STD_DB_MAX = 40.0    # erbStdDb; proposal
CHUNK_CREST_DB_MIN = 0.0       # crest = peak - RMS, never negative
CHUNK_CREST_DB_MAX = 60.0      # crest of real audio; proposal
CHUNK_LUFS_MIN_DB = -70.0      # integrated loudness floor; proposal
CHUNK_LUFS_MAX_DB = 0.0        # LUFS cannot exceed full scale

# T7 rule 2: per-token rate limit. A PARAMETER, not a buried constant:
# the server is the one that knows the token, so gate_session() takes
# max_sessions_per_hour and the server passes its own value. The default
# below is a proposal: an A/B needs 3 sessions per side and a phone
# flushes at most a few listening sessions per hour in real use, so 6/h
# is generous for one token and still blocks a replay flood.
DEFAULT_MAX_SESSIONS_PER_HOUR = 6
RATE_WINDOW_SECONDS = 3600

# T7 rule 3: per-band outlier hold. A session whose error in any band
# deviates from the aggregated profile by more than this does not enter
# aggregation until 2 further sessions confirm the same deviation (same
# band, same sign, more than OUTLIER_CONFIRM_DB). Proposal: 6 dB is well
# above the 1 dB correction deadband and above any plausible single-band
# drift of a real room change, while a genuine change (mic/speakers
# moved) reappears in the next sessions and confirms; the confirmation
# bar is half the hold bar so a real but weaker repeat still counts.
OUTLIER_DEVIATION_DB = 6.0
OUTLIER_CONFIRM_DB = 3.0
OUTLIER_CONFIRM_SESSIONS = 2


# ---------------------------------------------------------------------------
# T6 — cumulative excursion cap + promotion brake
# (evolucion-magistral-autonomia-completa.md T6). PROPOSALS, not measured
# data: the proposal leaves the cap and the brake to Esse ("solo él puede
# fijar ese límite"). Each value below is the proposal's own example
# ("p. ej. ±4 dB", "máximo una promoción por día" / "máximo 2
# aplicaciones automáticas por día" on the phone), taken as the explicit
# default until Esse confirms or changes it. They are named constants so
# the server can tune them without touching the rules — and so Esse can
# fix them with one number each.
# ---------------------------------------------------------------------------

# T6 rule (a): cumulative excursion per slot vs the FACTORY baseline.
# The factory baseline is no correction at all: 0.0 dB in every slot
# (the v1.0/v1.1 factory profile carries no EQ correction of its own).
# The per-version clamp (EQ_CLAMP_DB, +/-2 dB) does NOT limit the drift:
# successive +2 dB rungs can walk a slot to +6/+8 dB over several
# versions without any human ever listening. The cumulative value of a
# slot — the EQ the profile actually carries after the whole climb,
# i.e. a ladder rung's eq_bands_db — must stay within +/-4 dB.
# Exactly at the cap is allowed; beyond it the cycle STOPS and asks a
# human (see gate_promotion: halt_needs_human), it does not silently
# truncate the climb.
CUMULATIVE_EXCURSION_CAP_DB = 4.0

# T6 rule (a): at most one promotion per calendar day. The server
# counts how many promotions it already published today and passes the
# count to check_promotion_brake / gate_promotion; the brake is a
# WAIT (publish again tomorrow), not a halt — unlike the excursion
# breach, which stops the cycle until a human decides.
MAX_PROMOTIONS_PER_DAY = 1


# ---------------------------------------------------------------------------
# S10 — residual > 4 dB: diagnose before climbing further
# (evolucion-magistral-autonomia-completa.md S10).
#
# The trigger is the spec's own number: a regional residual above 4 dB
# ("supera 4 dB", strictly greater; exactly 4.0 dB does not trigger).
# The release conditions are the spec's own words — "hasta que la
# coherencia suba o la medición confirme el déficit" — with one
# modelling choice made explicit: "coherence has risen" is read as
# mean_coherence >= COHERENCE_GUARD (0.20), the same guard S9 already
# uses for "this table can be trusted". That threshold reuse is a
# choice, not a measured value; the spec fixes no number for it. The
# other release is explicit evidence: ``deficit_confirmed=True``,
# i.e. the EVO-003 physical measurement confirmed the deficit.
# ---------------------------------------------------------------------------

S10_RESIDUAL_FREEZE_DB = 4.0


# ---------------------------------------------------------------------------
# S15 — P0/D10 as A/B covariates, not decoration
# (evolucion-magistral-autonomia-completa.md S15).
#
# Tolerance from the spec: if the candidate moves the D10 action or
# the P0 outside +/-0.5 dB, the A/B is "no concluyente" even if the
# RMS improves. The spec itself says the tolerance is "a fijar con la
# primera medición", so 0.5 dB is the spec's value taken as the
# explicit default until the first measurement confirms or changes it
# — a named constant, not a measured fact.
# ---------------------------------------------------------------------------

D10_P0_TOLERANCE_DB = 0.5


# ---------------------------------------------------------------------------
# Small numeric helpers (stdlib only)
# ---------------------------------------------------------------------------

def _mean(values):
    values = list(values)
    return sum(values) / len(values) if values else 0.0


def _rms(values):
    values = list(values)
    return math.sqrt(sum(v * v for v in values) / len(values)) if values else 0.0


def _clamp(value, low, high):
    return max(low, min(high, value))


def _solve_2x2(a11, a12, a22, b1, b2):
    """Solve [[a11, a12], [a12, a22]] x = [b1, b2]; None if singular."""
    det = a11 * a22 - a12 * a12
    if abs(det) < 1e-12:
        return None
    return ((b1 * a22 - b2 * a12) / det, (a11 * b2 - a12 * b1) / det)


# ---------------------------------------------------------------------------
# §2.3 — regional aggregation, resonance detection, regional correction
# ---------------------------------------------------------------------------

def region_error(error_mean_db, region):
    """Mean errorMeanDb over the bands of a region (1-based indices).

    ``error_mean_db`` is the 24-band compiled table; ``region`` is a
    (first, last) inclusive 1-based band range from REGIONS / AB_REGIONS.
    """
    first, last = region
    band_values = error_mean_db[first - 1:last]
    return _mean(band_values)


def region_rms(error_mean_db, region):
    """RMS of errorMeanDb over the bands of a region (S8 gate (a))."""
    first, last = region
    return _rms(error_mean_db[first - 1:last])


def detect_resonances(error_mean_db):
    """Bands that are resonances (§2.3): error >= +6 dB and >= 3 dB above
    both neighbours. Returns a list of 1-based band indices."""
    found = []
    for i, err in enumerate(error_mean_db):
        if err < RESONANCE_MIN_DB:
            continue
        left = error_mean_db[i - 1] if i > 0 else float("-inf")
        right = error_mean_db[i + 1] if i < len(error_mean_db) - 1 else float("-inf")
        if err - left >= RESONANCE_NEIGHBOUR_DB and err - right >= RESONANCE_NEIGHBOUR_DB:
            found.append(i + 1)
    return found


def regional_correction(error_mean_db):
    """§2.3 rules 1-5: regional error -> clamped slot correction.

    Returns a dict with:
      ``regional_error_db``  per-region mean error,
      ``proposed_db``        per-region correction = -error (0 if inside
                             the 1 dB deadband),
      ``applied_db``         per motor slot, clamped to +/-2 dB (regions
                             without a slot get no entry),
      ``residual_db``        per-region unapplied remainder (proposed minus
                             applied; 0 for slotted regions inside clamp),
      ``unslotted_db``       proposed correction for regions without a
                             motor slot (documented, never applied),
      ``trim_db``            -(sum of applied slots), net gain 0.0 dB.
    """
    regional_error_db = {}
    proposed_db = {}
    applied_db = {}
    residual_db = {}
    unslotted_db = {}
    slot_for_region = {region: slot for slot, region in SLOT_REGION.items()}
    for region_name, band_range in REGIONS.items():
        err = region_error(error_mean_db, band_range)
        regional_error_db[region_name] = err
        proposed = -err if abs(err) >= CORRECTION_DEADBAND_DB else 0.0
        proposed_db[region_name] = proposed
        slot = slot_for_region.get(region_name)
        if slot is None:
            unslotted_db[region_name] = proposed
            residual_db[region_name] = proposed
        else:
            applied = _clamp(proposed, -EQ_CLAMP_DB, EQ_CLAMP_DB)
            applied_db[slot] = applied
            residual_db[region_name] = proposed - applied
    trim_db = -sum(applied_db.values())
    return {
        "regional_error_db": regional_error_db,
        "proposed_db": proposed_db,
        "applied_db": applied_db,
        "residual_db": residual_db,
        "unslotted_db": unslotted_db,
        "trim_db": trim_db,
    }


# ---------------------------------------------------------------------------
# S10 — residual > 4 dB: diagnose before climbing further (Rol 5 P7)
# ---------------------------------------------------------------------------

def check_residual_freeze(regional, mean_coherence=None,
                          deficit_confirmed=False):
    """S10: a regional residual above 4 dB smells of a measurement
    problem (mic position, room comb), not of a real +9 dB deficit.
    Closing it with blind ladder steps assumes linearity with no
    evidence, so the rule FREEZES the EQ steps in that region and
    marks EVO-003 (physical measurement: distance to walls / test by
    position) as the next required step.

    ``regional`` is the dict returned by :func:`regional_correction`
    (only its ``residual_db`` is read). ``mean_coherence`` is the
    compiled profile's mean coherence when the server knows it (None
    when it does not). ``deficit_confirmed`` is True only when the
    EVO-003 physical measurement already confirmed the deficit.

    Freeze rule (spec): a region freezes when |residual| >
    S10_RESIDUAL_FREEZE_DB (4.0 dB, strictly greater). Regions without
    a motor slot (transición, aire, ...) have no EQ step to freeze,
    but they still trigger the diagnostic: their residual is
    uncorrected error, and it is reported in ``triggered_regions``.
    ``frozen_slots`` lists only the motor slots that lose their steps.

    Release rule (spec: "hasta que la coherencia suba o la medición
    confirme el déficit"): the freeze is released when
    ``deficit_confirmed`` is True, or when ``mean_coherence`` >=
    COHERENCE_GUARD (0.20 — the S9 guard, reused here as "coherence
    has risen"; see the constants block). A released freeze still
    reports the regions that triggered, in ``triggered_regions``, so
    the diagnostic signal is not lost; ``frozen_regions`` is empty.

    ``evo003_required`` is True when a region triggered and the freeze
    was NOT released: EVO-003 is the next required step. With
    coherence below the guard the residual cannot be trusted as a room
    deficit (S1: coherence 0% is instrumentation — the Organism path
    does not feed the reference), so
    ``evo003_pending_reference_fix`` is True in exactly that case:
    the EVO-003 firing itself stays pending the S1 reference fix,
    because a position probe would measure the instrument, not the
    room (see README §S10 and §S2/S16).

    Returns ``{"triggered_regions": [...], "frozen_regions": [...],
    "frozen_slots": [...], "evo003_required": bool,
    "evo003_pending_reference_fix": bool, "released": bool,
    "release_reason": str, "residual_db": {...}, "reason": str}``.
    Region lists are sorted by the REGIONS order; slot lists are
    sorted alphabetically.
    """
    residual_db = dict(regional["residual_db"])
    triggered = [name for name in REGIONS
                 if abs(residual_db.get(name, 0.0)) > S10_RESIDUAL_FREEZE_DB + 1e-9]

    released = False
    release_reason = ""
    if not triggered:
        release_reason = "no region exceeds the %.1f dB residual freeze threshold" % S10_RESIDUAL_FREEZE_DB
    elif deficit_confirmed:
        released = True
        release_reason = ("EVO-003 physical measurement already confirmed "
                          "the deficit; freeze released")
    elif mean_coherence is not None and mean_coherence >= COHERENCE_GUARD:
        released = True
        release_reason = ("mean coherence %.3f >= %.2f (S9 guard): the "
                          "table can be trusted, the residual is a real "
                          "deficit, freeze released"
                          % (mean_coherence, COHERENCE_GUARD))

    frozen_regions = [] if released else list(triggered)
    slot_for_region = {region: slot for slot, region in SLOT_REGION.items()}
    frozen_slots = sorted({slot_for_region[name] for name in frozen_regions
                           if name in slot_for_region})
    evo003_required = bool(triggered) and not released
    pending_reference_fix = bool(
        evo003_required and mean_coherence is not None
        and mean_coherence < COHERENCE_GUARD)

    if not triggered:
        reason = ("S10: no freeze — largest |residual| %.2f dB in %s "
                  "(threshold > %.1f dB)"
                  % (max([abs(v) for v in residual_db.values()] or [0.0]),
                     max(residual_db, key=lambda k: abs(residual_db[k]),
                         default="—"),
                     S10_RESIDUAL_FREEZE_DB))
    elif released:
        reason = ("S10: residual > %.1f dB in %s, but the freeze is "
                  "RELEASED — %s"
                  % (S10_RESIDUAL_FREEZE_DB, triggered, release_reason))
    else:
        reason = ("S10: residual > %.1f dB in %s — EQ steps FROZEN in "
                  "those regions (slots %s); EVO-003 is the next required "
                  "step%s"
                  % (S10_RESIDUAL_FREEZE_DB, frozen_regions,
                     frozen_slots or "none (unslotted regions only)",
                     "; firing pending the S1 reference fix (mean "
                     "coherence %.3f < %.2f, residual not trustworthy as "
                     "a room deficit)" % (mean_coherence, COHERENCE_GUARD)
                     if pending_reference_fix else ""))
    return {
        "triggered_regions": triggered,
        "frozen_regions": frozen_regions,
        "frozen_slots": frozen_slots,
        "evo003_required": evo003_required,
        "evo003_pending_reference_fix": pending_reference_fix,
        "released": released,
        "release_reason": release_reason,
        "residual_db": residual_db,
        "reason": reason,
    }


# ---------------------------------------------------------------------------
# S2 — comb vs response discrimination (Rol 2 P2)
# ---------------------------------------------------------------------------

def _modal_basis(freq_hz):
    """Fixed modal shape: Gaussian bumps (0.2 octave sigma, in log2) at
    each axial mode fundamental and its harmonics up to 16 kHz, weighted
    1/k so higher harmonics contribute less. One fitted coefficient
    (modal gain) scales the whole shape."""
    log_f = math.log2(freq_hz)
    total = 0.0
    for fundamental in AXIAL_MODES_HZ:
        k = 1
        while fundamental * k <= 16000.0:
            fm = fundamental * k
            total += (1.0 / k) * math.exp(
                -0.5 * ((log_f - math.log2(fm)) / 0.2) ** 2
            )
            k += 1
    return total


def _fit_smooth(error_mean_db):
    """Least squares of error(f) = tilt*log2(f/1000) + gain*modal(f).
    Returns (tilt, gain, residual_rms)."""
    xs1 = [math.log2(f / 1000.0) for f in ERB_CENTER_HZ]
    xs2 = [_modal_basis(f) for f in ERB_CENTER_HZ]
    a11 = sum(x * x for x in xs1)
    a12 = sum(x * y for x, y in zip(xs1, xs2))
    a22 = sum(y * y for y in xs2)
    b1 = sum(x * y for x, y in zip(xs1, error_mean_db))
    b2 = sum(x * y for x, y in zip(xs2, error_mean_db))
    sol = _solve_2x2(a11, a12, a22, b1, b2)
    if sol is None:
        return 0.0, 0.0, _rms(error_mean_db)
    tilt, gain = sol
    resid = [y - (tilt * x1 + gain * x2)
             for x1, x2, y in zip(xs1, xs2, error_mean_db)]
    return tilt, gain, _rms(resid)


def _fit_with_comb(error_mean_db, distance_m):
    """Least squares with the comb term added at a fixed distance d:
    error(f) = tilt*log2(f/1000) + gain*modal(f)
               + COMB_AMPLITUDE_DB * cos(2*pi*f*2d/c).
    Only tilt and gain are fitted; d is the grid parameter.
    Returns (tilt, gain, residual_rms)."""
    tau = 2.0 * distance_m / SPEED_OF_SOUND_M_S
    comb = [COMB_AMPLITUDE_DB * math.cos(2.0 * math.pi * f * tau)
            for f in ERB_CENTER_HZ]
    adjusted = [y - c for y, c in zip(error_mean_db, comb)]
    tilt, gain, resid_rms = _fit_smooth(adjusted)
    return tilt, gain, resid_rms


def fit_comb_vs_response(error_mean_db, erb_std_db=None):
    """S2: decide whether a measured deficit is a comb (room reflection)
    or a response (target curve / Genezi / mic position).

    Fits error(f) with the 3-parameter model (tilt, modal gain, d) by
    least squares over the 24 ERB bands, grid-searching d, and compares
    against the smooth-only fit. The fitted ``d`` and its residual are
    published; no wall distance is assumed.

    ``erb_std_db`` is the per-band dispersion of the error (SessionChunk
    field). PENDING DATA: the phone computes it, but OrganismPayload does
    NOT send it to the server today, so the compiled server profile may
    not carry it. When it is missing the verdict is ``indeterminate``
    with reason ``erb_std_db_missing`` -- the rule refuses to guess.

    Returns a dict with ``verdict`` ("comb" | "response" |
    "inconclusive" | "indeterminate"), ``distance_m`` (fitted d, or None),
    ``residual_rms_db``, ``smooth_residual_rms_db``,
    ``improvement`` (fractional residual reduction of the comb fit),
    ``std_contrast_db`` (mean erbStdDb at predicted nulls minus mean at
    predicted peaks; None when erb_std_db is missing), and ``reason``.
    """
    if len(error_mean_db) != N_BANDS:
        raise ValueError("error_mean_db must have exactly 24 bands")
    if erb_std_db is not None and len(erb_std_db) != N_BANDS:
        raise ValueError("erb_std_db must have exactly 24 bands")

    _, _, smooth_rms = _fit_smooth(error_mean_db)
    best = None
    d = COMB_FIT_D_MIN_M
    while d <= COMB_FIT_D_MAX_M + 1e-9:
        tilt, gain, resid_rms = _fit_with_comb(error_mean_db, d)
        if best is None or resid_rms < best[2]:
            best = (tilt, gain, resid_rms, d)
        d = round(d + COMB_FIT_D_STEP_M, 10)
    tilt, gain, comb_rms, best_d = best
    improvement = (smooth_rms - comb_rms) / smooth_rms if smooth_rms > 0 else 0.0

    result = {
        "verdict": "inconclusive",
        "distance_m": best_d,
        "tilt_db_per_octave": tilt,
        "modal_gain_db": gain,
        "residual_rms_db": comb_rms,
        "smooth_residual_rms_db": smooth_rms,
        "improvement": improvement,
        "std_contrast_db": None,
        "reason": "",
    }

    if erb_std_db is None:
        result["verdict"] = "indeterminate"
        result["reason"] = (
            "erb_std_db_missing: per-band error dispersion is not in the "
            "compiled server profile (OrganismPayload does not send "
            "erbStdDb); S2 cannot distinguish comb from response without it"
        )
        return result

    tau = 2.0 * best_d / SPEED_OF_SOUND_M_S
    cos_vals = [math.cos(2.0 * math.pi * f * tau) for f in ERB_CENTER_HZ]
    null_std = [s for s, c in zip(erb_std_db, cos_vals) if c < COMB_NULL_COS]
    peak_std = [s for s, c in zip(erb_std_db, cos_vals) if c > COMB_PEAK_COS]
    if null_std and peak_std:
        contrast = _mean(null_std) - _mean(peak_std)
    else:
        contrast = 0.0
    result["std_contrast_db"] = contrast

    if improvement >= COMB_IMPROVEMENT_MIN and contrast >= COMB_STD_CONTRAST_MIN_DB:
        result["verdict"] = "comb"
        result["reason"] = (
            "comb fit cuts the smooth-only residual by %.0f%% and erbStdDb "
            "is %.1f dB higher where the comb predicts nulls than where it "
            "predicts peaks" % (100.0 * improvement, contrast)
        )
    elif improvement < COMB_IMPROVEMENT_MIN and _mean(erb_std_db) < 1.0:
        result["verdict"] = "response"
        result["reason"] = (
            "comb fit does not improve the smooth model (%.0f%%) and "
            "erbStdDb is low throughout (mean %.2f dB): smooth deficit, "
            "not a comb" % (100.0 * improvement, _mean(erb_std_db))
        )
    else:
        result["reason"] = (
            "evidence is mixed (comb improvement %.0f%%, std contrast "
            "%.1f dB); neither comb nor response is established"
            % (100.0 * improvement, contrast)
        )
    return result


# ---------------------------------------------------------------------------
# S6 — cuts first, boosts after (Rol 2 P5)
# ---------------------------------------------------------------------------

def precheck_headroom(level_max_dbfs, eq_slots_db, exciter_amount=0.0):
    """S6 deterministic pre-check, run before emitting ANY candidate:
    ``levelMaxDbfs + maxBoost <= -0.1 dBFS`` (APEX ceiling).

    ``eq_slots_db`` maps slot name -> applied dB (any subset of EQ_SLOTS).
    ``maxBoost`` is the largest positive slot value (0 if cut-only).

    If the check fails, the boost is scaled down deterministically until
    it passes; if the live level is already at/above the ceiling even
    with zero boost, the exciter is also zeroed and emission is BLOCKED.

    Returns a dict with ``ok`` (bool), ``adjusted_slots_db``,
    ``adjusted_exciter_amount``, ``max_boost_db`` (after adjustment) and
    ``notes`` (list of strings describing every adjustment made).
    """
    notes = []
    adjusted = dict(eq_slots_db)
    max_boost = max([0.0] + [v for v in adjusted.values() if v > 0.0])
    allowed_boost = APEX_CEILING_DBFS - level_max_dbfs
    exciter = exciter_amount
    # EPS: float guard at the exact ceiling boundary (-1.0 + 0.9 is
    # -0.09999999999999998 in binary, not -0.1).
    eps = 1e-9

    if level_max_dbfs + max_boost <= APEX_CEILING_DBFS + eps:
        return {
            "ok": True,
            "adjusted_slots_db": adjusted,
            "adjusted_exciter_amount": exciter,
            "max_boost_db": max_boost,
            "notes": notes,
        }

    if allowed_boost > 0.0 and max_boost > 0.0:
        scale = allowed_boost / max_boost
        for slot, value in adjusted.items():
            if value > 0.0:
                adjusted[slot] = value * scale
        notes.append(
            "boost scaled by %.3f: max boost %.2f -> %.2f dB so that "
            "levelMaxDbfs (%.2f) + maxBoost <= -0.1 dBFS"
            % (scale, max_boost, allowed_boost, level_max_dbfs)
        )
        max_boost = allowed_boost
    elif max_boost > 0.0:
        for slot, value in adjusted.items():
            if value > 0.0:
                adjusted[slot] = 0.0
        notes.append(
            "all boost removed: levelMaxDbfs %.2f dBFS leaves no headroom "
            "below the -0.1 dBFS APEX ceiling" % level_max_dbfs
        )
        max_boost = 0.0

    if level_max_dbfs + max_boost > APEX_CEILING_DBFS + eps:
        # Live level already at/above the ceiling: even a cut-only
        # candidate cannot be emitted safely. Zero the exciter too and
        # block emission rather than letting the limiter eat the signal.
        if exciter != 0.0:
            notes.append(
                "exciter_amount %.2f -> 0.0: no headroom left" % exciter
            )
            exciter = 0.0
        return {
            "ok": False,
            "adjusted_slots_db": adjusted,
            "adjusted_exciter_amount": exciter,
            "max_boost_db": max_boost,
            "notes": notes + [
                "BLOCKED: levelMaxDbfs %.2f dBFS is already at/above the "
                "-0.1 dBFS APEX ceiling; candidate not emitted"
                % level_max_dbfs
            ],
        }

    return {
        "ok": True,
        "adjusted_slots_db": adjusted,
        "adjusted_exciter_amount": exciter,
        "max_boost_db": max_boost,
        "notes": notes,
    }


def split_cut_boost(proposed_db, level_max_dbfs, exciter_amount=0.3,
                    v1_1a_promoted=False, room_corr_cut_db=None,
                    room_corr_available=False):
    """S6: split a proposed correction into v1.1a (cut-only) and v1.1b
    (boost), in that order.

    ``proposed_db`` maps motor slot -> proposed correction in dB (the
    unclamped regional correction, e.g. v1.1: low -3, mid +4, high +9,
    presence +9). ``room_corr_cut_db`` is the optional ERB-3 cut via
    ``roomCorrDb`` (S3); it is cut-only and belongs to v1.1a, but only
    when ``room_corr_available`` is True (S3 done). Otherwise v1.1a uses
    only ``low_60_120`` and the ERB-3 cut is reported as pending S3.

    v1.1a: negative corrections only, clamped to -2 dB; trim compensates
    only the cuts (trim = -(sum of slots), net gain 0).
    v1.1b: positive corrections only, clamped to +2 dB; emitted ONLY if
    ``v1_1a_promoted`` is True (it must have promoted and freed headroom
    first). Otherwise v1.1b is None with the reason recorded.

    Each half goes through :func:`precheck_headroom` before emission.

    Returns ``{"v1_1a": <candidate dict or None>, "v1_1b": ...,
    "notes": [...]}``. A candidate dict carries ``version`` ("v1.1a" /
    "v1.1b"), ``eq_bands_db``, ``room_corr_db`` (v1.1a only, else None),
    ``trim_db``, ``exciter_amount`` and ``precheck``.
    """
    notes = []
    cuts = {}
    boosts = {}
    for slot, value in proposed_db.items():
        if slot not in EQ_SLOTS:
            continue
        if value < 0.0:
            cuts[slot] = _clamp(value, -EQ_CLAMP_DB, 0.0)
        elif value > 0.0:
            boosts[slot] = _clamp(value, 0.0, EQ_CLAMP_DB)

    # --- v1.1a: cut-only ---
    room_corr = None
    if room_corr_cut_db is not None:
        if room_corr_available:
            room_corr = _clamp(room_corr_cut_db, -EQ_CLAMP_DB, 0.0)
        else:
            notes.append(
                "ERB-3 roomCorrDb cut %.2f dB NOT applied in v1.1a: S3 "
                "(roomCorrDb JNI slot) is not done; v1.1a uses only "
                "low_60_120" % room_corr_cut_db
            )
    a_slots = dict(cuts)
    a_trim = -sum(a_slots.values())
    a_pre = precheck_headroom(level_max_dbfs, a_slots, exciter_amount)
    a_trim = -sum(a_pre["adjusted_slots_db"].values())
    v1_1a = None
    if not a_slots and room_corr is None:
        notes.append(
            "v1.1a empty: no cut proposed (nothing to cut, or the S9 "
            "coherence guard blocked every cut region)"
        )
    elif a_pre["ok"]:
        v1_1a = {
            "version": "v1.1a",
            "eq_bands_db": a_pre["adjusted_slots_db"],
            "room_corr_db": room_corr,
            "trim_db": a_trim,
            "exciter_amount": a_pre["adjusted_exciter_amount"],
            "precheck": a_pre,
        }
    else:
        notes.append("v1.1a BLOCKED by headroom pre-check: " + "; ".join(a_pre["notes"]))

    # --- v1.1b: boost, gated on v1.1a having promoted ---
    v1_1b = None
    if not v1_1a_promoted:
        notes.append(
            "v1.1b NOT emitted: v1.1a has not promoted yet (boost waits "
            "for the cut-only candidate to win its A/B and free headroom)"
        )
    elif boosts:
        b_pre = precheck_headroom(level_max_dbfs, boosts, exciter_amount)
        b_trim = -sum(b_pre["adjusted_slots_db"].values())
        if b_pre["ok"]:
            v1_1b = {
                "version": "v1.1b",
                "eq_bands_db": b_pre["adjusted_slots_db"],
                "room_corr_db": None,
                "trim_db": b_trim,
                "exciter_amount": b_pre["adjusted_exciter_amount"],
                "precheck": b_pre,
            }
        else:
            notes.append("v1.1b BLOCKED by headroom pre-check: " + "; ".join(b_pre["notes"]))
    else:
        notes.append("v1.1b empty: no positive correction proposed")

    return {"v1_1a": v1_1a, "v1_1b": v1_1b, "notes": notes}


# ---------------------------------------------------------------------------
# S8 — hardened A/B gates (Rol 2 P7), on top of the 5 gates of §4
# ---------------------------------------------------------------------------

def _session_mean(sessions, field):
    return _mean([s[field] for s in sessions if field in s and s[field] is not None])


def _session_mean_or_none(sessions, field):
    """Mean of a field across sessions, or None when no session carries
    it. Unlike ``_session_mean(...) or None``, a genuine mean of 0.0
    stays 0.0 instead of collapsing to None (a D10 action of 0.0 is a
    real value, not an absence)."""
    values = [s[field] for s in sessions if field in s and s[field] is not None]
    return _mean(values) if values else None


def _aggregate_sessions(sessions):
    """Aggregate 3 sessions (base or candidate) into one summary.

    ``error_mean_db`` is averaged per band across sessions first, then
    region RMS is computed on the mean table. ``level_max_dbfs`` takes
    the worst (max) chunk level across sessions; crest takes the worst
    (min) crest; ``level_mean_dbfs`` and ``lufs`` are means.
    """
    n = len(sessions)
    mean_error = [
        _mean([s["error_mean_db"][i] for s in sessions])
        for i in range(N_BANDS)
    ]
    return {
        "n_sessions": n,
        "spectral_error_rms_db": _mean([s["spectral_error_rms_db"] for s in sessions]),
        "mean_error_db": mean_error,
        "mean_coherence": _mean([s["mean_coherence"] for s in sessions]),
        "level_max_dbfs": max(s["level_max_dbfs"] for s in sessions),
        "level_mean_dbfs": _mean([s["level_mean_dbfs"] for s in sessions]),
        "crest_db": _session_mean(sessions, "crest_db") or None,
        "crest_min_db": min([s["crest_db"] for s in sessions if s.get("crest_db") is not None], default=None),
        "lufs": _session_mean(sessions, "lufs") or None,
        # S15 covariates (None when the telemetry is absent: the A/B
        # must not require what the phone does not send yet).
        "d10_action": _session_mean_or_none(sessions, "d10_action"),
        "p0_db": _session_mean_or_none(sessions, "p0_db"),
        "trim_db_effective": _session_mean_or_none(sessions, "trim_db_effective"),
    }


# ---------------------------------------------------------------------------
# S15 — P0/D10 as A/B covariates (Rol 2 P9)
# ---------------------------------------------------------------------------

def check_d10_p0_covariates(base_sessions, candidate_sessions,
                            tolerance_db=D10_P0_TOLERANCE_DB):
    """S15: the A/B must not mistake "the field changed" for "the EQ
    improved". P0 measures -6.065/-6.274 dB (ALL7 -14.18 dB) without
    trim; the v1.1 ``trim_db = -4.0`` is nominal from the file, not
    measured loudness; the D10 arbiter is legacy as gain (telemetry
    only) and the LTI D10 kernel is still engaged. So the D10 action
    and the P0 ride along as covariates of the A/B.

    Each session may carry ``d10_action``, ``p0_db`` and
    ``trim_db_effective`` (all optional, retrocompatible: sessions
    without them are not rejected — see :func:`validate_chunk`).

    Rule (spec): if the candidate moves the D10 action or the P0
    outside +/-``tolerance_db`` (0.5 dB), the A/B is "no concluyente"
    even if the RMS improves. ``trim_db_effective`` is REPORTED (its
    delta is in the result) but does NOT gate: the spec fixes no
    tolerance for the trim, and inventing one would be inventing a
    metric. Do not touch the arbiter or the kernel: this is telemetry
    + a rule, nothing else.

    The comparison is on the per-side means of the covariate fields
    only, so the function also works on session dicts that carry just
    the covariates (it does not need the full A/B session shape).

    Returns ``{"status": "ok" | "inconclusive" | "unchecked",
    "d10_action_delta_db": float | None,
    "p0_db_delta_db": float | None,
    "trim_db_effective_delta_db": float | None, "tolerance_db": float,
    "reason": str}``. ``unchecked`` means the telemetry is absent on
    at least one side (or on both): the covariate cannot be checked,
    so it does not block anything — the A/B proceeds without it and
    the skip is recorded in the reason. A genuine mean of 0.0 is a
    value, not an absence (see ``_session_mean_or_none``).
    """
    def _delta(field):
        b = _session_mean_or_none(base_sessions, field)
        c = _session_mean_or_none(candidate_sessions, field)
        if b is None or c is None:
            return None
        return c - b

    d10_delta = _delta("d10_action")
    p0_delta = _delta("p0_db")
    trim_delta = _delta("trim_db_effective")

    missing = [label for label, delta in
               (("d10_action", d10_delta), ("p0_db", p0_delta))
               if delta is None]
    if missing:
        return {
            "status": "unchecked",
            "d10_action_delta_db": d10_delta,
            "p0_db_delta_db": p0_delta,
            "trim_db_effective_delta_db": trim_delta,
            "tolerance_db": tolerance_db,
            "reason": ("S15 covariate check SKIPPED: %s telemetry absent "
                       "on at least one side (SessionChunk carries no "
                       "D10/P0 fields yet); the A/B proceeds without this "
                       "covariate" % ", ".join(missing)),
        }

    out = []
    if abs(d10_delta) > tolerance_db + 1e-9:
        out.append("D10 action moved %+.2f dB (tolerance +/-%.1f dB)"
                   % (d10_delta, tolerance_db))
    if abs(p0_delta) > tolerance_db + 1e-9:
        out.append("P0 moved %+.2f dB (tolerance +/-%.1f dB)"
                   % (p0_delta, tolerance_db))
    if out:
        return {
            "status": "inconclusive",
            "d10_action_delta_db": d10_delta,
            "p0_db_delta_db": p0_delta,
            "trim_db_effective_delta_db": trim_delta,
            "tolerance_db": tolerance_db,
            "reason": ("S15: " + "; ".join(out) + " — the candidate moved "
                       "something other than the EQ; the A/B is NOT "
                       "CONCLUSIVE even if the RMS improved"),
        }
    return {
        "status": "ok",
        "d10_action_delta_db": d10_delta,
        "p0_db_delta_db": p0_delta,
        "trim_db_effective_delta_db": trim_delta,
        "tolerance_db": tolerance_db,
        "reason": ("S15: D10 action %+.2f dB, P0 %+.2f dB (tolerance "
                   "+/-%.1f dB); trim_db_effective %s — covariates "
                   "inside tolerance"
                   % (d10_delta, p0_delta, tolerance_db,
                      ("%+.2f dB" % trim_delta)
                      if trim_delta is not None else "absent")),
    }


def evaluate_ab(base_sessions, candidate_sessions):
    """S8: evaluate a candidate over 3 sessions vs 3 base sessions.

    Each session is a dict with:
      ``spectral_error_rms_db`` (float), ``error_mean_db`` (24 floats),
      ``mean_coherence`` (float), ``level_max_dbfs`` (float, worst chunk),
      ``level_mean_dbfs`` (float); optional ``crest_db`` (float) and
      ``lufs`` (float, when that telemetry exists); optional S15
      covariates ``d10_action``, ``p0_db`` and ``trim_db_effective``
      (floats, when that telemetry exists — retrocompatible: absent
      means the S15 gate is skipped, not failed).

    Gates (all must pass for promotion; any failure -> rollback):
      §4-1  ERB improvement >= 20%: (rms_base - rms_cand)/rms_base.
      §4-2  Coherence: mean_coherence_cand >= base - 0.05. (With a 0%
            base this does not discriminate; kept so it does when S1/S2
            move coherence off 0.)
      §4-3  No new resonance: no band >= +6 dB in the candidate that was
            not already >= +6 dB in the base.
      §4-4  No clip: no session with levelMaxDbfs >= 0 dBFS.
      S8(a) Region RMS: no region worsens by > 1 dB (graves/medios-
            graves, medios, brillo, presencia, aire).
      S8(b) APEX not working harder: crest must not fall > 0.5 dB, and
            the candidate's levelMax must not sit pegged at -0.1 dBFS
            while the base did not. (Crest part skipped, noted, when
            crest telemetry is absent.)
      S8(c) Loudness matching: |levelMean_cand - levelMean_base| <= 3 dB;
            when lufs exists on both sides, |lufs_cand - lufs_base|
            <= 1 dB.
      S15  D10/P0 covariates: when the telemetry exists on both sides,
            the candidate must not move the D10 action or the P0 by
            more than +/-0.5 dB. A move outside tolerance makes the
            verdict "inconclusive" (NOT a rollback: the candidate did
            not fail on its merits, the comparison itself is
            invalidated — something other than the EQ changed). When
            the telemetry is absent the gate is skipped + noted and
            the verdict is unaffected. A candidate that fails any
            §4/S8 gate is still a "rollback" even if it also moved
            D10/P0: failing on the merits outranks inconclusive.

    Returns ``{"verdict": "promote" | "rollback" | "inconclusive",
    "gates": {...}, "base": <summary>, "candidate": <summary>}``. Each
    gate entry is ``{"passed": bool, "detail": str}``.
    """
    base = _aggregate_sessions(base_sessions)
    cand = _aggregate_sessions(candidate_sessions)
    gates = {}

    # §4-1: ERB improvement >= 20%.
    improvement = ((base["spectral_error_rms_db"] - cand["spectral_error_rms_db"])
                   / base["spectral_error_rms_db"]) if base["spectral_error_rms_db"] > 0 else 0.0
    gates["erb_improvement"] = {
        "passed": improvement >= AB_MIN_IMPROVEMENT,
        "detail": "RMS %.2f -> %.2f dB (improvement %.1f%%, need >= 20%%; "
                  "target <= %.2f dB from baseline %.2f dB)"
                  % (base["spectral_error_rms_db"], cand["spectral_error_rms_db"],
                     100.0 * improvement, TARGET_RMS_DB, BASELINE_RMS_DB),
    }

    # §4-2: coherence does not worsen.
    coh_ok = cand["mean_coherence"] >= base["mean_coherence"] - AB_COHERENCE_SLACK
    gates["coherence"] = {
        "passed": coh_ok,
        "detail": "mean coherence %.3f -> %.3f (allowed drop <= %.2f)"
                  % (base["mean_coherence"], cand["mean_coherence"], AB_COHERENCE_SLACK),
    }

    # §4-3: no new resonance.
    base_res = set(detect_resonances(base["mean_error_db"]))
    cand_res = set(detect_resonances(cand["mean_error_db"]))
    new_res = sorted(cand_res - base_res)
    gates["no_new_resonance"] = {
        "passed": not new_res,
        "detail": ("no new resonance (base %s, candidate %s)"
                   % (sorted(base_res), sorted(cand_res))) if not new_res else
                  ("new resonance in ERB band(s) %s (error >= +6 dB, "
                   ">= 3 dB above neighbours)" % new_res),
    }

    # §4-4: no clip.
    gates["no_clip"] = {
        "passed": cand["level_max_dbfs"] < 0.0,
        "detail": "worst chunk levelMaxDbfs %.2f dBFS (clip at >= 0 dBFS)"
                  % cand["level_max_dbfs"],
    }

    # S8(a): region RMS, none may worsen > 1 dB.
    worst_region = None
    worst_delta = float("-inf")
    region_details = []
    for name, band_range in AB_REGIONS.items():
        base_r = region_rms(base["mean_error_db"], band_range)
        cand_r = region_rms(cand["mean_error_db"], band_range)
        delta = cand_r - base_r
        region_details.append("%s %.2f -> %.2f dB (%+.2f)" % (name, base_r, cand_r, delta))
        if delta > worst_delta:
            worst_delta = delta
            worst_region = name
    gates["region_rms"] = {
        "passed": worst_delta <= AB_MAX_REGION_WORSENING_DB,
        "detail": ("worst region %s worsened %+.2f dB (limit +%.1f dB); "
                   % (worst_region, worst_delta, AB_MAX_REGION_WORSENING_DB)
                   + "; ".join(region_details)),
    }

    # S8(b): APEX not working harder.
    apex_notes = []
    apex_ok = True
    if base["crest_min_db"] is not None and cand["crest_min_db"] is not None:
        crest_drop = base["crest_min_db"] - cand["crest_min_db"]
        apex_notes.append("crest min %.2f -> %.2f dB (drop %.2f dB, limit %.1f dB)"
                          % (base["crest_min_db"], cand["crest_min_db"],
                             crest_drop, AB_CREST_DROP_MAX_DB))
        if crest_drop > AB_CREST_DROP_MAX_DB:
            apex_ok = False
    else:
        apex_notes.append("crest telemetry absent; crest part of this gate skipped")
    base_pegged = abs(base["level_max_dbfs"] - APEX_CEILING_DBFS) <= AB_APEX_PEG_TOLERANCE_DB
    cand_pegged = abs(cand["level_max_dbfs"] - APEX_CEILING_DBFS) <= AB_APEX_PEG_TOLERANCE_DB
    apex_notes.append("levelMax pegged at -0.1 dBFS: base=%s candidate=%s"
                      % (base_pegged, cand_pegged))
    if cand_pegged and not base_pegged:
        apex_ok = False
    gates["apex_not_working_harder"] = {
        "passed": apex_ok,
        "detail": "; ".join(apex_notes),
    }

    # S8(c): loudness matching.
    level_delta = abs(cand["level_mean_dbfs"] - base["level_mean_dbfs"])
    loud_ok = level_delta <= AB_LEVEL_MATCH_DB
    loud_detail = ("|levelMean| %.2f vs %.2f dBFS (delta %.2f dB, limit %.1f dB)"
                   % (base["level_mean_dbfs"], cand["level_mean_dbfs"],
                      level_delta, AB_LEVEL_MATCH_DB))
    if base["lufs"] is not None and cand["lufs"] is not None:
        lufs_delta = abs(cand["lufs"] - base["lufs"])
        loud_detail += ("; |LUFS| %.2f vs %.2f (delta %.2f dB, limit %.1f dB)"
                        % (base["lufs"], cand["lufs"], lufs_delta, AB_LUFS_MATCH_DB))
        if lufs_delta > AB_LUFS_MATCH_DB:
            loud_ok = False
    else:
        loud_detail += "; lufs() telemetry absent, LUFS part skipped"
    gates["loudness_match"] = {
        "passed": loud_ok,
        "detail": loud_detail,
    }

    # S15: D10/P0 covariates. A move outside tolerance invalidates the
    # comparison (verdict "inconclusive"), it does not fail the
    # candidate. Absent telemetry skips the gate without blocking.
    covariates = check_d10_p0_covariates(base_sessions, candidate_sessions)
    gates["d10_p0_covariates"] = {
        "passed": covariates["status"] != "inconclusive",
        "detail": covariates["reason"],
    }

    s8_failed = not all(g["passed"] for name, g in gates.items()
                        if name != "d10_p0_covariates")
    if s8_failed:
        verdict = "rollback"
    elif covariates["status"] == "inconclusive":
        verdict = "inconclusive"
    else:
        verdict = "promote"
    return {"verdict": verdict, "gates": gates, "base": base, "candidate": cand}


# ---------------------------------------------------------------------------
# S5 via B — deterministic +2 dB ladder (Rol 2 P4, recommended way)
# ---------------------------------------------------------------------------

def emit_ladder(current_applied_db, target_db, base_version="v1.1"):
    """S5 via B: close a residual as a deterministic ladder of +2 dB per
    version rung (v1.1 -> v1.2 -> v1.3 ...), each rung with its own A/B.

    ``current_applied_db`` maps slot -> dB already applied (e.g. the v1.1
    clamped values); ``target_db`` maps slot -> full desired correction
    (e.g. +9 dB in brillo/presencia). Each rung moves every slot toward
    its target by at most LADDER_STEP_DB (2.0 dB), in the direction of the
    residual; the final rung takes the remainder (< 2 dB). Every rung
    recomputes ``trim_db = -(sum of slots)`` (net gain 0) and is marked
    ``requires_ab`` and ``reversible``. The clamp is NEVER widened in one
    jump: the +7 dB residual closes only through successive A/B-validated
    rungs.

    Returns a list of rung dicts (empty when nothing remains to climb),
    each with ``version``, ``eq_bands_db``, ``trim_db``,
    ``residual_remaining_db``, ``requires_ab`` and ``reversible``.
    """
    try:
        major, minor = base_version.lstrip("vV").split(".")
        minor = int(minor)
    except (ValueError, AttributeError):
        raise ValueError("base_version must look like 'v1.1'")
    prefix = "v%s." % major

    remaining = {}
    for slot in EQ_SLOTS:
        target = target_db.get(slot, current_applied_db.get(slot, 0.0))
        current = current_applied_db.get(slot, 0.0)
        delta = target - current
        if abs(delta) > 1e-9:
            remaining[slot] = delta
    if not remaining:
        return []

    rungs = []
    applied = {slot: current_applied_db.get(slot, 0.0) for slot in EQ_SLOTS}
    for step in range(1, LADDER_MAX_RUNGS + 1):
        still = {}
        for slot, delta in remaining.items():
            move = _clamp(delta, -LADDER_STEP_DB, LADDER_STEP_DB)
            applied[slot] = applied[slot] + move
            left = delta - move
            if abs(left) > 1e-9:
                still[slot] = left
        remaining = still
        rungs.append({
            "version": "%s%d" % (prefix, minor + step),
            "eq_bands_db": dict(applied),
            "trim_db": -sum(applied.values()),
            "residual_remaining_db": dict(remaining),
            "requires_ab": True,
            "reversible": True,
        })
        if not remaining:
            break
    return rungs


# ---------------------------------------------------------------------------
# S9 — coherence guard, server side (Rol 5 P3)
# ---------------------------------------------------------------------------

def coherence_guard(mean_coherence, per_band_coherence, corrections_by_band,
                    ab_measurement_only=False):
    """S9 (server side): with mean coherence below 0.20 the ERB table may
    be a phase artefact (comb), not tonal error; applying EQ on top of a
    comb null corrects a ghost.

    ``per_band_coherence`` is the 24-band coherence list (or None when
    the compiled profile does not carry it). ``corrections_by_band``
    maps 1-based ERB band index -> proposed correction in dB.

    Rules:
      * mean_coherence >= 0.20 -> every correction is allowed.
      * mean_coherence < 0.20 -> only corrections in bands whose own
        coherence is > 0.20 are allowed; the rest are blocked.
      * mean_coherence < 0.20 and no per-band coherence -> nothing is
        allowed (cannot tell which bands are trustworthy).
      * ``ab_measurement_only=True`` -> everything is allowed BUT marked
        ``measurement_only``: the A/B runs as a measurement, never as a
        path to promotion (the phone-side ProfileApplier must reject
        candidates with mean_coherence < 0.20 unless they carry this
        mark, per the spec).

    Returns ``{"allowed": {...}, "blocked": {...}, "mode": ...,
    "reason": ...}`` with mode "full" | "partial" | "none" |
    "measurement_only".
    """
    if mean_coherence >= COHERENCE_GUARD:
        return {
            "allowed": dict(corrections_by_band),
            "blocked": {},
            "mode": "full",
            "reason": "mean coherence %.3f >= %.2f; all corrections allowed"
                      % (mean_coherence, COHERENCE_GUARD),
        }

    if ab_measurement_only:
        return {
            "allowed": dict(corrections_by_band),
            "blocked": {},
            "mode": "measurement_only",
            "reason": "mean coherence %.3f < %.2f but ab_measurement_only "
                      "is set: corrections allowed ONLY as an A/B "
                      "measurement, never for promotion"
                      % (mean_coherence, COHERENCE_GUARD),
        }

    if per_band_coherence is None or len(per_band_coherence) != N_BANDS:
        return {
            "allowed": {},
            "blocked": dict(corrections_by_band),
            "mode": "none",
            "reason": "mean coherence %.3f < %.2f and no per-band "
                      "coherence is available: no correction can be "
                      "trusted, none emitted" % (mean_coherence, COHERENCE_GUARD),
        }

    allowed = {}
    blocked = {}
    for band, value in corrections_by_band.items():
        if 1 <= band <= N_BANDS and per_band_coherence[band - 1] > COHERENCE_GUARD:
            allowed[band] = value
        else:
            blocked[band] = value
    mode = "partial" if allowed else "none"
    return {
        "allowed": allowed,
        "blocked": blocked,
        "mode": mode,
        "reason": "mean coherence %.3f < %.2f: %d of %d band corrections "
                  "allowed (only bands with coherence > %.2f)"
                  % (mean_coherence, COHERENCE_GUARD,
                     len(allowed), len(corrections_by_band), COHERENCE_GUARD),
    }


# ---------------------------------------------------------------------------
# T7 — anti-poisoning: the server rejects anomalous sessions BEFORE
# aggregating (evolucion-magistral-autonomia-completa.md T7, closes
# ARTEX risk no. 2 / C17).
#
# Threat model: the autonomous cycle learns from the sessions that are
# uploaded. If the Bearer token leaks, if a buggy client sends corrupt
# sessions, or if someone replays old sessions, the deterministic
# analyzer would compile a poisoned profile and the A/B could promote
# it on its own. The A/B compares against a baseline but cannot tell
# "a real room that changed" from "false data". These rules run BEFORE
# any session enters the aggregate, in this order:
#   1. validate_chunk: physically impossible values are rejected.
#   2. check_replay: a session_id seen before, or an error table
#      bit-identical to an earlier accepted session, is rejected.
#   3. check_token_rate: how many sessions one token may contribute
#      per hour (a parameter -- the server knows the token).
#   4. quarantine_update: a session deviating > OUTLIER_DEVIATION_DB
#      in any band from the aggregated profile is HELD, not aggregated,
#      until 2 further sessions confirm the same deviation (same band,
#      same sign); otherwise it is discarded.
#   Every rejection (and every hold) is recorded with its reason via
#   log_rejection. gate_session() runs all four in order and is the
#   entry point the server calls per incoming session.
#
# State the server must keep (this module is pure; it never stores):
# per-token lists of accepted-session timestamps, the set of seen
# session_ids, the list of accepted error tables (or their hashes),
# the quarantine state dict, and the rejection log. All of it is
# passed in and returned; nothing here touches disk or network.
# ---------------------------------------------------------------------------

def _is_finite_number(value):
    return (isinstance(value, (int, float))
            and not isinstance(value, bool)
            and math.isfinite(value))


def _check_band_list(field, values, low, high, reasons):
    """Validate one 24-band field: list of 24 finite numbers in range.
    Appends human-readable reasons; returns True when valid."""
    if not isinstance(values, (list, tuple)):
        reasons.append("%s: not a list of 24 numbers" % field)
        return False
    if len(values) != N_BANDS:
        reasons.append("%s: %d bands, expected 24" % (field, len(values)))
        return False
    ok = True
    for i, value in enumerate(values):
        if not _is_finite_number(value):
            reasons.append("%s: ERB-%d is not a finite number (%r)"
                           % (field, i + 1, value))
            ok = False
        elif value < low or value > high:
            reasons.append("%s: ERB-%d %.2f dB outside [%.1f, %.1f]"
                           % (field, i + 1, value, low, high))
            ok = False
    return ok


def _check_scalar(field, value, low, high, reasons):
    """Validate one scalar field: finite number in range."""
    if not _is_finite_number(value):
        reasons.append("%s: not a finite number (%r)" % (field, value))
        return False
    if value < low or value > high:
        reasons.append("%s: %.2f outside [%.1f, %.1f]"
                       % (field, value, low, high))
        return False
    return True


def validate_chunk(chunk):
    """T7 rule 1: reject chunks with physically impossible values.

    ``chunk`` is a session/chunk dict. ``error_mean_db`` (24 bands) is
    required; every other field is optional and validated only when
    present, like S8 treats optional telemetry: ``magnitude_db`` (24),
    ``coherence`` (24, per-band), ``mean_coherence``, ``level_max_dbfs``,
    ``level_mean_dbfs``, ``spectral_error_rms_db``, ``erb_std_db`` (24),
    ``crest_db``, ``lufs`` — and the S15 D10/P0 telemetry fields
    ``d10_action``, ``p0_db`` and ``trim_db_effective``.

    A value is rejected when it is not a finite number (NaN/inf from a
    corrupt or truncated payload) or outside its physical range: error
    +/-40 dB, magnitude -120..0 dBFS, coherence 0..1, levels -120..0
    dBFS, RMS 0..40 dB, erbStdDb 0..40 dB, crest 0..60 dB, LUFS
    -70..0 dB. All bounds are the CHUNK_* constants; the dBFS/coherence
    ones follow from the definitions, the rest are proposals (see the
    constants block and README §T7). The S15 fields carry NO range
    bound: no scale or physical range for the D10 action or the P0 is
    fixed in any spec, so they are checked only for finiteness —
    inventing a range for them would be inventing a metric.

    Returns ``{"ok": bool, "reasons": [str]}``; every reason names the
    field, the band when it applies, and the offending value.
    """
    reasons = []
    if not isinstance(chunk, dict):
        return {"ok": False,
                "reasons": ["chunk is not a dict (%r)" % (chunk,)]}
    ok = True
    if "error_mean_db" not in chunk:
        return {"ok": False, "reasons": ["error_mean_db: missing (required)"]}
    if not _check_band_list("error_mean_db", chunk["error_mean_db"],
                            CHUNK_ERROR_DB_MIN, CHUNK_ERROR_DB_MAX, reasons):
        ok = False
    if "magnitude_db" in chunk and not _check_band_list(
            "magnitude_db", chunk["magnitude_db"],
            CHUNK_MAGNITUDE_DB_MIN, CHUNK_MAGNITUDE_DB_MAX, reasons):
        ok = False
    if "coherence" in chunk and not _check_band_list(
            "coherence", chunk["coherence"],
            CHUNK_COHERENCE_MIN, CHUNK_COHERENCE_MAX, reasons):
        ok = False
    if "erb_std_db" in chunk and not _check_band_list(
            "erb_std_db", chunk["erb_std_db"],
            CHUNK_ERB_STD_DB_MIN, CHUNK_ERB_STD_DB_MAX, reasons):
        ok = False
    for field, low, high in (
            ("mean_coherence", CHUNK_COHERENCE_MIN, CHUNK_COHERENCE_MAX),
            ("level_max_dbfs", CHUNK_LEVEL_DBFS_MIN, CHUNK_LEVEL_DBFS_MAX),
            ("level_mean_dbfs", CHUNK_LEVEL_DBFS_MIN, CHUNK_LEVEL_DBFS_MAX),
            ("spectral_error_rms_db", 0.0, CHUNK_RMS_DB_MAX),
            ("crest_db", CHUNK_CREST_DB_MIN, CHUNK_CREST_DB_MAX),
            ("lufs", CHUNK_LUFS_MIN_DB, CHUNK_LUFS_MAX_DB)):
        if field in chunk and chunk[field] is not None:
            if not _check_scalar(field, chunk[field], low, high, reasons):
                ok = False
    # S15 telemetry: finite-number check only, no range bound (see the
    # docstring: no scale/range for D10 action or P0 is fixed anywhere).
    for field in ("d10_action", "p0_db", "trim_db_effective"):
        if field in chunk and chunk[field] is not None:
            if not _is_finite_number(chunk[field]):
                reasons.append("%s: not a finite number (%r)"
                               % (field, chunk[field]))
                ok = False
    return {"ok": ok, "reasons": reasons}


def check_token_rate(prior_timestamps, now_epoch,
                     max_sessions_per_hour=DEFAULT_MAX_SESSIONS_PER_HOUR,
                     window_seconds=RATE_WINDOW_SECONDS):
    """T7 rule 2: how many sessions one token may contribute per hour.

    ``prior_timestamps`` is the list of epoch seconds at which this
    token's sessions were previously ACCEPTED (the server keeps it per
    token); ``now_epoch`` is the current epoch second. Only timestamps
    inside (now - window, now] count; older ones have expired and a
    timestamp in the future is not counted. ``max_sessions_per_hour``
    is a parameter -- the server passes its own value; the default is
    the proposal (6/h, see the constants block).

    Returns ``{"allowed": bool, "count_in_window": int,
    "max_sessions_per_hour": int, "reason": str}``.
    """
    count = sum(1 for ts in prior_timestamps
                if now_epoch - window_seconds < ts <= now_epoch)
    allowed = count < max_sessions_per_hour
    return {
        "allowed": allowed,
        "count_in_window": count,
        "max_sessions_per_hour": max_sessions_per_hour,
        "reason": ("token has %d session(s) in the last %d s (limit %d/h)"
                   % (count, window_seconds, max_sessions_per_hour))
                  + ("" if allowed else
                     ": rate limit exceeded, session rejected"),
    }


def check_replay(session_id, seen_session_ids, error_mean_db=None,
                 seen_error_tables=None):
    """T7 replay check: a re-sent old session must not enter twice.

    ``session_id`` is the session's id when the client sends one (None
    when it does not); ``seen_session_ids`` is the set/list of ids the
    server already accepted. A repeated id is a replay. Independently,
    an error table bit-identical to an earlier accepted session's table
    is a replay too: a genuine new listening session never reproduces
    24 floats exactly. ``seen_error_tables`` is the list of accepted
    tables (the server may store hashes instead and compare those --
    this function compares tables as given).

    Returns ``{"ok": bool, "reason": str}``; ok with an empty reason
    when there is nothing to object to.
    """
    if session_id is not None and session_id in seen_session_ids:
        return {"ok": False,
                "reason": "replay: session_id %r was already accepted"
                          % (session_id,)}
    if error_mean_db is not None and seen_error_tables:
        for table in seen_error_tables:
            if list(table) == list(error_mean_db):
                return {"ok": False,
                        "reason": "replay: error table is identical to an "
                                  "earlier accepted session"}
    return {"ok": True, "reason": ""}


def classify_session_outlier(error_mean_db, aggregate_error_db):
    """T7 rule 3, classification half: compare one session's 24-band
    error table against the aggregated profile.

    A band deviates when |session - aggregate| > OUTLIER_DEVIATION_DB
    (6 dB, proposal). With no aggregate yet (cold start) there is
    nothing to deviate from and the session is accepted.

    Returns ``{"status": "accepted" | "held", "deviating_bands":
    {band: signed deviation in dB}, "max_deviation_db": float,
    "reason": str}``. ``deviating_bands`` uses 1-based ERB indices.
    """
    if aggregate_error_db is None:
        return {"status": "accepted", "deviating_bands": {},
                "max_deviation_db": 0.0,
                "reason": "no aggregate profile yet; nothing to compare against"}
    if len(error_mean_db) != N_BANDS or len(aggregate_error_db) != N_BANDS:
        raise ValueError("error tables must have exactly 24 bands")
    deviating = {}
    max_dev = 0.0
    for i, (session_err, agg_err) in enumerate(
            zip(error_mean_db, aggregate_error_db)):
        dev = session_err - agg_err
        if abs(dev) > max_dev:
            max_dev = abs(dev)
        if abs(dev) > OUTLIER_DEVIATION_DB:
            deviating[i + 1] = dev
    if not deviating:
        return {"status": "accepted", "deviating_bands": {},
                "max_deviation_db": max_dev,
                "reason": "max band deviation %.2f dB <= %.1f dB threshold"
                          % (max_dev, OUTLIER_DEVIATION_DB)}
    bands = ", ".join("ERB-%d (%+.1f dB)" % (b, d)
                      for b, d in sorted(deviating.items()))
    return {"status": "held", "deviating_bands": deviating,
            "max_deviation_db": max_dev,
            "reason": "held: band deviation > %.1f dB in %s; enters "
                      "aggregation only after %d further session(s) "
                      "confirm the same deviation (same band, same sign)"
                      % (OUTLIER_DEVIATION_DB, bands,
                         OUTLIER_CONFIRM_SESSIONS)}


def _empty_quarantine_state():
    return {"held": []}


def quarantine_update(state, session_id, error_mean_db, aggregate_error_db):
    """T7 rule 3, stateful half: hold outliers until 2 sessions confirm.

    ``state`` is ``{"held": [records]}``; a record carries
    ``session_id``, ``error_mean_db``, ``deviating_bands`` (from
    :func:`classify_session_outlier`), ``confirmations`` and
    ``sessions_seen_after``. The function never mutates its input: it
    returns ``(new_state, decision)``.

    On each incoming session:
      * every held record ages by one session (``sessions_seen_after``
        += 1). The incoming session confirms a held record when, in at
        least one of the record's deviating bands, the incoming
        deviation from the aggregate has the same sign and exceeds
        OUTLIER_CONFIRM_DB (3 dB, proposal); that adds one confirmation
        (at most one per session, however many bands match). The
        incoming session counts as a confirmer whether it is itself
        accepted or held -- a real room change produces consecutive
        deviating sessions.
      * a record with >= 2 confirmations is RELEASED: it leaves the
        quarantine and its error table is returned in
        ``decision["released_sessions"]`` so the server can aggregate
        it (the room really changed).
      * a record that has seen 2 further sessions without 2
        confirmations is DISCARDED: its id is returned in
        ``decision["discarded_session_ids"]`` and it never enters the
        aggregate. Release is checked before discard, so a record
        confirmed on its second following session is released, not
        discarded.
      * the incoming session itself is classified; when held it is
        appended as a new record (confirmations 0).

    ``decision`` is ``{"status": "accepted" | "held", "session_id":
    ..., "released_sessions": [...], "discarded_session_ids": [...],
    "reason": str}``. ``status`` describes the incoming session only;
    releases/discards of older held sessions ride along in the same
    decision.
    """
    state = {"held": [dict(r) for r in state.get("held", [])]}
    decision = {"status": "accepted", "session_id": session_id,
                "released_sessions": [], "discarded_session_ids": [],
                "reason": ""}
    if aggregate_error_db is None:
        decision["reason"] = ("no aggregate profile yet; session accepted, "
                              "quarantine untouched")
        return state, decision

    incoming = classify_session_outlier(error_mean_db, aggregate_error_db)
    incoming_dev = [s - a for s, a in zip(error_mean_db, aggregate_error_db)]

    still_held = []
    for record in state["held"]:
        record["sessions_seen_after"] += 1
        confirmed = False
        for band, dev in record["deviating_bands"].items():
            incoming_band_dev = incoming_dev[band - 1]
            if (incoming_band_dev * dev > 0.0
                    and abs(incoming_band_dev) > OUTLIER_CONFIRM_DB):
                confirmed = True
                break
        if confirmed:
            record["confirmations"] += 1
        if record["confirmations"] >= OUTLIER_CONFIRM_SESSIONS:
            decision["released_sessions"].append({
                "session_id": record["session_id"],
                "error_mean_db": list(record["error_mean_db"]),
            })
        elif record["sessions_seen_after"] >= OUTLIER_CONFIRM_SESSIONS:
            decision["discarded_session_ids"].append(record["session_id"])
        else:
            still_held.append(record)
    state["held"] = still_held

    if incoming["status"] == "held":
        state["held"].append({
            "session_id": session_id,
            "error_mean_db": list(error_mean_db),
            "deviating_bands": dict(incoming["deviating_bands"]),
            "confirmations": 0,
            "sessions_seen_after": 0,
        })
        decision["status"] = "held"
    decision["reason"] = incoming["reason"]
    if decision["released_sessions"]:
        decision["reason"] += ("; released held session(s) %s (confirmed "
                               "by %d further session(s))"
                               % ([r["session_id"]
                                  for r in decision["released_sessions"]],
                                  OUTLIER_CONFIRM_SESSIONS))
    if decision["discarded_session_ids"]:
        decision["reason"] += ("; discarded held session(s) %s (not "
                               "confirmed by the next %d session(s))"
                               % (decision["discarded_session_ids"],
                                  OUTLIER_CONFIRM_SESSIONS))
    return state, decision


def log_rejection(log, session_id, rule, reason, decision="rejected",
                  token=None):
    """T7 rule 4: record every rejection (and every hold) with its reason.

    ``log`` is the server's list of entries; a new list is returned, the
    input is not mutated. An entry is ``{"session_id": ..., "token":
    ..., "rule": ..., "decision": "rejected" | "held", "reason": ...}``.
    The server stamps the entry with its own timestamp when it persists
    it -- this module has no clock. ``rule`` is one of
    ``chunk_out_of_range``, ``replay``, ``rate_limit``, ``outlier_hold``.
    """
    entry = {"session_id": session_id, "token": token, "rule": rule,
             "decision": decision, "reason": reason}
    return list(log) + [entry]


def rejection_summary(log):
    """Count a rejection log by rule and by decision, for the server's
    triage view. Returns ``{"total": int, "by_rule": {...},
    "by_decision": {...}}``."""
    by_rule = {}
    by_decision = {}
    for entry in log:
        by_rule[entry.get("rule", "?")] = by_rule.get(entry.get("rule", "?"), 0) + 1
        by_decision[entry.get("decision", "?")] = (
            by_decision.get(entry.get("decision", "?"), 0) + 1)
    return {"total": len(log), "by_rule": by_rule, "by_decision": by_decision}


def gate_session(session, aggregate_error_db, token_history, now_epoch,
                 quarantine_state, rejection_log, token=None,
                 seen_session_ids=None, seen_error_tables=None,
                 max_sessions_per_hour=DEFAULT_MAX_SESSIONS_PER_HOUR):
    """T7 entry point: gate ONE incoming session before aggregation.

    Runs the rules in order; the first failure stops the pipeline and
    the session never enters the aggregate:
      1. :func:`validate_chunk` -- physically impossible values.
      2. :func:`check_replay` -- re-sent session_id or identical table.
      3. :func:`check_token_rate` -- sessions per hour for this token.
      4. :func:`quarantine_update` -- per-band outlier hold.

    ``session`` is the chunk dict (with optional ``session_id``);
    ``aggregate_error_db`` is the current aggregated 24-band table (None
    on cold start); ``token_history`` the token's accepted-session epoch
    timestamps; ``now_epoch`` the current epoch second;
    ``quarantine_state`` / ``rejection_log`` the server-kept state
    (both returned updated, inputs untouched); ``seen_session_ids`` /
    ``seen_error_tables`` the replay state (empty when the server does
    not track it -- the replay check then only covers what is given).

    Returns ``{"decision": {...}, "quarantine_state": ...,
    "rejection_log": ...}``. ``decision`` is ``{"status": "accepted" |
    "held" | "rejected", "reason": str, "released_sessions": [...],
    "discarded_session_ids": [...]}``. On "accepted" the server
    aggregates the session (and any released held sessions) and records
    ``now_epoch`` in the token history, the session_id in the seen set,
    and the error table in the seen tables. On "held" the server
    aggregates nothing yet. On "rejected" the server aggregates
    nothing and the reason is already in the returned rejection log.
    """
    session_id = session.get("session_id") if isinstance(session, dict) else None
    released = []
    discarded = []

    check = validate_chunk(session)
    if not check["ok"]:
        reason = "chunk_out_of_range: " + "; ".join(check["reasons"])
        new_log = log_rejection(rejection_log, session_id,
                                "chunk_out_of_range", reason,
                                decision="rejected", token=token)
        return {"decision": {"status": "rejected", "reason": reason,
                             "released_sessions": released,
                             "discarded_session_ids": discarded},
                "quarantine_state": quarantine_state,
                "rejection_log": new_log}

    replay = check_replay(session_id, seen_session_ids or [],
                          error_mean_db=session.get("error_mean_db"),
                          seen_error_tables=seen_error_tables)
    if not replay["ok"]:
        new_log = log_rejection(rejection_log, session_id, "replay",
                                replay["reason"], decision="rejected",
                                token=token)
        return {"decision": {"status": "rejected", "reason": replay["reason"],
                             "released_sessions": released,
                             "discarded_session_ids": discarded},
                "quarantine_state": quarantine_state,
                "rejection_log": new_log}

    rate = check_token_rate(token_history, now_epoch,
                            max_sessions_per_hour=max_sessions_per_hour)
    if not rate["allowed"]:
        new_log = log_rejection(rejection_log, session_id, "rate_limit",
                                rate["reason"], decision="rejected",
                                token=token)
        return {"decision": {"status": "rejected", "reason": rate["reason"],
                             "released_sessions": released,
                             "discarded_session_ids": discarded},
                "quarantine_state": quarantine_state,
                "rejection_log": new_log}

    new_state, q_decision = quarantine_update(
        quarantine_state, session_id, session["error_mean_db"],
        aggregate_error_db)
    status = q_decision["status"]
    new_log = rejection_log
    if status == "held":
        new_log = log_rejection(rejection_log, session_id, "outlier_hold",
                                q_decision["reason"], decision="held",
                                token=token)
    return {"decision": {"status": status, "reason": q_decision["reason"],
                         "released_sessions": q_decision["released_sessions"],
                         "discarded_session_ids": q_decision["discarded_session_ids"]},
            "quarantine_state": new_state,
            "rejection_log": new_log}


# ---------------------------------------------------------------------------
# T6 — cumulative excursion cap + promotion brake
# (evolucion-magistral-autonomia-completa.md T6).
#
# Threat model: successive +2 dB rungs can walk the EQ to +6/+8 dB over
# several versions without any human ever listening — the per-version
# clamp does not limit the accumulated drift. And without an antichurn
# brake, a server publishing a new version at every poll can keep the
# phone applying profiles non-stop: the A/B never completes its 3
# stable sessions, the motor never settles, and a rollback can be
# reversed at the next poll. These rules run on the SERVER, before a
# promotion is published:
#   1. check_cumulative_excursion: no slot's cumulative value vs the
#      factory baseline (0.0 dB) may exceed +/-CUMULATIVE_EXCURSION_CAP_DB.
#      A breach HALTS the cycle (halt_needs_human): the server stops
#      publishing and asks Esse — it does not truncate silently.
#   2. check_promotion_brake: at most MAX_PROMOTIONS_PER_DAY promotions
#      per calendar day. A breach is a WAIT (publish again tomorrow),
#      not a halt.
# gate_promotion() runs both and is the entry point the server calls
# before publishing a promotion. compile_candidate() also reports the
# excursion verdict of the whole planned climb in its "t6" entry, so
# the server knows in advance which rung would breach the cap.
#
# The phone-side half of T6 (dwell, daily auto-apply cap, 24 h version
# veto, A/B timeout, master switch also stopping the poll) lives in
# OrganismController.kt, not here: this module never sees the phone.
# ---------------------------------------------------------------------------

def check_cumulative_excursion(slot_values_db,
                               cap_db=CUMULATIVE_EXCURSION_CAP_DB):
    """T6 rule (a): cumulative excursion per slot vs the factory baseline.

    ``slot_values_db`` maps motor slot -> the CUMULATIVE EQ value the
    profile would carry (dB), i.e. a ladder rung's ``eq_bands_db`` (the
    total after the whole climb), not a single version's increment.
    The factory baseline is 0.0 dB in every slot (no correction). Slots
    outside EQ_SLOTS are ignored (the motor has no such slot).

    Exactly at the cap is allowed; beyond it is a breach. Returns
    ``{"ok": bool, "excursions_db": {slot: value},
    "max_excursion_db": float, "exceeded_slots": [slot],
    "cap_db": float, "reason": str}``.
    """
    excursions = {slot: float(slot_values_db.get(slot, 0.0))
                  for slot in EQ_SLOTS if slot in slot_values_db}
    max_excursion = max([abs(v) for v in excursions.values()] or [0.0])
    exceeded = sorted(slot for slot, value in excursions.items()
                      if abs(value) > cap_db + 1e-9)
    ok = not exceeded
    if ok:
        reason = ("max cumulative excursion %.2f dB <= cap +/-%.1f dB "
                  "(factory baseline 0.0 dB per slot)"
                  % (max_excursion, cap_db))
    else:
        reason = ("cumulative excursion exceeds the cap +/-%.1f dB in "
                  "%s (factory baseline 0.0 dB per slot): %s"
                  % (cap_db, exceeded,
                     ", ".join("%s %+.2f dB" % (slot, excursions[slot])
                               for slot in exceeded)))
    return {
        "ok": ok,
        "excursions_db": excursions,
        "max_excursion_db": max_excursion,
        "exceeded_slots": exceeded,
        "cap_db": cap_db,
        "reason": reason,
    }


def check_promotion_brake(promotions_today,
                          max_promotions_per_day=MAX_PROMOTIONS_PER_DAY):
    """T6 rule (a): at most one promotion per calendar day.

    ``promotions_today`` is how many promotions the server already
    published today (the server counts per calendar day and passes the
    count; this module has no clock). A breach is a WAIT — the server
    publishes again tomorrow — not a halt.

    Returns ``{"allowed": bool, "promotions_today": int,
    "max_promotions_per_day": int, "reason": str}``.
    """
    if promotions_today < 0:
        raise ValueError("promotions_today must be >= 0")
    allowed = promotions_today < max_promotions_per_day
    return {
        "allowed": allowed,
        "promotions_today": promotions_today,
        "max_promotions_per_day": max_promotions_per_day,
        "reason": ("promotions today: %d/%d" % (promotions_today,
                                                max_promotions_per_day))
                  + ("" if allowed else
                     ": daily promotion brake — wait until tomorrow"),
    }


def gate_promotion(candidate_slots_db, promotions_today,
                   cap_db=CUMULATIVE_EXCURSION_CAP_DB,
                   max_promotions_per_day=MAX_PROMOTIONS_PER_DAY):
    """T6 entry point: gate ONE promotion before the server publishes it.

    ``candidate_slots_db`` is the CUMULATIVE slot values the promoted
    profile would carry (a ladder rung's ``eq_bands_db``); see
    :func:`check_cumulative_excursion`. ``promotions_today`` is the
    server's count of promotions already published today; see
    :func:`check_promotion_brake`.

    Returns ``{"allowed": bool, "halt_needs_human": bool,
    "excursion": {...}, "promotion_brake": {...}, "reasons": [str]}``.
    ``allowed`` is False when either rule fails. ``halt_needs_human``
    is True only on an excursion breach: the cycle STOPS and asks Esse
    (the cap is his limit — "solo él puede fijar ese límite"). A
    promotion-brake failure alone only waits until tomorrow.
    """
    excursion = check_cumulative_excursion(candidate_slots_db, cap_db=cap_db)
    brake = check_promotion_brake(promotions_today,
                                  max_promotions_per_day=max_promotions_per_day)
    reasons = []
    if not excursion["ok"]:
        reasons.append(excursion["reason"])
    if not brake["allowed"]:
        reasons.append(brake["reason"])
    return {
        "allowed": excursion["ok"] and brake["allowed"],
        "halt_needs_human": not excursion["ok"],
        "excursion": excursion,
        "promotion_brake": brake,
        "reasons": reasons,
    }


# ---------------------------------------------------------------------------
# T3 — confirmation "sounding, not just installed" (Rol 1 P4)
# (evolucion-magistral-autonomia-completa.md T3).
#
# The spec's POST /api/v1/muse/ivanna/profile/applied only says
# "installed". A profile installed with the daemon down, no music, or
# the sensor stopped would generate 3 "empty" sessions that the
# server could read as a failure of the candidate (or as a success
# through silence). So the server only starts counting the 3 test
# sessions when it sees evidence that it is sounding: the effect
# beacon advancing + valid frames with the candidate. The phone side
# (T3's Android half: exposing daemonOk in OrganismController.State
# and sending the evidence in the confirmation / first chunk) is not
# in this module; this is the server-side counting rule.
#
# Evidence fields (all optional in the incoming chunk — a chunk
# without them simply carries no evidence and does not count):
#   * ``effect_beacon_frames`` (int): the DSP effect beacon frame
#     counter, read via DSPBridge.effectBeaconFrames(). It must be
#     ADVANCING — strictly greater than the previous observation the
#     server saw for this candidate. A first observation, with no
#     previous value to compare against, cannot establish advance yet
#     and does not count: the server must keep the previous value and
#     wait for the next chunk.
#   * ``snapshots_accepted`` (int): snapshots the sensor consumer
#     accepted (SensorSnapshotConsumer.snapshotsAccepted). Must be > 0.
#   * ``frames_valid`` (bool, or int count): framesValid of the chunk.
#     A bool must be True; an int must be > 0.
#   * ``daemon_ok`` (bool): daemon health from the reconnection loop.
#     Must be True — it is what distinguishes "applied without a
#     daemon" from "applied and sounding".
# A chunk without evidence does NOT count: it is neither a success
# nor a failure, it simply does not advance the A/B count. This
# module never defines session boundaries — the server owns them; it
# feeds ordered chunk evidences here and counts sessions from the
# chunks that count.
# ---------------------------------------------------------------------------

def check_playing_evidence(evidence, previous_beacon_frames=None):
    """T3: decide whether ONE chunk counts toward the A/B.

    ``evidence`` is the chunk's evidence dict (any subset of the four
    fields above; missing fields are missing evidence). ``previous_beacon_frames``
    is the effect-beacon value the server saw on the previous chunk
    for this candidate (None on the first observation).

    Returns ``{"counts": bool, "status": "counts" | "not_counted",
    "checks": {"daemon_ok": bool, "frames_valid": bool,
    "snapshots_accepted": bool, "beacon_advancing": bool},
    "reason": str}``. ``counts`` is True only when all four checks
    pass. ``reason`` names every failing check, so the server can tell
    "no music" from "daemon down" from "beacon stalled".
    """
    if not isinstance(evidence, dict):
        evidence = {}

    checks = {}
    failures = []

    daemon_ok = evidence.get("daemon_ok")
    checks["daemon_ok"] = daemon_ok is True
    if daemon_ok is None:
        failures.append("daemon_ok missing (cannot tell 'applied without "
                        "a daemon' from 'applied and sounding')")
    elif daemon_ok is not True:
        failures.append("daemon_ok is False (daemon down)")

    frames_valid = evidence.get("frames_valid")
    if isinstance(frames_valid, bool):
        checks["frames_valid"] = frames_valid
    elif isinstance(frames_valid, (int, float)) and not isinstance(frames_valid, bool):
        checks["frames_valid"] = frames_valid > 0
    else:
        checks["frames_valid"] = False
    if frames_valid is None:
        failures.append("frames_valid missing (no valid-frame evidence)")
    elif not checks["frames_valid"]:
        failures.append("frames_valid is %r (no valid frames)" % (frames_valid,))

    snapshots = evidence.get("snapshots_accepted")
    checks["snapshots_accepted"] = (
        isinstance(snapshots, (int, float))
        and not isinstance(snapshots, bool) and snapshots > 0)
    if snapshots is None:
        failures.append("snapshots_accepted missing (no consumer evidence)")
    elif not checks["snapshots_accepted"]:
        failures.append("snapshots_accepted is %r (consumer accepted nothing)"
                        % (snapshots,))

    beacon = evidence.get("effect_beacon_frames")
    if (isinstance(beacon, (int, float)) and not isinstance(beacon, bool)
            and isinstance(previous_beacon_frames, (int, float))
            and not isinstance(previous_beacon_frames, bool)):
        checks["beacon_advancing"] = beacon > previous_beacon_frames
    else:
        checks["beacon_advancing"] = False
    if beacon is None:
        failures.append("effect_beacon_frames missing (no beacon evidence)")
    elif previous_beacon_frames is None:
        failures.append("first beacon observation (%r): advance not yet "
                        "established, wait for the next chunk" % (beacon,))
    elif not checks["beacon_advancing"]:
        failures.append("effect beacon NOT advancing (%r -> %r)"
                        % (previous_beacon_frames, beacon))

    counts = all(checks.values())
    if counts:
        reason = ("T3: chunk counts toward the A/B — daemon ok, "
                  "frames valid, snapshots accepted, effect beacon "
                  "advancing (%r -> %r)"
                  % (previous_beacon_frames, beacon))
    else:
        reason = ("T3: chunk does NOT count toward the A/B (neither "
                  "success nor failure): " + "; ".join(failures))
    return {
        "counts": counts,
        "status": "counts" if counts else "not_counted",
        "checks": checks,
        "reason": reason,
    }


def count_playing_chunks(evidences, previous_beacon_frames=None):
    """T3, sequential helper: thread the beacon state across an
    ordered list of chunk evidences (oldest first) and count how many
    chunks carry sounding evidence.

    ``evidences`` is a list of evidence dicts (see
    :func:`check_playing_evidence`); ``previous_beacon_frames`` is the
    beacon value before the first chunk in the list (None when the
    server has never seen one for this candidate). After each chunk,
    the last seen beacon value becomes the previous for the next one
    (a chunk without a beacon value leaves it unchanged).

    Returns ``{"counted": int, "not_counted": int,
    "decisions": [per-chunk check_playing_evidence results],
    "last_beacon_frames": int | None}``. The server counts its 3 A/B
    sessions from the counted chunks; this function does not define
    what a session is.
    """
    decisions = []
    last_beacon = previous_beacon_frames
    for evidence in evidences:
        decision = check_playing_evidence(evidence or {}, last_beacon)
        decisions.append(decision)
        beacon = (evidence or {}).get("effect_beacon_frames")
        if (isinstance(beacon, (int, float))
                and not isinstance(beacon, bool)):
            last_beacon = beacon
    counted = sum(1 for d in decisions if d["counts"])
    return {
        "counted": counted,
        "not_counted": len(decisions) - counted,
        "decisions": decisions,
        "last_beacon_frames": last_beacon,
    }


# ---------------------------------------------------------------------------
# Pipeline: §2.3 regional correction + S9 guard + S6 split (+ S5 ladder)
# ---------------------------------------------------------------------------

def compile_candidate(error_mean_db, coherence_mean_db, mean_coherence,
                      erb_std_db=None, level_max_dbfs=-6.0,
                      exciter_amount=0.3, v1_1a_promoted=False,
                      room_corr_available=False, base_version="v1.1",
                      ab_measurement_only=False, deficit_confirmed=False):
    """Full deterministic pipeline for one compiled profile table.

    1. §2.3 regional correction (deadband, clamp +/-2 dB, trim, residual).
    2. S2 comb-vs-response fit (informational; its verdict is reported,
       it does not by itself block emission -- S9 does that).
    3. S9 coherence guard on the per-band corrections implied by the
       regional correction (a region's correction is applied to its bands
       only where the guard allows it; a region with no allowed band
       keeps its correction as documented residual).
    3b. S10 residual freeze: regions whose |residual| > 4 dB have their
       EQ steps FROZEN (their slots leave the split and their ladder
       targets stay at the currently applied value — the ladder does
       not climb there) and EVO-003 is marked as the next required
       step, unless the freeze is released (mean coherence >= 0.20 or
       ``deficit_confirmed`` — the EVO-003 measurement confirmed the
       deficit). See :func:`check_residual_freeze`.
    4. S6 split into v1.1a (cut-only) / v1.1b (boost, gated on
       ``v1_1a_promoted``), each headroom pre-checked.
    5. S5 ladder for the remaining positive residual (rungs after the
       current version), so the server knows the whole climb in advance.
    6. T6 excursion verdict on the whole planned climb: the cumulative
       slot values of every ladder rung are checked against the
       +/-CUMULATIVE_EXCURSION_CAP_DB cap vs the factory baseline
       (0.0 dB). ``t6["halt_needs_human"]`` is True when any rung would
       breach the cap, with ``first_breaching_rung`` naming the first
       rung that does; the server must not publish that rung or any
       later one, and must stop the cycle pending a human decision.
       With no ladder (nothing left to climb) the check runs on the
       currently applied (clamped) slots instead.

    ``coherence_mean_db`` is the 24-band per-band coherence list.
    Returns a dict with keys ``regional``, ``s2``, ``coherence_guard``,
    ``s10``, ``split``, ``ladder`` and ``t6``. Pure function; the server decides whether
    to publish the returned candidates.
    """
    regional = regional_correction(error_mean_db)
    s2 = fit_comb_vs_response(error_mean_db, erb_std_db)
    s10 = check_residual_freeze(regional, mean_coherence,
                                deficit_confirmed=deficit_confirmed)

    # Per-band corrections implied by the regional correction: each band
    # inherits its region's proposed correction.
    per_band = {}
    for region_name, band_range in REGIONS.items():
        proposed = regional["proposed_db"][region_name]
        if proposed == 0.0:
            continue
        for band in range(band_range[0], band_range[1] + 1):
            per_band[band] = proposed
    guard = coherence_guard(mean_coherence, coherence_mean_db, per_band,
                            ab_measurement_only=ab_measurement_only)

    # The S6 split works on the motor slots. A slot whose region was
    # fully blocked by the S9 guard contributes nothing to the split;
    # a partially blocked region keeps its slot value (the guard is
    # per-band, the slot is per-region -- the partial case is documented
    # in the notes, not silently applied band by band, because the motor
    # has no per-band slot). A slot whose region is FROZEN by S10
    # (|residual| > 4 dB) contributes nothing either: no EQ step is
    # proposed in a frozen region until EVO-003 clears it.
    slot_for_region = {region: slot for slot, region in SLOT_REGION.items()}
    blocked_regions = set()
    for region_name, band_range in REGIONS.items():
        slot = slot_for_region.get(region_name)
        if slot is None:
            continue
        bands = list(range(band_range[0], band_range[1] + 1))
        allowed_bands = [b for b in bands if b in guard["allowed"]]
        if bands and not allowed_bands:
            blocked_regions.add(region_name)
    frozen_regions = set(s10["frozen_regions"])
    proposed_slots = {}
    for slot in EQ_SLOTS:
        region = SLOT_REGION[slot]
        if region in blocked_regions or region in frozen_regions:
            continue
        proposed_slots[slot] = regional["proposed_db"][region]
    # ERB-3 roomCorr cut: the medios-graves proposed correction, if any.
    room_corr_cut = regional["proposed_db"]["medios_graves"]
    if room_corr_cut >= 0.0:
        room_corr_cut = None

    split = split_cut_boost(
        proposed_slots,
        level_max_dbfs=level_max_dbfs,
        exciter_amount=exciter_amount,
        v1_1a_promoted=v1_1a_promoted,
        room_corr_cut_db=room_corr_cut,
        room_corr_available=room_corr_available,
    )
    if blocked_regions:
        split["notes"].append(
            "S9 guard blocked region(s) %s entirely (no band with "
            "coherence > %.2f); their corrections stay as documented "
            "residual, not in any candidate"
            % (sorted(blocked_regions), COHERENCE_GUARD)
        )
    if s10["frozen_regions"]:
        split["notes"].append(
            "S10 freeze: region(s) %s have |residual| > %.1f dB; their "
            "EQ steps are FROZEN (slots %s propose nothing) — EVO-003 "
            "is the next required step%s"
            % (s10["frozen_regions"], S10_RESIDUAL_FREEZE_DB,
               s10["frozen_slots"] or "none (unslotted regions only)",
               "; firing pending the S1 reference fix (mean coherence "
               "%.3f < %.2f, residual not trustworthy as a room deficit)"
               % (mean_coherence, COHERENCE_GUARD)
               if s10["evo003_pending_reference_fix"] else "")
        )
    if guard["mode"] == "partial":
        split["notes"].append(
            "S9 guard is PARTIAL: some bands blocked; slot values are "
            "region-wide and cannot express per-band blocking, so the "
            "split above uses the region correction as proposed -- the "
            "server must treat this candidate as measurement-only until "
            "coherence rises"
        )

    # S5 ladder: from the currently applied (clamped) slots toward the
    # full proposed targets. A frozen region's target stays at the
    # currently applied value: the ladder does not climb where S10
    # froze the steps.
    target_slots = {}
    for slot in EQ_SLOTS:
        region = SLOT_REGION[slot]
        if region in frozen_regions:
            target_slots[slot] = regional["applied_db"][slot]
        else:
            target_slots[slot] = regional["proposed_db"][region]
    ladder = emit_ladder(regional["applied_db"], target_slots,
                         base_version=base_version)

    # T6: excursion verdict on the whole planned climb. A rung's
    # eq_bands_db IS the cumulative value the profile would carry, so
    # every rung is checked; the headline is the rung with the largest
    # excursion and first_breaching_rung is the first rung that
    # breaches (None when none does). With no ladder the check runs on
    # the currently applied (clamped) slots.
    if ladder:
        rung_checks = [check_cumulative_excursion(rung["eq_bands_db"])
                       for rung in ladder]
        worst = max(rung_checks, key=lambda c: c["max_excursion_db"])
        first_breach = next(
            (rung["version"] for rung, check in zip(ladder, rung_checks)
             if not check["ok"]),
            None)
        t6_excursion = worst
    else:
        t6_excursion = check_cumulative_excursion(regional["applied_db"])
        first_breach = None if t6_excursion["ok"] else "current"
    t6 = {
        "excursion": t6_excursion,
        "first_breaching_rung": first_breach,
        "halt_needs_human": not t6_excursion["ok"],
        "reason": (
            "planned climb stays within the +/-%.1f dB cumulative "
            "excursion cap (max %.2f dB)"
            % (CUMULATIVE_EXCURSION_CAP_DB, t6_excursion["max_excursion_db"])
            if t6_excursion["ok"] else
            "planned climb would breach the +/-%.1f dB cumulative "
            "excursion cap at rung %s: %s — STOP the cycle, do not "
            "publish that rung or any later one, ask Esse"
            % (CUMULATIVE_EXCURSION_CAP_DB, first_breach,
               t6_excursion["reason"])),
    }

    return {
        "regional": regional,
        "s2": s2,
        "coherence_guard": guard,
        "s10": s10,
        "split": split,
        "ladder": ladder,
        "t6": t6,
    }
