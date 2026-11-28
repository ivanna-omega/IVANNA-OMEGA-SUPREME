"""Tests for the deterministic MuseLink analyzer (release 510).

Standard library ``unittest`` only, no dependencies, no randomness, no
network. Synthetic data throughout, except for the v1.1 profile file
(``omega-1795857152818-9lec-v1.1.ivanna.json``), which is loaded where the
test says so.

Run from the module directory:
    python3 -m unittest test_analyzer -v
"""

import json
import math
import os
import unittest

from analyzer import (
    AB_REGIONS,
    APEX_CEILING_DBFS,
    BASELINE_RMS_DB,
    CHUNK_ERROR_DB_MAX,
    COHERENCE_GUARD,
    COMB_FIT_D_MAX_M,
    COMB_FIT_D_MIN_M,
    COMB_IMPROVEMENT_MIN,
    CUMULATIVE_EXCURSION_CAP_DB,
    D10_P0_TOLERANCE_DB,
    DEFAULT_MAX_SESSIONS_PER_HOUR,
    ERB_CENTER_HZ,
    EQ_CLAMP_DB,
    LADDER_STEP_DB,
    MAX_PROMOTIONS_PER_DAY,
    OUTLIER_CONFIRM_DB,
    OUTLIER_CONFIRM_SESSIONS,
    OUTLIER_DEVIATION_DB,
    REGIONS,
    S10_RESIDUAL_FREEZE_DB,
    TARGET_RMS_DB,
    check_cumulative_excursion,
    check_d10_p0_covariates,
    check_playing_evidence,
    check_promotion_brake,
    check_replay,
    check_residual_freeze,
    check_token_rate,
    classify_session_outlier,
    coherence_guard,
    compile_candidate,
    count_playing_chunks,
    detect_resonances,
    emit_ladder,
    evaluate_ab,
    fit_comb_vs_response,
    gate_promotion,
    gate_session,
    log_rejection,
    precheck_headroom,
    quarantine_update,
    region_rms,
    regional_correction,
    rejection_summary,
    split_cut_boost,
    validate_chunk,
)

SPEED_OF_SOUND_M_S = 343.0
V1_1_PATH = os.path.join(
    os.path.expanduser("~"), "workspace", "your_files",
    "omega-1795857152818-9lec-v1.1.ivanna.json",
)

# A 24-band error table whose regional means reproduce the v1.1 derivation
# exactly: graves error +3 dB, medios-graves +2, medios -4, transicion -7,
# brillo -9, presencia -9, aire -12. (V1.1 derivation: corrections -3, -2,
# +4, +9, +9 dB respectively; subgraves +1 has no band.)
V1_1_ERROR_DB = (
    [3.0]                    # ERB-1  graves
    + [2.0] * 5              # ERB-2..6  medios-graves
    + [-4.0] * 3             # ERB-7..9  medios
    + [-7.0] * 3             # ERB-10..12  transicion
    + [-9.0] * 4             # ERB-13..16  brillo
    + [-9.0] * 4             # ERB-17..20  presencia
    + [-12.0] * 4            # ERB-21..24  aire
)
assert len(V1_1_ERROR_DB) == 24


def _session(rms_db, error_db, coherence=0.0, level_max=-6.0,
             level_mean=-18.0, crest=12.0, lufs=None):
    return {
        "spectral_error_rms_db": rms_db,
        "error_mean_db": list(error_db),
        "mean_coherence": coherence,
        "level_max_dbfs": level_max,
        "level_mean_dbfs": level_mean,
        "crest_db": crest,
        "lufs": lufs,
    }


class TestS2CombVsResponse(unittest.TestCase):
    def _comb_table(self, distance_m, tilt_db_per_octave=-1.0):
        """Synthetic comb: smooth tilt + a 3 dB cosine comb at distance d,
        plus erbStdDb high at predicted nulls, low at predicted peaks."""
        tau = 2.0 * distance_m / SPEED_OF_SOUND_M_S
        error = []
        std = []
        for f in ERB_CENTER_HZ:
            cos_val = math.cos(2.0 * math.pi * f * tau)
            error.append(tilt_db_per_octave * math.log2(f / 1000.0)
                         + 3.0 * cos_val)
            std.append(3.0 if cos_val < -0.5 else 0.3)
        return error, std

    def test_comb_is_detected_and_distance_recovered(self):
        error, std = self._comb_table(2.0)
        result = fit_comb_vs_response(error, std)
        self.assertEqual(result["verdict"], "comb")
        self.assertIsNotNone(result["distance_m"])
        self.assertAlmostEqual(result["distance_m"], 2.0, delta=0.05)
        self.assertGreaterEqual(result["improvement"], 0.25)
        self.assertGreaterEqual(result["std_contrast_db"], 1.0)

    def test_smooth_response_is_not_a_comb(self):
        error = [-1.0 * math.log2(f / 1000.0) - 2.0 for f in ERB_CENTER_HZ]
        std = [0.3] * 24
        result = fit_comb_vs_response(error, std)
        self.assertEqual(result["verdict"], "response")
        self.assertLess(result["improvement"], 0.25)

    def test_missing_erb_std_db_is_indeterminate_not_guessed(self):
        error, _std = self._comb_table(2.0)
        result = fit_comb_vs_response(error, None)
        self.assertEqual(result["verdict"], "indeterminate")
        self.assertIn("erb_std_db_missing", result["reason"])
        # The fit still ran and published d + residual; only the
        # classification refuses to guess.
        self.assertIsNotNone(result["distance_m"])
        self.assertTrue(COMB_FIT_D_MIN_M <= result["distance_m"] <= COMB_FIT_D_MAX_M)

    def test_mixed_evidence_is_inconclusive(self):
        # Comb-shaped error but FLAT dispersion: the comb fit helps, yet
        # the dispersion does not follow the nulls, so neither verdict
        # is established.
        error, _std = self._comb_table(2.0)
        result = fit_comb_vs_response(error, [1.5] * 24)
        self.assertEqual(result["verdict"], "inconclusive")


class TestS6CutsFirst(unittest.TestCase):
    # v1.1 proposed (unclamped) corrections from its derivation block.
    V1_1_PROPOSED = {
        "low_60_120": -3.0,
        "mid_500_1k": 4.0,
        "high_2k_4k": 9.0,
        "presence_4k_8k": 9.0,
    }

    def test_v1_1a_is_cut_only_and_comes_first(self):
        split = split_cut_boost(self.V1_1_PROPOSED, level_max_dbfs=-6.0,
                                v1_1a_promoted=False)
        v1_1a = split["v1_1a"]
        self.assertIsNotNone(v1_1a)
        self.assertEqual(v1_1a["version"], "v1.1a")
        # Only the cut survives, clamped to -2 dB; no boost in v1.1a.
        self.assertEqual(v1_1a["eq_bands_db"], {"low_60_120": -2.0})
        # trim compensates only the cuts: -(-2.0) = +2.0, net gain 0.
        self.assertAlmostEqual(v1_1a["trim_db"], 2.0)
        self.assertTrue(v1_1a["precheck"]["ok"])

    def test_v1_1b_is_gated_on_v1_1a_promotion(self):
        split = split_cut_boost(self.V1_1_PROPOSED, level_max_dbfs=-6.0,
                                v1_1a_promoted=False)
        self.assertIsNone(split["v1_1b"])
        self.assertTrue(any("v1.1a has not promoted" in n for n in split["notes"]))

        split = split_cut_boost(self.V1_1_PROPOSED, level_max_dbfs=-6.0,
                                v1_1a_promoted=True)
        v1_1b = split["v1_1b"]
        self.assertIsNotNone(v1_1b)
        self.assertEqual(v1_1b["version"], "v1.1b")
        self.assertEqual(v1_1b["eq_bands_db"], {
            "mid_500_1k": 2.0, "high_2k_4k": 2.0, "presence_4k_8k": 2.0})
        self.assertAlmostEqual(v1_1b["trim_db"], -6.0)

    def test_v1_1a_without_s3_uses_only_low_slot(self):
        split = split_cut_boost(self.V1_1_PROPOSED, level_max_dbfs=-6.0,
                                room_corr_cut_db=-2.0, room_corr_available=False)
        self.assertIsNone(split["v1_1a"]["room_corr_db"])
        self.assertTrue(any("S3" in n for n in split["notes"]))

    def test_precheck_scales_down_boost_that_would_clip(self):
        # levelMax -1.0 dBFS + max boost +2.0 dB = +1.0 dBFS: violates
        # the -0.1 dBFS APEX ceiling. The boost must be scaled to +0.9.
        result = precheck_headroom(-1.0, {"high_2k_4k": 2.0, "presence_4k_8k": 2.0})
        self.assertTrue(result["ok"])
        self.assertAlmostEqual(result["max_boost_db"], 0.9)
        self.assertAlmostEqual(result["adjusted_slots_db"]["high_2k_4k"], 0.9)
        self.assertAlmostEqual(result["adjusted_slots_db"]["presence_4k_8k"], 0.9)
        self.assertTrue(result["notes"])

    def test_precheck_blocks_when_live_level_is_already_at_ceiling(self):
        result = precheck_headroom(0.0, {"high_2k_4k": 2.0}, exciter_amount=0.3)
        self.assertFalse(result["ok"])
        self.assertEqual(result["adjusted_exciter_amount"], 0.0)
        self.assertTrue(any("BLOCKED" in n for n in result["notes"]))

    def test_precheck_passes_cut_only_at_high_level(self):
        # Cut-only has maxBoost 0, so it passes whenever the live level
        # itself is below the ceiling.
        result = precheck_headroom(-0.5, {"low_60_120": -2.0})
        self.assertTrue(result["ok"])


