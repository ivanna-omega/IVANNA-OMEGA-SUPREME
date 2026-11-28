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
    COHERENCE_GUARD,
    COMB_FIT_D_MAX_M,
    COMB_FIT_D_MIN_M,
    ERB_CENTER_HZ,
    EQ_CLAMP_DB,
    LADDER_STEP_DB,
    REGIONS,
    TARGET_RMS_DB,
    coherence_guard,
    compile_candidate,
    detect_resonances,
    emit_ladder,
    evaluate_ab,
    fit_comb_vs_response,
    precheck_headroom,
    region_rms,
    regional_correction,
    split_cut_boost,
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
        # The S5 ladder still plans the climb from the clamped slots.
        self.assertEqual(out["ladder"][0]["version"], "v1.2")
        self.assertEqual(out["ladder"][-1]["version"], "v1.5")

    def test_pipeline_with_good_coherence_emits_v1_1a(self):
        out = compile_candidate(
            V1_1_ERROR_DB, [0.9] * 24, 0.9,
            erb_std_db=[0.3] * 24, level_max_dbfs=-6.0)
        self.assertEqual(out["coherence_guard"]["mode"], "full")
        self.assertEqual(out["split"]["v1_1a"]["eq_bands_db"],
                         {"low_60_120": -2.0})
        self.assertIsNone(out["split"]["v1_1b"])  # v1.1a not yet promoted


if __name__ == "__main__":
    unittest.main()
