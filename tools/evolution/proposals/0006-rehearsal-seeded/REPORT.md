# Propuesta 0006-rehearsal-seeded — ENSAYO CONTROLADO: clamp_amount no clampéa a [0,1] (fallo sembrado)

- **Hallazgo:** rehearsal-seeded
- **Evidencia:** test_rehearsal_seed.py: clamp_amount(2.0) devuelve 2.0 en vez de 1.0; clamp_amount(-1.0) devuelve -1.0 en vez de 0.0. Fallo sembrado a propósito para el ensayo; NO es una reparación real.
- **Consejo:** consejo de acuerdo: proposer + reviewer + arbiter (1 rondas)
- **Draft:** modelo consejo: proposer ddgw/tinfoil/gpt-oss-120b + reviewer ddgw/claude-haiku-4-5 + arbiter ddgw/gpt-5.6-luna, 5.0 s
- **Archivos:** tools/evolution/rehearsal_seed.py
- **Veredicto:** VERDE — lista para revisión/publicación
- **Árbol intacto:** False (scratch worktree, eliminado: True)

## Validación (ver validation.json)

```json
{
  "proposal": "0006-rehearsal-seeded",
  "patch": "0006-rehearsal-seeded.patch",
  "files": [
    "tools/evolution/rehearsal_seed.py"
  ],
  "scratch": {
    "mode": "worktree",
    "removed": true,
    "worktree": "/home/hatch/workspace/ivanna-evo-94fwy6kw/wt"
  },
  "applied": true,
  "full_suite": {
    "skipped": true,
    "reason": "el parche no toca C++"
  },
  "python_checks": {
    "unittest": {
      "passed": true,
      "tail": "sts_file) ... ok\ntest_new_file_lists_file (test_patch_gates.TestPatchTouchedFiles.test_new_file_lists_file) ... ok\ntest_delete_forbidden_raises_before_worktree (test_patch_gates.TestValidateProposalDeletionGate.test_delete_forbidden_raises_before_worktree) ... ok\ntest_clamps_above_one (test_rehearsal_seed.TestRehearsalSeed.test_clamps_above_one) ... ok\ntest_clamps_below_zero (test_rehearsal_seed.TestRehearsalSeed.test_clamps_below_zero) ... ok\ntest_keeps_valid (test_rehearsal_seed.TestRehearsalSeed.test_keeps_valid) ... ok\ntest_scan_saf_guards_filters_false_positives_and_deduplicates (test_scan_saf_guards.TestScanSafGuards.test_scan_saf_guards_filters_false_positives_and_deduplicates) ... ok\n\n----------------------------------------------------------------------\nRan 74 tests in 0.539s\n\nOK\n"
    },
    "scan": {
      "passed": true,
      "tail": "\n        {\n          \"file\": \"app/src/main/cpp/tests/CMakeLists.txt\",\n          \"line\": 188,\n          \"tag\": \"TODO\",\n          \"text\": \"# archivo: buscan \\\"FALLARON\\\"/\\\"FAIL\\\" como subcadena en TODO el stdout+stderr\"\n        },\n        {\n          \"file\": \"app/src/main/cpp/tests/test_punch_engine.cpp\",\n          \"line\": 126,\n          \"tag\": \"TODO\",\n          \"text\": \"for (size_t i = 0; i < in.l.size(); ++i) {                                   // TODO el contenido con el mismo paneo 0.7\"\n        },\n        {\n          \"file\": \"app/src/main/cpp/usb_audio_pro_manager.cpp\",\n          \"line\": 36,\n          \"tag\": \"TODO\",\n          \"text\": \"// Sustituye por completo los dos TODO (\\\"motor asíncrono real\\\" / \\\"detención\"\n        }\n      ],\n      \"total\": 28,\n      \"truncated\": false\n    }\n  }\n}\n",
      "knowledge_json": true
    }
  },
  "built": [],
  "new_tests": {},
  "tree_untouched": false
}
```