class TestS8HardenedGates(unittest.TestCase):
    def _base_sessions(self):
        return [_session(BASELINE_RMS_DB, V1_1_ERROR_DB) for _ in range(3)]

    def _candidate_error(self, presencia_db=-9.0):
        err = list(V1_1_ERROR_DB)
        for i in range(16, 20):  # ERB-17..20 presencia
            err[i] = presencia_db
        return err

    def test_global_improvement_but_region_worsens_is_rollback(self):
        # Global RMS improves 28% (10.46 -> 7.50 dB) but presencia
        # worsens from 9.0 to 10.5 dB RMS (+1.5 dB > 1 dB limit).
        cand = [_session(7.50, self._candidate_error(presencia_db=-10.5))
                for _ in range(3)]
        result = evaluate_ab(self._base_sessions(), cand)
        self.assertEqual(result["verdict"], "rollback")
        self.assertTrue(result["gates"]["erb_improvement"]["passed"])
        self.assertFalse(result["gates"]["region_rms"]["passed"])
        self.assertIn("presencia", result["gates"]["region_rms"]["detail"])

    def test_region_worsening_within_1db_does_not_block(self):
        cand = [_session(7.50, self._candidate_error(presencia_db=-9.8))
                for _ in range(3)]
        result = evaluate_ab(self._base_sessions(), cand)
        self.assertTrue(result["gates"]["region_rms"]["passed"])
        self.assertEqual(result["verdict"], "promote")

    def test_all_gates_pass_promotes(self):
        # Candidate improves every region: halve every regional error.
        improved = [e * 0.5 for e in V1_1_ERROR_DB]
        cand = [_session(5.23, improved) for _ in range(3)]
        result = evaluate_ab(self._base_sessions(), cand)
        self.assertEqual(result["verdict"], "promote")
        for gate in result["gates"].values():
            self.assertTrue(gate["passed"], gate)

    def test_apex_working_harder_blocks(self):
        improved = [e * 0.5 for e in V1_1_ERROR_DB]
        # Crest collapses 12 -> 10 dB: the limiter is eating the signal.
        cand = [_session(5.23, improved, crest=10.0) for _ in range(3)]
        result = evaluate_ab(self._base_sessions(), cand)
        self.assertEqual(result["verdict"], "rollback")
        self.assertFalse(result["gates"]["apex_not_working_harder"]["passed"])

    def test_levelmax_pegged_at_ceiling_blocks(self):
        improved = [e * 0.5 for e in V1_1_ERROR_DB]
        cand = [_session(5.23, improved, level_max=APEX_CEILING_DBFS)
                for _ in range(3)]
        result = evaluate_ab(self._base_sessions(), cand)
        self.assertEqual(result["verdict"], "rollback")
        self.assertFalse(result["gates"]["apex_not_working_harder"]["passed"])

    def test_loudness_mismatch_blocks(self):
        improved = [e * 0.5 for e in V1_1_ERROR_DB]
        cand = [_session(5.23, improved, level_mean=-13.0) for _ in range(3)]
        result = evaluate_ab(self._base_sessions(), cand)
        self.assertEqual(result["verdict"], "rollback")
        self.assertFalse(result["gates"]["loudness_match"]["passed"])

    def test_coherence_gate_kept_and_discriminating(self):
        improved = [e * 0.5 for e in V1_1_ERROR_DB]
        base = [_session(BASELINE_RMS_DB, V1_1_ERROR_DB, coherence=0.60)
                for _ in range(3)]
        cand = [_session(5.23, improved, coherence=0.50) for _ in range(3)]
        result = evaluate_ab(base, cand)
        self.assertFalse(result["gates"]["coherence"]["passed"])
        self.assertEqual(result["verdict"], "rollback")

    def test_no_new_resonance_blocks(self):
        improved = [e * 0.5 for e in V1_1_ERROR_DB]
        improved[2] = 8.0  # ERB-3 becomes a +8 dB resonance in candidate
        cand = [_session(5.23, improved) for _ in range(3)]
        result = evaluate_ab(self._base_sessions(), cand)
        self.assertFalse(result["gates"]["no_new_resonance"]["passed"])
        self.assertEqual(result["verdict"], "rollback")


class TestS5Ladder(unittest.TestCase):
    def test_seven_db_residual_closes_in_two_db_rungs(self):
        current = {"low_60_120": -2.0, "mid_500_1k": 2.0,
                   "high_2k_4k": 2.0, "presence_4k_8k": 2.0}
        target = {"low_60_120": -2.0, "mid_500_1k": 2.0,
                  "high_2k_4k": 9.0, "presence_4k_8k": 9.0}
        rungs = emit_ladder(current, target, base_version="v1.1")
        self.assertEqual([r["version"] for r in rungs],
                         ["v1.2", "v1.3", "v1.4", "v1.5"])
        # Each rung moves brillo/presencia by exactly +2 dB, last +1 dB.
        self.assertEqual(rungs[0]["eq_bands_db"]["high_2k_4k"], 4.0)
        self.assertEqual(rungs[1]["eq_bands_db"]["high_2k_4k"], 6.0)
        self.assertEqual(rungs[2]["eq_bands_db"]["high_2k_4k"], 8.0)
        self.assertEqual(rungs[3]["eq_bands_db"]["high_2k_4k"], 9.0)
        self.assertEqual(rungs[3]["residual_remaining_db"], {})
        for rung in rungs:
            self.assertTrue(rung["requires_ab"])
            self.assertTrue(rung["reversible"])
            # Net gain stays 0.0 dB at every rung.
            self.assertAlmostEqual(
                sum(rung["eq_bands_db"].values()) + rung["trim_db"], 0.0)
        # No rung ever jumps more than the +2 dB step.
        prev = current
        for rung in rungs:
            for slot in ("high_2k_4k", "presence_4k_8k"):
                self.assertLessEqual(
                    rung["eq_bands_db"][slot] - prev[slot],
                    LADDER_STEP_DB + 1e-9)
            prev = rung["eq_bands_db"]

    def test_no_residual_means_no_rungs(self):
        current = {"low_60_120": -2.0, "mid_500_1k": 2.0}
        self.assertEqual(emit_ladder(current, dict(current)), [])


