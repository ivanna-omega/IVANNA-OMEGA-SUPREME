# Draft 0006-rehearsal-seeded (consejo multi-modelo vía OmniRoute)

- **Modelo:** consejo: proposer ddgw/tinfoil/gpt-oss-120b + reviewer ddgw/claude-haiku-4-5 + arbiter ddgw/gpt-5.6-luna (gateway http://localhost:20128/v1)
- **Latencia del consejo:** 5.0 s
- **Veredicto del consejo:** consejo de acuerdo: proposer + reviewer + arbiter
- **Hallazgo:** rehearsal-seeded: ENSAYO CONTROLADO: clamp_amount no clampéa a [0,1] (fallo sembrado)
- **Archivos del parche:** tools/evolution/rehearsal_seed.py

## Transcripción del consejo

```
CONSEJO (veredicto: consejo de acuerdo: proposer + reviewer + arbiter)
Proposer: ddgw/tinfoil/gpt-oss-120b
Reviewer: ddgw/claude-haiku-4-5
Arbiter: ddgw/gpt-5.6-luna
Ronda 1: proposer=diff redactado; reviewer={'round': 1, 'approved': True, 'text': 'APROBADO\n\nCorrige la causa raíz: ahora sí aplica el clamp a \\[0,1\\] en vez de solo tocar el síntoma. No degrada rendimiento de forma relevante y el diff es limpio: no inventa APIs ni rompe la firma. Además cubre ambos extremos y deja pasar valores válidos sin cambio.'}

diff --git a/tools/evolution/rehearsal_seed.py b/tools/evolution/rehearsal_seed.py
--- a/tools/evolution/rehearsal_seed.py
+++ b/tools/evolution/rehearsal_seed.py
@@ -14,3 +14,15 @@
-def clamp_amount(x: float) -> float:
-    """Cantidad DSP 0..1. BUG SEMBRADO: devuelve x sin clampear."""
-    return x
+def clamp_amount(x: float) -> float:
+    """Clamp a DSP amount to the inclusive range [0, 1].
+
+    The original implementation returned the input value unchanged,
+    which violated the contract that the function should constrain
+    values to the DSP‑valid interval. This refined implementation
+    ensures that any value below 0.0 is raised to 0.0 and any value
+    above 1.0 is lowered to 1.0, while values already within the range
+    are returned unchanged.
+    """
+    if x < 0.0:
+        return 0.0
+    if x > 1.0:
+        return 1.0
+    return x

```
