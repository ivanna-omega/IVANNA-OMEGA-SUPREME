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
    S9 (server side).

Baseline fixed by the specs: spectral error RMS 10.46 dB (v1.1 profile),
promotion target <= 8.37 dB (0.8 x 10.46).

Public API (all pure functions):
  * S2  -- :func:`fit_comb_vs_response` (comb vs response discrimination)
  * S6  -- :func:`split_cut_boost`, :func:`precheck_headroom`
  * S8  -- :func:`evaluate_ab`
  * S5  -- :func:`emit_ladder`
  * S9  -- :func:`coherence_guard`
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
    }


def evaluate_ab(base_sessions, candidate_sessions):
    """S8: evaluate a candidate over 3 sessions vs 3 base sessions.

    Each session is a dict with:
      ``spectral_error_rms_db`` (float), ``error_mean_db`` (24 floats),
      ``mean_coherence`` (float), ``level_max_dbfs`` (float, worst chunk),
      ``level_mean_dbfs`` (float); optional ``crest_db`` (float) and
      ``lufs`` (float, when that telemetry exists).

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

    Returns ``{"verdict": "promote" | "rollback", "gates": {...},
    "base": <summary>, "candidate": <summary>}``. Each gate entry is
    ``{"passed": bool, "detail": str}``.
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

    verdict = "promote" if all(g["passed"] for g in gates.values()) else "rollback"
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
# Pipeline: §2.3 regional correction + S9 guard + S6 split (+ S5 ladder)
# ---------------------------------------------------------------------------

def compile_candidate(error_mean_db, coherence_mean_db, mean_coherence,
                      erb_std_db=None, level_max_dbfs=-6.0,
                      exciter_amount=0.3, v1_1a_promoted=False,
                      room_corr_available=False, base_version="v1.1",
                      ab_measurement_only=False):
    """Full deterministic pipeline for one compiled profile table.

    1. §2.3 regional correction (deadband, clamp +/-2 dB, trim, residual).
    2. S2 comb-vs-response fit (informational; its verdict is reported,
       it does not by itself block emission -- S9 does that).
    3. S9 coherence guard on the per-band corrections implied by the
       regional correction (a region's correction is applied to its bands
       only where the guard allows it; a region with no allowed band
       keeps its correction as documented residual).
    4. S6 split into v1.1a (cut-only) / v1.1b (boost, gated on
       ``v1_1a_promoted``), each headroom pre-checked.
    5. S5 ladder for the remaining positive residual (rungs after the
       current version), so the server knows the whole climb in advance.

    ``coherence_mean_db`` is the 24-band per-band coherence list.
    Returns a dict with keys ``regional``, ``s2``, ``coherence_guard``,
    ``split`` and ``ladder``. Pure function; the server decides whether
    to publish the returned candidates.
    """
    regional = regional_correction(error_mean_db)
    s2 = fit_comb_vs_response(error_mean_db, erb_std_db)

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
    # has no per-band slot).
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
    proposed_slots = {}
    for slot in EQ_SLOTS:
        region = SLOT_REGION[slot]
        if region in blocked_regions:
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
    if guard["mode"] == "partial":
        split["notes"].append(
            "S9 guard is PARTIAL: some bands blocked; slot values are "
            "region-wide and cannot express per-band blocking, so the "
            "split above uses the region correction as proposed -- the "
            "server must treat this candidate as measurement-only until "
            "coherence rises"
        )

    # S5 ladder: from the currently applied (clamped) slots toward the
    # full proposed targets.
    target_slots = {slot: regional["proposed_db"][SLOT_REGION[slot]]
                    for slot in EQ_SLOTS}
    ladder = emit_ladder(regional["applied_db"], target_slots,
                         base_version=base_version)
    return {
        "regional": regional,
        "s2": s2,
        "coherence_guard": guard,
        "split": split,
        "ladder": ladder,
    }