class TestS9CoherenceGuard(unittest.TestCase):
    def test_zero_coherence_emits_nothing(self):
        corrections = {3: -2.0, 13: 9.0, 17: 9.0}
        result = coherence_guard(0.0, [0.0] * 24, corrections)
        self.assertEqual(result["mode"], "none")
        self.assertEqual(result["allowed"], {})
        self.assertEqual(result["blocked"], corrections)

    def test_zero_coherence_with_measurement_mark_is_measurement_only(self):
        corrections = {3: -2.0, 13: 9.0, 17: 9.0}
        result = coherence_guard(0.0, [0.0] * 24, corrections,
                                 ab_measurement_only=True)
        self.assertEqual(result["mode"], "measurement_only")
        self.assertEqual(result["allowed"], corrections)
        self.assertEqual(result["blocked"], {})

    def test_only_coherent_bands_are_allowed(self):
        per_band = [0.0] * 24
        per_band[2] = 0.5   # ERB-3 coherent
        per_band[12] = 0.6  # ERB-13 coherent
        corrections = {3: -2.0, 13: 9.0, 17: 9.0}
        result = coherence_guard(0.10, per_band, corrections)
        self.assertEqual(result["mode"], "partial")
        self.assertEqual(result["allowed"], {3: -2.0, 13: 9.0})
        self.assertEqual(result["blocked"], {17: 9.0})

    def test_coherence_above_guard_allows_everything(self):
        result = coherence_guard(0.5, [0.5] * 24, {3: -2.0, 13: 9.0})
        self.assertEqual(result["mode"], "full")

    def test_no_per_band_coherence_blocks_everything(self):
        result = coherence_guard(0.10, None, {3: -2.0})
        self.assertEqual(result["mode"], "none")
        self.assertEqual(result["allowed"], {})


class TestRegionalCorrectionV11(unittest.TestCase):
    def test_v1_1_regional_correction_reproduces_the_profile_file(self):
        corr = regional_correction(V1_1_ERROR_DB)
        # Applied slots (clamped +/-2 dB) match the v1.1 engine block.
        self.assertEqual(corr["applied_db"], {
            "low_60_120": -2.0, "mid_500_1k": 2.0,
            "high_2k_4k": 2.0, "presence_4k_8k": 2.0})
        # Residuals match the v1.1 derivation block.
        self.assertAlmostEqual(corr["residual_db"]["graves"], -1.0)
        self.assertAlmostEqual(corr["residual_db"]["medios"], 2.0)
        self.assertAlmostEqual(corr["residual_db"]["brillo"], 7.0)
        self.assertAlmostEqual(corr["residual_db"]["presencia"], 7.0)
        # Unslotted regions are documented, never applied.
        self.assertAlmostEqual(corr["unslotted_db"]["medios_graves"], -2.0)
        # trim -4.0 dB compensates the +4.0 dB slot sum (net gain 0).
        self.assertAlmostEqual(corr["trim_db"], -4.0)

    def test_resonance_detection_finds_erb3(self):
        error = list(V1_1_ERROR_DB)
        error[2] = 9.86  # ERB-3 209 Hz, the verified resonance
        self.assertEqual(detect_resonances(error), [3])

    def test_deadband_ignores_sub_1db_error(self):
        error = [0.5] * 24
        corr = regional_correction(error)
        self.assertEqual(corr["applied_db"], {
            "low_60_120": 0.0, "mid_500_1k": 0.0,
            "high_2k_4k": 0.0, "presence_4k_8k": 0.0})
        self.assertAlmostEqual(corr["trim_db"], 0.0)


class TestV11ProfileFile(unittest.TestCase):
    def test_v1_1_file_drives_s6_and_s9(self):
        with open(V1_1_PATH, encoding="utf-8") as fh:
            profile = json.load(fh)
        self.assertAlmostEqual(
            profile["fingerprint"]["spectral_error_rms_db"], BASELINE_RMS_DB)
        self.assertAlmostEqual(profile["fingerprint"]["mean_coherence"], 0.0)
        # S9: with the file's 0.0 coherence, nothing is emitted for
        # auto-application (unless marked measurement-only).
        guard = coherence_guard(
            profile["fingerprint"]["mean_coherence"], [0.0] * 24,
            {3: -2.0, 13: 9.0, 17: 9.0})
        self.assertEqual(guard["mode"], "none")
        guard_m = coherence_guard(
            profile["fingerprint"]["mean_coherence"], [0.0] * 24,
            {3: -2.0, 13: 9.0, 17: 9.0}, ab_measurement_only=True)
        self.assertEqual(guard_m["mode"], "measurement_only")
        # S6: the file's engine block is the clamped v1.1; the split of
        # its unclamped proposal yields v1.1a cut-only first.
        split = split_cut_boost(
            {"low_60_120": -3.0, "mid_500_1k": 4.0,
             "high_2k_4k": 9.0, "presence_4k_8k": 9.0},
            level_max_dbfs=-6.0, v1_1a_promoted=False)
        self.assertEqual(split["v1_1a"]["eq_bands_db"], {"low_60_120": -2.0})
        self.assertIsNone(split["v1_1b"])

    def test_target_rms_is_80_percent_of_baseline(self):
        self.assertAlmostEqual(TARGET_RMS_DB, 0.8 * BASELINE_RMS_DB, places=2)


class TestCompileCandidatePipeline(unittest.TestCase):
    def test_pipeline_with_zero_coherence_blocks_and_still_plans(self):
        out = compile_candidate(
            V1_1_ERROR_DB, [0.0] * 24, 0.0,
            erb_std_db=None, level_max_dbfs=-6.0,
            ab_measurement_only=False)
        # S2 cannot classify without erbStdDb (pending server data).
        self.assertEqual(out["s2"]["verdict"], "indeterminate")
        # S9 blocks every correction for auto-application.
        self.assertEqual(out["coherence_guard"]["mode"], "none")
        self.assertIsNone(out["split"]["v1_1a"])
        # S10 freezes brillo/presencia (residual +7 dB) at coherence
        # 0%: EVO-003 is the next required step, pending the S1
        # reference fix. (transicion/aire also trigger — unslotted,
        # so they freeze no slot.)
        self.assertEqual(out["s10"]["frozen_regions"],
                         ["transicion", "brillo", "presencia", "aire"])
        self.assertEqual(out["s10"]["frozen_slots"],
                         ["high_2k_4k", "presence_4k_8k"])
        self.assertTrue(out["s10"]["evo003_required"])
        self.assertTrue(out["s10"]["evo003_pending_reference_fix"])
        # The S5 ladder plans only the UNFROZEN climb (low -3, mid +4):
        # one rung; brillo/presencia stay at their clamped +2 dB.
        self.assertEqual(len(out["ladder"]), 1)
        self.assertEqual(out["ladder"][0]["version"], "v1.2")
        self.assertEqual(out["ladder"][0]["eq_bands_db"], {
            "low_60_120": -3.0, "mid_500_1k": 4.0,
            "high_2k_4k": 2.0, "presence_4k_8k": 2.0})

    def test_pipeline_with_good_coherence_emits_v1_1a(self):
        out = compile_candidate(
            V1_1_ERROR_DB, [0.9] * 24, 0.9,
            erb_std_db=[0.3] * 24, level_max_dbfs=-6.0)
        self.assertEqual(out["coherence_guard"]["mode"], "full")
        self.assertEqual(out["split"]["v1_1a"]["eq_bands_db"],
                         {"low_60_120": -2.0})
        self.assertIsNone(out["split"]["v1_1b"])  # v1.1a not yet promoted


class TestT7ValidateChunk(unittest.TestCase):
    """T7 rule 1: physically impossible chunk values are rejected.

    All anomalous chunks below are SYNTHETIC (hand-built to violate one
    bound); the valid chunk is the v1.1 synthetic error table plus
    plausible telemetry."""

    def _chunk(self, **overrides):
        chunk = {
            "error_mean_db": list(V1_1_ERROR_DB),
            "magnitude_db": [-20.0] * 24,
            "coherence": [0.5] * 24,
            "mean_coherence": 0.5,
            "level_max_dbfs": -6.0,
            "level_mean_dbfs": -18.0,
            "spectral_error_rms_db": 10.46,
            "erb_std_db": [0.3] * 24,
            "crest_db": 12.0,
            "lufs": -18.0,
        }
        chunk.update(overrides)
        return chunk

    def test_valid_chunk_passes(self):
        self.assertTrue(validate_chunk(self._chunk())["ok"])

    def test_only_error_table_is_required(self):
        self.assertTrue(validate_chunk(
            {"error_mean_db": list(V1_1_ERROR_DB)})["ok"])

    def test_nan_error_is_rejected(self):
        chunk = self._chunk()
        chunk["error_mean_db"][4] = float("nan")
        result = validate_chunk(chunk)
        self.assertFalse(result["ok"])
        self.assertTrue(any("ERB-5" in r and "finite" in r
                            for r in result["reasons"]))

    def test_infinite_magnitude_is_rejected(self):
        chunk = self._chunk()
        chunk["magnitude_db"][0] = float("inf")
        self.assertFalse(validate_chunk(chunk)["ok"])

    def test_error_beyond_plus_40_db_is_rejected_with_band(self):
        chunk = self._chunk()
        chunk["error_mean_db"][4] = 45.0  # ERB-5, +45 dB: impossible
        result = validate_chunk(chunk)
        self.assertFalse(result["ok"])
        self.assertTrue(any("ERB-5" in r and "45.00" in r
                            for r in result["reasons"]))

    def test_error_below_minus_40_db_is_rejected(self):
        chunk = self._chunk(error_mean_db=[-41.0] * 24)
        self.assertFalse(validate_chunk(chunk)["ok"])

    def test_magnitude_above_0_dbfs_is_rejected(self):
        chunk = self._chunk(magnitude_db=[3.0] * 24)
        result = validate_chunk(chunk)
        self.assertFalse(result["ok"])
        self.assertTrue(any("magnitude_db" in r for r in result["reasons"]))

    def test_coherence_above_1_is_rejected(self):
        chunk = self._chunk(coherence=[1.2] * 24)
        self.assertFalse(validate_chunk(chunk)["ok"])

    def test_level_max_above_0_dbfs_is_rejected(self):
        chunk = self._chunk(level_max_dbfs=0.5)
        result = validate_chunk(chunk)
        self.assertFalse(result["ok"])
        self.assertTrue(any("level_max_dbfs" in r for r in result["reasons"]))

    def test_wrong_band_count_is_rejected(self):
        chunk = self._chunk(error_mean_db=[0.0] * 23)
        result = validate_chunk(chunk)
        self.assertFalse(result["ok"])
        self.assertTrue(any("23 bands" in r for r in result["reasons"]))

    def test_missing_error_table_is_rejected(self):
        self.assertFalse(validate_chunk({"level_max_dbfs": -6.0})["ok"])

    def test_negative_crest_is_rejected(self):
        self.assertFalse(validate_chunk(self._chunk(crest_db=-1.0))["ok"])


class TestT7TokenRate(unittest.TestCase):
    def test_under_limit_is_allowed(self):
        now = 1_000_000
        prior = [now - 600 * i for i in range(5)]  # 5 in the last hour
        result = check_token_rate(prior, now)
        self.assertTrue(result["allowed"])
        self.assertEqual(result["count_in_window"], 5)

    def test_at_limit_is_rejected(self):
        now = 1_000_000
        prior = [now - 600 * i for i in range(DEFAULT_MAX_SESSIONS_PER_HOUR)]
        result = check_token_rate(prior, now)
        self.assertFalse(result["allowed"])
        self.assertIn("rate limit exceeded", result["reason"])

    def test_timestamps_older_than_one_hour_do_not_count(self):
        now = 1_000_000
        prior = [now - 4000] * 10  # 10 sessions, all > 1 h ago
        result = check_token_rate(prior, now)
        self.assertTrue(result["allowed"])
        self.assertEqual(result["count_in_window"], 0)

    def test_limit_is_a_parameter_not_a_buried_constant(self):
        now = 1_000_000
        prior = [now - 100, now - 200]
        self.assertTrue(check_token_rate(prior, now, max_sessions_per_hour=3)["allowed"])
        self.assertFalse(check_token_rate(prior, now, max_sessions_per_hour=2)["allowed"])


class TestT7Replay(unittest.TestCase):
    def test_fresh_session_passes(self):
        result = check_replay("s-1", {"s-0"}, list(V1_1_ERROR_DB),
                              [list(V1_1_ERROR_DB)])
        # Same table as a seen one -> replay even with a fresh id.
        self.assertFalse(result["ok"])
        result = check_replay("s-1", {"s-0"}, [0.0] * 24,
                              [list(V1_1_ERROR_DB)])
        self.assertTrue(result["ok"])

    def test_repeated_session_id_is_replay(self):
        result = check_replay("s-1", {"s-1", "s-0"})
        self.assertFalse(result["ok"])
        self.assertIn("already accepted", result["reason"])

    def test_identical_error_table_is_replay(self):
        result = check_replay("s-9", {"s-0"}, list(V1_1_ERROR_DB),
                              [list(V1_1_ERROR_DB)])
        self.assertFalse(result["ok"])
        self.assertIn("identical", result["reason"])

    def test_no_tracking_state_means_nothing_to_object_to(self):
        self.assertTrue(check_replay(None, set())["ok"])


class TestT7OutlierHold(unittest.TestCase):
    AGGREGATE = V1_1_ERROR_DB

    def _deviating(self, band, deviation_db):
        """Synthetic session: aggregate with one band shifted."""
        err = list(self.AGGREGATE)
        err[band - 1] += deviation_db
        return err

    def test_session_within_threshold_is_accepted(self):
        result = classify_session_outlier(self._deviating(5, 5.0),
                                          self.AGGREGATE)
        self.assertEqual(result["status"], "accepted")

    def test_deviation_exactly_at_threshold_is_accepted(self):
        result = classify_session_outlier(
            self._deviating(5, OUTLIER_DEVIATION_DB), self.AGGREGATE)
        self.assertEqual(result["status"], "accepted")

    def test_session_beyond_threshold_is_held_with_band_and_reason(self):
        result = classify_session_outlier(self._deviating(5, 8.0),
                                          self.AGGREGATE)
        self.assertEqual(result["status"], "held")
        self.assertEqual(result["deviating_bands"], {5: 8.0})
        self.assertAlmostEqual(result["max_deviation_db"], 8.0)
        self.assertIn("ERB-5", result["reason"])

    def test_no_aggregate_means_accepted(self):
        result = classify_session_outlier(self._deviating(5, 30.0), None)
        self.assertEqual(result["status"], "accepted")

    def test_two_confirmations_release_the_held_session(self):
        state = {"held": []}
        # S1 deviates +8 dB in ERB-5 -> held.
        state, d1 = quarantine_update(state, "s1", self._deviating(5, 8.0),
                                      self.AGGREGATE)
        self.assertEqual(d1["status"], "held")
        self.assertEqual(len(state["held"]), 1)
        # S2 deviates +5 dB, same band and sign: accepted itself (<= 6 dB)
        # and counts as confirmation 1 for S1.
        state, d2 = quarantine_update(state, "s2", self._deviating(5, 5.0),
                                      self.AGGREGATE)
        self.assertEqual(d2["status"], "accepted")
        self.assertEqual(state["held"][0]["confirmations"], 1)
        self.assertEqual(d2["released_sessions"], [])
        # S3 deviates +4 dB, same band and sign: confirmation 2 -> S1 released.
        state, d3 = quarantine_update(state, "s3", self._deviating(5, 4.0),
                                      self.AGGREGATE)
        self.assertEqual(len(state["held"]), 0)
        self.assertEqual(len(d3["released_sessions"]), 1)
        self.assertEqual(d3["released_sessions"][0]["session_id"], "s1")
        self.assertEqual(d3["released_sessions"][0]["error_mean_db"],
                         self._deviating(5, 8.0))

    def test_no_confirmation_in_two_sessions_discards(self):
        state = {"held": []}
        state, _ = quarantine_update(state, "s1", self._deviating(5, 8.0),
                                     self.AGGREGATE)
        # Two ordinary sessions follow: no confirmation either time.
        state, _ = quarantine_update(state, "s2", list(self.AGGREGATE),
                                     self.AGGREGATE)
        self.assertEqual(len(state["held"]), 1)
        state, d3 = quarantine_update(state, "s3", list(self.AGGREGATE),
                                      self.AGGREGATE)
        self.assertEqual(len(state["held"]), 0)
        self.assertEqual(d3["discarded_session_ids"], ["s1"])
        self.assertEqual(d3["released_sessions"], [])

    def test_opposite_sign_does_not_confirm(self):
        state = {"held": []}
        state, _ = quarantine_update(state, "s1", self._deviating(5, 8.0),
                                     self.AGGREGATE)
        # Two sessions deviate the OTHER way in ERB-5: same magnitude
        # band, opposite sign -- not a confirmation of S1.
        state, _ = quarantine_update(state, "s2", self._deviating(5, -5.0),
                                     self.AGGREGATE)
        self.assertEqual(state["held"][0]["confirmations"], 0)
        state, d3 = quarantine_update(state, "s3", self._deviating(5, -5.0),
                                      self.AGGREGATE)
        self.assertEqual(d3["discarded_session_ids"], ["s1"])

    def test_confirmation_below_half_threshold_does_not_count(self):
        state = {"held": []}
        state, _ = quarantine_update(state, "s1", self._deviating(5, 8.0),
                                     self.AGGREGATE)
        # +2 dB in ERB-5 is the same direction but below the 3 dB
        # confirmation bar (OUTLIER_CONFIRM_DB).
        state, _ = quarantine_update(state, "s2", self._deviating(5, 2.0),
                                     self.AGGREGATE)
        self.assertEqual(state["held"][0]["confirmations"], 0)

    def test_one_confirmation_then_silence_discards(self):
        state = {"held": []}
        state, _ = quarantine_update(state, "s1", self._deviating(5, 8.0),
                                     self.AGGREGATE)
        state, _ = quarantine_update(state, "s2", self._deviating(5, 5.0),
                                     self.AGGREGATE)
        self.assertEqual(state["held"][0]["confirmations"], 1)
        state, d3 = quarantine_update(state, "s3", list(self.AGGREGATE),
                                      self.AGGREGATE)
        self.assertEqual(d3["discarded_session_ids"], ["s1"])

    def test_quarantine_does_not_mutate_its_input(self):
        state = {"held": []}
        state1, _ = quarantine_update(state, "s1", self._deviating(5, 8.0),
                                      self.AGGREGATE)
        self.assertEqual(state, {"held": []})
        self.assertEqual(len(state1["held"]), 1)


class TestT7GateSession(unittest.TestCase):
    NOW = 1_000_000

    def _session(self, session_id, error_db=None, **overrides):
        session = {
            "session_id": session_id,
            "error_mean_db": list(error_db if error_db is not None
                                  else V1_1_ERROR_DB),
            "mean_coherence": 0.5,
            "level_max_dbfs": -6.0,
            "level_mean_dbfs": -18.0,
            "spectral_error_rms_db": 10.46,
        }
        session.update(overrides)
        return session

    def _gate(self, session, **kwargs):
        kwargs.setdefault("aggregate_error_db", V1_1_ERROR_DB)
        kwargs.setdefault("token_history", [])
        kwargs.setdefault("now_epoch", self.NOW)
        kwargs.setdefault("quarantine_state", {"held": []})
        kwargs.setdefault("rejection_log", [])
        return gate_session(session, **kwargs)

    def test_clean_session_is_accepted_and_log_stays_empty(self):
        out = self._gate(self._session("s1"))
        self.assertEqual(out["decision"]["status"], "accepted")
        self.assertEqual(out["rejection_log"], [])
        self.assertEqual(out["quarantine_state"], {"held": []})

    def test_out_of_range_chunk_is_rejected_with_reason_logged(self):
        bad = self._session("s1")
        bad["error_mean_db"][4] = 45.0
        out = self._gate(bad)
        self.assertEqual(out["decision"]["status"], "rejected")
        self.assertIn("chunk_out_of_range", out["decision"]["reason"])
        self.assertEqual(len(out["rejection_log"]), 1)
        entry = out["rejection_log"][0]
        self.assertEqual(entry["rule"], "chunk_out_of_range")
        self.assertEqual(entry["decision"], "rejected")
        self.assertEqual(entry["session_id"], "s1")
        self.assertIn("ERB-5", entry["reason"])

    def test_replayed_session_is_rejected_and_logged(self):
        session = self._session("s1", error_db=[0.0] * 24)
        out = self._gate(session, seen_session_ids={"s1"})
        self.assertEqual(out["decision"]["status"], "rejected")
        self.assertEqual(out["rejection_log"][0]["rule"], "replay")

    def test_rate_limited_session_is_rejected_and_logged(self):
        history = [self.NOW - 600 * i
                   for i in range(DEFAULT_MAX_SESSIONS_PER_HOUR)]
        out = self._gate(self._session("s1"), token_history=history)
        self.assertEqual(out["decision"]["status"], "rejected")
        self.assertEqual(out["rejection_log"][0]["rule"], "rate_limit")

    def test_outlier_session_is_held_not_rejected_and_logged_as_held(self):
        err = list(V1_1_ERROR_DB)
        err[4] += 8.0  # ERB-5 +8 dB vs aggregate
        out = self._gate(self._session("s1", error_db=err))
        self.assertEqual(out["decision"]["status"], "held")
        self.assertEqual(len(out["quarantine_state"]["held"]), 1)
        self.assertEqual(len(out["rejection_log"]), 1)
        self.assertEqual(out["rejection_log"][0]["rule"], "outlier_hold")
        self.assertEqual(out["rejection_log"][0]["decision"], "held")

    def test_held_session_is_released_after_two_confirmations_via_gate(self):
        err = list(V1_1_ERROR_DB)
        err[4] += 8.0
        out = self._gate(self._session("s1", error_db=err))
        state, log = out["quarantine_state"], out["rejection_log"]
        err2 = list(V1_1_ERROR_DB)
        err2[4] += 5.0
        out = self._gate(self._session("s2", error_db=err2),
                         quarantine_state=state, rejection_log=log)
        self.assertEqual(out["decision"]["status"], "accepted")
        state, log = out["quarantine_state"], out["rejection_log"]
        err3 = list(V1_1_ERROR_DB)
        err3[4] += 4.0
        out = self._gate(self._session("s3", error_db=err3),
                         quarantine_state=state, rejection_log=log)
        self.assertEqual(
            [r["session_id"] for r in out["decision"]["released_sessions"]],
            ["s1"])

    def test_rejection_summary_counts_by_rule(self):
        log = []
        log = log_rejection(log, "a", "chunk_out_of_range", "bad value")
        log = log_rejection(log, "b", "rate_limit", "too many")
        log = log_rejection(log, "c", "rate_limit", "too many")
        log = log_rejection(log, "d", "outlier_hold", "held", decision="held")
        summary = rejection_summary(log)
        self.assertEqual(summary["total"], 4)
        self.assertEqual(summary["by_rule"],
                         {"chunk_out_of_range": 1, "rate_limit": 2,
                          "outlier_hold": 1})
        self.assertEqual(summary["by_decision"],
                         {"rejected": 3, "held": 1})

    def test_gate_does_not_mutate_input_state(self):
        state = {"held": []}
        log = []
        self._gate(self._session("s1"), quarantine_state=state,
                   rejection_log=log)
        self.assertEqual(state, {"held": []})
        self.assertEqual(log, [])


class TestT6CumulativeExcursion(unittest.TestCase):
    """T6 rule (a): cumulative excursion per slot vs the factory baseline.

    The cap is the proposal's own example (+/-4 dB, "p. ej. ±4 dB"),
    taken as the explicit default until Esse confirms or changes it.
    """

    def test_within_cap_is_ok(self):
        result = check_cumulative_excursion(
            {"low_60_120": -2.0, "mid_500_1k": 2.0,
             "high_2k_4k": 3.5, "presence_4k_8k": -3.5})
        self.assertTrue(result["ok"])
        self.assertEqual(result["max_excursion_db"], 3.5)
        self.assertEqual(result["exceeded_slots"], [])

    def test_exactly_at_the_cap_is_allowed(self):
        result = check_cumulative_excursion(
            {"high_2k_4k": 4.0, "presence_4k_8k": -4.0})
        self.assertTrue(result["ok"])
        self.assertEqual(result["max_excursion_db"], 4.0)

    def test_just_beyond_the_cap_breaches_and_names_the_slot(self):
        result = check_cumulative_excursion(
            {"low_60_120": -3.0, "high_2k_4k": 4.01})
        self.assertFalse(result["ok"])
        self.assertEqual(result["exceeded_slots"], ["high_2k_4k"])
        self.assertIn("high_2k_4k", result["reason"])
        self.assertIn("+4.01", result["reason"])

    def test_negative_breach_counts_the_same(self):
        result = check_cumulative_excursion({"low_60_120": -6.0})
        self.assertFalse(result["ok"])
        self.assertEqual(result["exceeded_slots"], ["low_60_120"])

    def test_empty_slots_and_unknown_slots_are_ignored(self):
        self.assertTrue(check_cumulative_excursion({})["ok"])
        self.assertTrue(
            check_cumulative_excursion({"room_corr_db": 9.0})["ok"])

    def test_cap_is_a_parameter_not_a_buried_constant(self):
        self.assertTrue(
            check_cumulative_excursion({"high_2k_4k": 6.0},
                                       cap_db=6.0)["ok"])
        self.assertFalse(
            check_cumulative_excursion({"high_2k_4k": 6.0},
                                       cap_db=5.0)["ok"])


class TestT6PromotionBrake(unittest.TestCase):
    """T6 rule (a): at most one promotion per calendar day."""

    def test_no_promotion_today_is_allowed(self):
        result = check_promotion_brake(0)
        self.assertTrue(result["allowed"])
        self.assertEqual(result["max_promotions_per_day"],
                         MAX_PROMOTIONS_PER_DAY)

    def test_one_promotion_today_blocks_the_second(self):
        result = check_promotion_brake(1)
        self.assertFalse(result["allowed"])
        self.assertIn("wait until tomorrow", result["reason"])

    def test_negative_count_is_a_caller_bug(self):
        with self.assertRaises(ValueError):
            check_promotion_brake(-1)


class TestT6GatePromotion(unittest.TestCase):
    """T6 entry point: both rules together, before publishing."""

    def test_clean_promotion_is_allowed_without_halt(self):
        result = gate_promotion({"high_2k_4k": 2.0}, promotions_today=0)
        self.assertTrue(result["allowed"])
        self.assertFalse(result["halt_needs_human"])
        self.assertEqual(result["reasons"], [])

    def test_excursion_breach_halts_the_cycle(self):
        result = gate_promotion({"high_2k_4k": 6.0}, promotions_today=0)
        self.assertFalse(result["allowed"])
        self.assertTrue(result["halt_needs_human"])
        self.assertFalse(result["excursion"]["ok"])
        self.assertTrue(result["promotion_brake"]["allowed"])

    def test_brake_alone_waits_without_halting(self):
        result = gate_promotion({"high_2k_4k": 2.0}, promotions_today=1)
        self.assertFalse(result["allowed"])
        self.assertFalse(result["halt_needs_human"])
        self.assertTrue(result["excursion"]["ok"])
        self.assertFalse(result["promotion_brake"]["allowed"])

    def test_both_failures_are_reported_together(self):
        result = gate_promotion({"high_2k_4k": 8.0}, promotions_today=2)
        self.assertFalse(result["allowed"])
        self.assertTrue(result["halt_needs_human"])
        self.assertEqual(len(result["reasons"]), 2)


class TestT6CompileCandidate(unittest.TestCase):
    """T6 verdict on the whole planned climb (compile_candidate "t6")."""

    def test_v1_1_climb_breaches_the_cap_at_rung_v1_3(self):
        # The v1.1 ladder as planned: v1.2 sits exactly at the cap
        # (+/-4.0 dB), v1.3 walks brillo/presencia to +6 dB. The gate
        # says the climb stops there unless Esse raises the cap.
        out = compile_candidate(
            V1_1_ERROR_DB, [0.9] * 24, 0.9,
            erb_std_db=[0.3] * 24, level_max_dbfs=-6.0)
        t6 = out["t6"]
        self.assertTrue(t6["halt_needs_human"])
        self.assertEqual(t6["first_breaching_rung"], "v1.3")
        self.assertEqual(t6["excursion"]["max_excursion_db"], 9.0)
        self.assertEqual(t6["excursion"]["cap_db"],
                         CUMULATIVE_EXCURSION_CAP_DB)

    def test_small_correction_stays_within_the_cap(self):
        # -1.5 dB error everywhere: proposed +1.5 dB per slot, no
        # ladder left to climb, excursion 1.5 dB — no halt.
        out = compile_candidate(
            [-1.5] * 24, [0.9] * 24, 0.9,
            erb_std_db=[0.3] * 24, level_max_dbfs=-6.0)
        t6 = out["t6"]
        self.assertFalse(t6["halt_needs_human"])
        self.assertIsNone(t6["first_breaching_rung"])
        self.assertEqual(t6["excursion"]["max_excursion_db"], 1.5)


class TestS10ResidualFreeze(unittest.TestCase):
    """S10: residual > 4 dB -> diagnose before climbing further.

    The triggering case is the v1.1 data itself: residual +7 dB in
    presencia/brillo with coherence 0%."""

    def test_v1_1_plus7_residual_with_zero_coherence_freezes(self):
        corr = regional_correction(V1_1_ERROR_DB)
        result = check_residual_freeze(corr, mean_coherence=0.0)
        self.assertIn("brillo", result["frozen_regions"])
        self.assertIn("presencia", result["frozen_regions"])
        self.assertEqual(result["frozen_slots"],
                         ["high_2k_4k", "presence_4k_8k"])
        self.assertTrue(result["evo003_required"])
        # Coherence 0% is instrumentation (S1), so the residual cannot
        # be trusted as a room deficit: the EVO-003 firing itself stays
        # pending the S1 reference fix.
        self.assertTrue(result["evo003_pending_reference_fix"])
        self.assertFalse(result["released"])

    def test_unslotted_regions_trigger_the_diagnostic_but_freeze_no_slot(self):
        # transicion (+7 dB) and aire (+12 dB) also exceed 4 dB; they
        # trigger EVO-003, but there is no EQ step to freeze there.
        corr = regional_correction(V1_1_ERROR_DB)
        result = check_residual_freeze(corr, mean_coherence=0.0)
        self.assertIn("transicion", result["triggered_regions"])
        self.assertIn("aire", result["triggered_regions"])
        self.assertEqual(result["frozen_slots"],
                         ["high_2k_4k", "presence_4k_8k"])

    def test_residual_exactly_4db_does_not_trigger(self):
        # "Supera 4 dB" is strictly greater: brillo error -6 dB gives
        # proposed +6, applied +2, residual exactly +4.0.
        error = [0.0] * 24
        for i in range(12, 16):  # ERB-13..16 brillo
            error[i] = -6.0
        result = check_residual_freeze(regional_correction(error),
                                       mean_coherence=0.0)
        self.assertEqual(result["triggered_regions"], [])
        self.assertFalse(result["evo003_required"])

    def test_residual_just_above_4db_triggers(self):
        error = [0.0] * 24
        for i in range(12, 16):  # ERB-13..16 brillo
            error[i] = -6.5  # proposed +6.5, applied +2, residual +4.5
        result = check_residual_freeze(regional_correction(error),
                                       mean_coherence=0.0)
        self.assertEqual(result["frozen_regions"], ["brillo"])
        self.assertEqual(result["frozen_slots"], ["high_2k_4k"])
        self.assertTrue(result["evo003_required"])

    def test_small_residuals_do_not_trigger(self):
        result = check_residual_freeze(regional_correction([-1.5] * 24),
                                       mean_coherence=0.0)
        self.assertEqual(result["triggered_regions"], [])
        self.assertFalse(result["evo003_required"])

    def test_good_coherence_releases_the_freeze(self):
        # "Hasta que la coherencia suba": mean coherence >= 0.20 (the
        # S9 guard, reused as "coherence has risen") means the table
        # can be trusted and the residual is a real deficit.
        corr = regional_correction(V1_1_ERROR_DB)
        result = check_residual_freeze(corr, mean_coherence=0.9)
        self.assertEqual(result["triggered_regions"],
                         ["transicion", "brillo", "presencia", "aire"])
        self.assertEqual(result["frozen_regions"], [])
        self.assertTrue(result["released"])
        self.assertFalse(result["evo003_required"])

    def test_deficit_confirmed_releases_even_at_zero_coherence(self):
        # The EVO-003 measurement confirmed the deficit: the freeze is
        # released even though coherence is still 0%.
        corr = regional_correction(V1_1_ERROR_DB)
        result = check_residual_freeze(corr, mean_coherence=0.0,
                                       deficit_confirmed=True)
        self.assertTrue(result["released"])
        self.assertEqual(result["frozen_regions"], [])
        self.assertFalse(result["evo003_required"])
        self.assertFalse(result["evo003_pending_reference_fix"])

    def test_compile_candidate_freezes_the_ladder_at_zero_coherence(self):
        out = compile_candidate(
            V1_1_ERROR_DB, [0.0] * 24, 0.0,
            erb_std_db=None, level_max_dbfs=-6.0)
        self.assertTrue(out["s10"]["evo003_required"])
        # No ladder rung climbs brillo/presencia past +2 dB.
        for rung in out["ladder"]:
            self.assertEqual(rung["eq_bands_db"]["high_2k_4k"], 2.0)
            self.assertEqual(rung["eq_bands_db"]["presence_4k_8k"], 2.0)
        self.assertTrue(any("S10 freeze" in n
                            for n in out["split"]["notes"]))

    def test_compile_candidate_climbs_when_the_freeze_is_released(self):
        # Good coherence releases the freeze: the v1.1 ladder climbs
        # to v1.5 again (and T6 still caps it at v1.3 — see TestT6).
        out = compile_candidate(
            V1_1_ERROR_DB, [0.9] * 24, 0.9,
            erb_std_db=[0.3] * 24, level_max_dbfs=-6.0)
        self.assertTrue(out["s10"]["released"])
        self.assertEqual(out["ladder"][-1]["version"], "v1.5")
        out = compile_candidate(
            V1_1_ERROR_DB, [0.0] * 24, 0.0,
            erb_std_db=None, level_max_dbfs=-6.0, deficit_confirmed=True)
        self.assertTrue(out["s10"]["released"])
        self.assertEqual(out["ladder"][-1]["version"], "v1.5")


class TestS15D10P0Covariates(unittest.TestCase):
    """S15: P0/D10 as A/B covariates, not decoration.

    The telemetry fields are optional (retrocompatible): the A/B must
    not require what the phone does not send yet. When they arrive on
    both sides, a move outside +/-0.5 dB makes the A/B inconclusive
    even if the RMS improves."""

    def _sessions(self, d10=None, p0=None, trim=None):
        sessions = []
        for _ in range(3):
            s = _session(5.23, [e * 0.5 for e in V1_1_ERROR_DB])
            if d10 is not None:
                s["d10_action"] = d10
            if p0 is not None:
                s["p0_db"] = p0
            if trim is not None:
                s["trim_db_effective"] = trim
            sessions.append(s)
        return sessions

    def _base(self, d10=1.0, p0=-6.0, trim=-4.0):
        return self._sessions(d10=d10, p0=p0, trim=trim)

    def test_telemetry_fields_are_accepted_when_present(self):
        chunk = {
            "error_mean_db": list(V1_1_ERROR_DB),
            "d10_action": 1.5,
            "p0_db": -6.2,
            "trim_db_effective": -4.0,
        }
        self.assertTrue(validate_chunk(chunk)["ok"])

    def test_nonfinite_d10_or_p0_is_rejected(self):
        chunk = {"error_mean_db": list(V1_1_ERROR_DB),
                 "d10_action": float("nan")}
        self.assertFalse(validate_chunk(chunk)["ok"])
        chunk = {"error_mean_db": list(V1_1_ERROR_DB),
                 "p0_db": float("inf")}
        self.assertFalse(validate_chunk(chunk)["ok"])

    def test_covariates_within_tolerance_are_ok(self):
        result = check_d10_p0_covariates(self._base(),
                                         self._sessions(d10=1.2, p0=-6.3,
                                                        trim=-4.1))
        self.assertEqual(result["status"], "ok")
        self.assertAlmostEqual(result["d10_action_delta_db"], 0.2)
        self.assertAlmostEqual(result["p0_db_delta_db"], -0.3)

    def test_exactly_at_tolerance_is_ok(self):
        result = check_d10_p0_covariates(self._base(),
                                         self._sessions(d10=1.5, p0=-6.5))
        self.assertEqual(result["status"], "ok")
        self.assertAlmostEqual(result["d10_action_delta_db"], 0.5)
        self.assertAlmostEqual(result["p0_db_delta_db"], -0.5)

    def test_d10_move_beyond_tolerance_is_inconclusive(self):
        result = check_d10_p0_covariates(self._base(),
                                         self._sessions(d10=1.8, p0=-6.0))
        self.assertEqual(result["status"], "inconclusive")
        self.assertAlmostEqual(result["d10_action_delta_db"], 0.8)
        self.assertIn("D10 action", result["reason"])

    def test_p0_move_beyond_tolerance_is_inconclusive(self):
        result = check_d10_p0_covariates(self._base(),
                                         self._sessions(d10=1.0, p0=-6.7))
        self.assertEqual(result["status"], "inconclusive")
        self.assertAlmostEqual(result["p0_db_delta_db"], -0.7)
        self.assertIn("P0", result["reason"])

    def test_trim_move_does_not_gate_but_is_reported(self):
        # The spec fixes no tolerance for the trim: a -2 dB trim move
        # is reported, not gating.
        result = check_d10_p0_covariates(self._base(),
                                         self._sessions(d10=1.0, p0=-6.0,
                                                        trim=-6.0))
        self.assertEqual(result["status"], "ok")
        self.assertAlmostEqual(result["trim_db_effective_delta_db"], -2.0)

    def test_missing_telemetry_is_unchecked_not_blocking(self):
        result = check_d10_p0_covariates(self._sessions(),
                                         self._sessions())
        self.assertEqual(result["status"], "unchecked")
        self.assertIsNone(result["d10_action_delta_db"])
        self.assertIn("SKIPPED", result["reason"])

    def test_telemetry_on_one_side_only_is_unchecked(self):
        result = check_d10_p0_covariates(self._base(), self._sessions())
        self.assertEqual(result["status"], "unchecked")

    def test_zero_d10_mean_is_a_value_not_an_absence(self):
        # A genuine mean of 0.0 must not collapse to "absent".
        result = check_d10_p0_covariates(self._sessions(d10=0.0, p0=-6.0),
                                         self._sessions(d10=0.8, p0=-6.0))
        self.assertEqual(result["status"], "inconclusive")
        self.assertAlmostEqual(result["d10_action_delta_db"], 0.8)

    def test_tolerance_is_a_parameter_not_a_buried_constant(self):
        result = check_d10_p0_covariates(self._base(),
                                         self._sessions(d10=1.8, p0=-6.0),
                                         tolerance_db=1.0)
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["tolerance_db"], 1.0)
        self.assertEqual(D10_P0_TOLERANCE_DB, 0.5)

    def test_ab_with_d10_move_is_inconclusive_despite_rms_improvement(self):
        base = [dict(_session(BASELINE_RMS_DB, V1_1_ERROR_DB),
                     d10_action=1.0, p0_db=-6.0) for _ in range(3)]
        cand = self._sessions(d10=1.8, p0=-6.0)
        result = evaluate_ab(base, cand)
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertTrue(result["gates"]["erb_improvement"]["passed"])
        self.assertFalse(result["gates"]["d10_p0_covariates"]["passed"])

    def test_ab_with_p0_move_is_inconclusive(self):
        base = [dict(_session(BASELINE_RMS_DB, V1_1_ERROR_DB),
                     d10_action=1.0, p0_db=-6.0) for _ in range(3)]
        cand = self._sessions(d10=1.0, p0=-6.7)
        result = evaluate_ab(base, cand)
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertFalse(result["gates"]["d10_p0_covariates"]["passed"])

    def test_ab_within_tolerance_promotes(self):
        base = [dict(_session(BASELINE_RMS_DB, V1_1_ERROR_DB),
                     d10_action=1.0, p0_db=-6.0) for _ in range(3)]
        cand = self._sessions(d10=1.2, p0=-6.3)
        result = evaluate_ab(base, cand)
        self.assertEqual(result["verdict"], "promote")

    def test_ab_without_telemetry_promotes_as_before(self):
        base = [_session(BASELINE_RMS_DB, V1_1_ERROR_DB) for _ in range(3)]
        cand = self._sessions()
        result = evaluate_ab(base, cand)
        self.assertEqual(result["verdict"], "promote")
        self.assertTrue(result["gates"]["d10_p0_covariates"]["passed"])
        self.assertIn("SKIPPED",
                      result["gates"]["d10_p0_covariates"]["detail"])

    def test_rollback_outranks_inconclusive(self):
        # Candidate fails on its merits (no RMS improvement) AND moves
        # D10: the verdict is rollback, not inconclusive.
        base = [dict(_session(BASELINE_RMS_DB, V1_1_ERROR_DB),
                     d10_action=1.0, p0_db=-6.0) for _ in range(3)]
        cand = [dict(_session(BASELINE_RMS_DB, V1_1_ERROR_DB),
                     d10_action=1.8, p0_db=-6.0) for _ in range(3)]
        result = evaluate_ab(base, cand)
        self.assertEqual(result["verdict"], "rollback")
        self.assertFalse(result["gates"]["erb_improvement"]["passed"])


class TestT3PlayingEvidence(unittest.TestCase):
    """T3: confirmation "sounding, not just installed".

    The server only starts counting the 3 test sessions when it sees
    evidence that it sounds: beacon advancing + valid frames with the
    candidate. Without evidence a chunk does not count — it is neither
    a success nor a failure."""

    def _evidence(self, beacon=150, snapshots=3, frames=True, daemon=True):
        return {
            "effect_beacon_frames": beacon,
            "snapshots_accepted": snapshots,
            "frames_valid": frames,
            "daemon_ok": daemon,
        }

    def test_full_evidence_with_advancing_beacon_counts(self):
        result = check_playing_evidence(self._evidence(), 100)
        self.assertTrue(result["counts"])
        self.assertEqual(result["status"], "counts")
        self.assertTrue(all(result["checks"].values()))

    def test_first_beacon_observation_does_not_count(self):
        # No previous value: advance cannot be established yet.
        result = check_playing_evidence(self._evidence(), None)
        self.assertFalse(result["counts"])
        self.assertEqual(result["status"], "not_counted")
        self.assertIn("first beacon observation", result["reason"])

    def test_stalled_beacon_does_not_count(self):
        result = check_playing_evidence(self._evidence(beacon=150), 150)
        self.assertFalse(result["counts"])
        self.assertFalse(result["checks"]["beacon_advancing"])
        self.assertIn("NOT advancing", result["reason"])
        result = check_playing_evidence(self._evidence(beacon=90), 100)
        self.assertFalse(result["counts"])

    def test_daemon_down_does_not_count(self):
        result = check_playing_evidence(self._evidence(daemon=False), 100)
        self.assertFalse(result["counts"])
        self.assertFalse(result["checks"]["daemon_ok"])
        self.assertIn("daemon", result["reason"])

    def test_daemon_ok_missing_does_not_count(self):
        evidence = self._evidence()
        del evidence["daemon_ok"]
        result = check_playing_evidence(evidence, 100)
        self.assertFalse(result["counts"])
        self.assertIn("daemon_ok missing", result["reason"])

    def test_no_valid_frames_does_not_count(self):
        result = check_playing_evidence(self._evidence(frames=False), 100)
        self.assertFalse(result["counts"])
        self.assertFalse(result["checks"]["frames_valid"])
        result = check_playing_evidence(self._evidence(frames=0), 100)
        self.assertFalse(result["counts"])

    def test_frames_valid_as_a_count_works(self):
        result = check_playing_evidence(self._evidence(frames=12), 100)
        self.assertTrue(result["counts"])
        self.assertTrue(result["checks"]["frames_valid"])

    def test_no_snapshots_accepted_does_not_count(self):
        result = check_playing_evidence(self._evidence(snapshots=0), 100)
        self.assertFalse(result["counts"])
        self.assertFalse(result["checks"]["snapshots_accepted"])
        evidence = self._evidence()
        del evidence["snapshots_accepted"]
        result = check_playing_evidence(evidence, 100)
        self.assertFalse(result["counts"])

    def test_no_evidence_at_all_does_not_count(self):
        result = check_playing_evidence({}, None)
        self.assertFalse(result["counts"])
        self.assertEqual(result["status"], "not_counted")
        # Not counted is neither success nor failure.
        self.assertIn("NOT count", result["reason"])
        self.assertIn("neither success nor failure", result["reason"])

    def test_non_dict_evidence_does_not_count(self):
        self.assertFalse(check_playing_evidence(None, 100)["counts"])

    def test_count_playing_chunks_threads_the_beacon(self):
        evidences = [
            self._evidence(beacon=100),  # first: advance not established
            self._evidence(beacon=150),  # counts
            self._evidence(beacon=150),  # stalled: does not count
            self._evidence(beacon=200),  # counts
        ]
        result = count_playing_chunks(evidences)
        self.assertEqual(result["counted"], 2)
        self.assertEqual(result["not_counted"], 2)
        self.assertEqual(result["last_beacon_frames"], 200)
        self.assertEqual(len(result["decisions"]), 4)

    def test_count_playing_chunks_with_a_previous_beacon(self):
        result = count_playing_chunks([self._evidence(beacon=150)],
                                      previous_beacon_frames=100)
        self.assertEqual(result["counted"], 1)
        self.assertEqual(result["last_beacon_frames"], 150)

    def test_count_playing_chunks_empty_list(self):
        result = count_playing_chunks([])
        self.assertEqual(result["counted"], 0)
        self.assertEqual(result["not_counted"], 0)
        self.assertIsNone(result["last_beacon_frames"])


class TestS2S16Verification(unittest.TestCase):
    """Fase 6 verification, empirical on the CURRENT data (nothing
    invented): S1 says coherence 0% is instrumentation; S2 must say
    whether the deficit is a room comb or a response. With the current
    data it cannot — and that is exactly why S16 (the physical
    EVO-003 measurement) does NOT trigger."""

    def test_s2_with_current_data_is_indeterminate(self):
        result = fit_comb_vs_response(V1_1_ERROR_DB, None)
        self.assertEqual(result["verdict"], "indeterminate")
        self.assertIn("erb_std_db_missing", result["reason"])
        self.assertIsNone(result["std_contrast_db"])
        # The fit itself ran and does not favour a comb either:
        # measured improvement 9.0%, far below the 25% comb bar.
        self.assertLess(result["improvement"], COMB_IMPROVEMENT_MIN)
        self.assertGreater(result["improvement"], 0.0)
        self.assertIsNotNone(result["distance_m"])

    def test_s1_plus_s2_do_not_point_to_the_room_so_s16_does_not_trigger(self):
        with open(V1_1_PATH, encoding="utf-8") as fh:
            profile = json.load(fh)
        # S1 premise: coherence 0% in the live profile (instrumentation,
        # not room data — the Organism path does not feed the reference).
        self.assertAlmostEqual(profile["fingerprint"]["mean_coherence"], 0.0)
        # S2 premise: indeterminate without erbStdDb.
        result = fit_comb_vs_response(V1_1_ERROR_DB, None)
        self.assertEqual(result["verdict"], "indeterminate")
        # Conclusion (documented in README §S2/S16, not a function):
        # neither S1 nor S2 points to the room, so S16's physical
        # EVO-003 measurement does NOT trigger. The next step is the S1
        # reference fix + erbStdDb in the payload, not a position probe.


if __name__ == "__main__":
    unittest.main()
