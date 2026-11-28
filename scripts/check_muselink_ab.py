#!/usr/bin/env python3
"""check_muselink_ab.py — Espejo reproducible de la decision A/B (C7).

Implementa literalmente la maquina de estados A/B de MuseLink
(STABLE -> CANDIDATE -> AB_RUNNING -> EVALUATE -> PROMOTED / ROLLED_BACK)
sobre fixtures, sin LLM, sin red, con semilla fija. Solo stdlib.

Uso de referencia (C7):
    python3 scripts/check_muselink_ab.py \
        --baseline tools/evolution/fixtures/ab/v1.0_sessions.json \
        --candidate tools/evolution/fixtures/ab/v1.1_sessions.json \
        --candidate-profile tools/evolution/fixtures/omega-1795857152818-9lec-v1.1.ivanna.json \
        --seed 1795857152818

Las 5 puertas (una linea por puerta en la salida):

  Puerta 1 (§4-1, S8): mejora ERB >= 20%: (RMS_base - RMS_cand) / RMS_base.
      Base RMS 10.46-10.67 dB -> objetivo <= ~8.4 dB.
  Puerta 2 (§4-2, S8): coherencia media candidata >= base - 0.05. Con la
      base verificada en 0.0, esta puerta NO discrimina hoy (un candidato
      no puede caer mas de 0.05 por debajo de 0.0); se mantiene para que
      discrimine cuando S1/S2 muevan la coherencia de 0.0.
  Puerta 3 (§4-3, S8): ninguna resonancia NUEVA: banda >= +6 dB en el
      candidato que no estuviera ya >= +6 dB en la base. ERB-3 209 Hz
      +9.86 dB ya existe en la base, asi que no bloquea.
  Puerta 4 (§4-4, S8): ningun chunk con levelMaxDbfs >= 0 dBFS.
  Puerta 5 (S8 endurecido, a/b/c): ninguna region empeora mas de 1 dB RMS;
      el limitador APEX no trabaja mas (caida de crest <= 0.5 dB, sin
      levelMax clavado en -0.1 dBFS si la base no lo estaba); sonoridad
      |levelMean_cand - levelMean_base| <= 3 dB (y |LUFS| <= 1 dB cuando
      existe esa telemetria).

Las puertas 1-4 y los tres sub-criterios de la 5 NO se reimplementan
aqui: se reutiliza `tools/muselink-analyzer/analyzer.py::evaluate_ab`
(Fase 3, S8), que ya los implementa. Este script solo anade la maquina
de estados, la lectura de fixtures/perfiles, el conteo de chunks con
clip y la salida de veredicto. Si el analizador no se puede importar,
el script falla (salida 2) en vez de duplicar la logica en silencio.

Fixtures: la exportacion de la tabla ERB agregada de las 177 sesiones
NO existe todavia (pendiente del servidor), asi que C7 corre con
fixtures SINTETICOS y lo dice en su salida. Un fixture real futuro
lleva `"synthetic": false` y su procedencia de servidor; el script lo
acepta igual (la marca solo cambia el aviso).

La semilla fija (`--seed`, por defecto 1795857152818, el id numerico
del perfil vivo) hace determinista la seleccion de sesiones: si un
fixture trae mas de `--sessions-per-side` sesiones por lado, se eligen
exactamente esas con `random.Random(seed).sample` sobre los ids
ordenados. Con exactamente 3 sesiones por lado la seleccion es la
identidad; la agregacion (medias y maximos) no depende del orden.

Salida 0 = PROMOTED; 1 = ROLLED_BACK; 2 = error de entrada/uso.
"""

import argparse
import json
import random
import sys
from pathlib import Path

# --- Localizacion del repo y reutilizacion del analizador S8 (Fase 3) ---
REPO_ROOT = Path(__file__).resolve().parent.parent
ANALYZER_DIR = REPO_ROOT / "tools" / "muselink-analyzer"
DEFAULT_CANDIDATE_PROFILE = (
    REPO_ROOT / "tools" / "evolution" / "fixtures"
    / "omega-1795857152818-9lec-v1.1.ivanna.json"
)
DEFAULT_SEED = 1795857152818
DEFAULT_SESSIONS_PER_SIDE = 3
CLIP_THRESHOLD_DBFS = 0.0

try:
    sys.path.insert(0, str(ANALYZER_DIR))
    from analyzer import (  # noqa: E402
        AB_COHERENCE_SLACK,
        AB_MAX_REGION_WORSENING_DB,
        AB_MIN_IMPROVEMENT,
        BASELINE_RMS_DB,
        ERB_CENTER_HZ,
        TARGET_RMS_DB,
        detect_resonances,
        evaluate_ab,
    )
except ImportError as exc:  # pragma: no cover - importacion fallida
    print("ERROR: no se pudo importar tools/muselink-analyzer/analyzer.py "
          "(C7 reutiliza sus puertas S8; no se duplica la logica): %s"
          % exc, file=sys.stderr)
    sys.exit(2)


def fail(msg):
    """Error de entrada/uso: salida 2, sin veredicto."""
    print("ERROR: %s" % msg, file=sys.stderr)
    sys.exit(2)


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def load_fixture(path, role):
    """Carga y valida la estructura minima de un fixture de sesiones."""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail("fixture %s no encontrado: %s" % (role, path))
    except (OSError, ValueError) as exc:
        fail("fixture %s no legible como JSON (%s): %s" % (role, path, exc))
    if not isinstance(data, dict):
        fail("fixture %s: debe ser un objeto JSON" % role)
    sessions = data.get("sessions")
    if not isinstance(sessions, list) or not sessions:
        fail("fixture %s: falta la lista no vacia 'sessions'" % role)
    for i, s in enumerate(sessions):
        where = "%s sessions[%d]" % (role, i)
        if not isinstance(s, dict):
            fail("%s: debe ser un objeto" % where)
        for key in ("session_id", "spectral_error_rms_db", "error_mean_db",
                    "mean_coherence", "level_max_dbfs", "level_mean_dbfs"):
            if key not in s:
                fail("%s: falta la clave '%s'" % (where, key))
        if not is_num(s["spectral_error_rms_db"]):
            fail("%s: spectral_error_rms_db debe ser numero" % where)
        err = s["error_mean_db"]
        if (not isinstance(err, list) or len(err) != 24
                or not all(is_num(v) for v in err)):
            fail("%s: error_mean_db debe ser una lista de 24 numeros" % where)
        for key in ("mean_coherence", "level_max_dbfs", "level_mean_dbfs"):
            if not is_num(s[key]):
                fail("%s: %s debe ser numero" % (where, key))
        chunks = s.get("chunks")
        if chunks is not None:
            if (not isinstance(chunks, list)
                    or not all(isinstance(c, dict)
                               and is_num(c.get("level_max_dbfs"))
                               for c in chunks)):
                fail("%s: chunks debe ser una lista de {level_max_dbfs}"
                     % where)
    return data


def select_sessions(fixture, count, seed):
    """Seleccion determinista de `count` sesiones por semilla fija."""
    sessions = sorted(fixture["sessions"], key=lambda s: s["session_id"])
    if len(sessions) < count:
        return None
    if len(sessions) == count:
        return sessions
    rng = random.Random(seed)
    return rng.sample(sessions, count)


def count_clipped_chunks(sessions):
    """(chunks con clip, chunks totales, detalle por sesion).

    Un chunk esta en clip cuando levelMaxDbfs >= 0 dBFS. Si una sesion
    no trae lista `chunks`, cuenta como un unico chunk con su
    level_max_dbfs de sesion.
    """
    clipped = 0
    total = 0
    per_session = []
    for s in sessions:
        chunks = s.get("chunks")
        if chunks is None:
            levels = [s["level_max_dbfs"]]
        else:
            levels = [c["level_max_dbfs"] for c in chunks]
        n_clip = sum(1 for v in levels if v >= CLIP_THRESHOLD_DBFS)
        clipped += n_clip
        total += len(levels)
        per_session.append((s["session_id"], n_clip, len(levels)))
    return clipped, total, per_session


def load_profile(path, role):
    """Carga un .ivanna.json y extrae sus numeros ERB de procedencia."""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail("perfil %s no encontrado: %s" % (role, path))
    except (OSError, ValueError) as exc:
        fail("perfil %s no legible como JSON (%s): %s" % (role, path, exc))
    if not isinstance(data, dict):
        fail("perfil %s: debe ser un objeto JSON" % role)
    fp = data.get("fingerprint") or {}
    info = {
        "path": str(path),
        "source": data.get("source", "?"),
        "spectral_error_rms_db": fp.get("spectral_error_rms_db"),
        "mean_coherence": fp.get("mean_coherence"),
        "erb3_error_db": None,
    }
    bands = fp.get("erb_bands_measured") or {}
    erb3 = bands.get("erb3_209hz") or {}
    if isinstance(erb3, dict) and is_num(erb3.get("error_db")):
        info["erb3_error_db"] = erb3["error_db"]
    return info


def fmt_profile(info):
    rms = info["spectral_error_rms_db"]
    coh = info["mean_coherence"]
    erb3 = info["erb3_error_db"]
    parts = []
    if is_num(rms):
        parts.append("RMS %.2f dB" % rms)
    if is_num(coh):
        parts.append("coherencia %.2f" % coh)
    if is_num(erb3):
        parts.append("ERB-3 209 Hz %+.2f dB" % erb3)
    return "; ".join(parts) if parts else "sin numeros ERB en fingerprint"


def fmt_resonances(mean_error_db):
    """'ERB-3 209 Hz +9.86 dB, ...' o 'ninguna'."""
    bands = detect_resonances(mean_error_db)
    if not bands:
        return "ninguna"
    return ", ".join(
        "ERB-%d %d Hz %+.2f dB" % (b, ERB_CENTER_HZ[b - 1],
                                   mean_error_db[b - 1])
        for b in bands)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Espejo reproducible de la decision A/B (C7, release 510).")
    parser.add_argument("--baseline", required=True,
                        help="fixture JSON de sesiones base (v1.0)")
    parser.add_argument("--candidate", required=True,
                        help="fixture JSON de sesiones candidatas (v1.1+)")
    parser.add_argument("--baseline-profile", default=None,
                        help="perfil .ivanna.json base (opcional: el "
                             "perfil v1.0 aun no esta en el repo)")
    parser.add_argument("--candidate-profile", default=None,
                        help="perfil .ivanna.json candidato (por defecto: "
                             "tools/evolution/fixtures/omega-1795857152818-"
                             "9lec-v1.1.ivanna.json si existe)")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED,
                        help="semilla fija de seleccion de sesiones "
                             "(por defecto %(default)s)")
    parser.add_argument("--sessions-per-side", type=int,
                        default=DEFAULT_SESSIONS_PER_SIDE,
                        help="sesiones por lado en el A/B (por defecto "
                             "%(default)s, segun la especificacion)")
    parser.add_argument("--json-out", default=None,
                        help="ruta opcional donde escribir el veredicto "
                             "en JSON (para CI/triaje)")
    args = parser.parse_args(argv)

    if args.sessions_per_side < 1:
        fail("--sessions-per-side debe ser >= 1")

    base_fx = load_fixture(args.baseline, "base")
    cand_fx = load_fixture(args.candidate, "candidato")
    synthetic = bool(base_fx.get("synthetic") and cand_fx.get("synthetic"))

    base_sessions = select_sessions(base_fx, args.sessions_per_side,
                                    args.seed)
    cand_sessions = select_sessions(cand_fx, args.sessions_per_side,
                                    args.seed)
    if base_sessions is None:
        fail("fixture base trae %d sesiones, se necesitan %d"
             % (len(base_fx["sessions"]), args.sessions_per_side))
    if cand_sessions is None:
        fail("fixture candidato trae %d sesiones, se necesitan %d"
             % (len(cand_fx["sessions"]), args.sessions_per_side))

    # --- Perfiles (procedencia; no son puertas, solo contexto) ---
    baseline_profile = None
    if args.baseline_profile:
        baseline_profile = load_profile(args.baseline_profile, "base")
    candidate_profile = None
    candidate_profile_path = args.candidate_profile
    if candidate_profile_path is None and DEFAULT_CANDIDATE_PROFILE.exists():
        candidate_profile_path = str(DEFAULT_CANDIDATE_PROFILE)
    if candidate_profile_path:
        candidate_profile = load_profile(candidate_profile_path, "candidato")

    # --- Maquina de estados: EVALUATE delega en analyzer.evaluate_ab ---
    print("MuseLink A/B — espejo reproducible (C7, release 510)")
    if synthetic:
        print("AVISO: fixtures SINTETICOS (synthetic=true en ambos lados). "
              "La exportacion de la tabla ERB agregada de las 177 sesiones "
              "esta pendiente del servidor; estos numeros NO son sesiones "
              "medidas.")
    else:
        print("Fixtures NO sinteticos (o sin marca synthetic=true): "
              "numeros procedentes de exportacion real del servidor.")
    print("seed=%d; sesiones por lado=%d (base %d disponibles, candidato "
          "%d disponibles)"
          % (args.seed, args.sessions_per_side,
             len(base_fx["sessions"]), len(cand_fx["sessions"])))
    print("[STABLE] base: %s (%s)"
          % (base_fx.get("profile_version", "?"), args.baseline))
    if baseline_profile:
        print("         perfil base: %s" % fmt_profile(baseline_profile))
    else:
        print("         perfil base en disco: ausente (el .ivanna.json v1.0 "
              "aun no esta en el repo; la puerta no lo necesita)")
    print("[CANDIDATE] candidato: %s (%s)"
          % (cand_fx.get("profile_version", "?"), args.candidate))
    if candidate_profile:
        print("         perfil candidato: %s" % fmt_profile(candidate_profile))
        print("         (su RMS es la medicion compilada que origino el "
              "candidato, no el RMS post-correccion: no se compara con "
              "las sesiones del candidato)")
    print("[AB_RUNNING] %d sesiones base + %d candidatas seleccionadas "
          "con seed=%d: base [%s] candidato [%s]"
          % (len(base_sessions), len(cand_sessions), args.seed,
             ", ".join(s["session_id"] for s in base_sessions),
             ", ".join(s["session_id"] for s in cand_sessions)))

    result = evaluate_ab(base_sessions, cand_sessions)
    base_sum = result["base"]
    cand_sum = result["candidate"]
    gates = result["gates"]
    clipped, total_chunks, per_session = count_clipped_chunks(cand_sessions)

    print("[EVALUATE] puertas (reutilizan tools/muselink-analyzer/"
          "analyzer.py::evaluate_ab):")
    base_rms = base_sum["spectral_error_rms_db"]
    cand_rms = cand_sum["spectral_error_rms_db"]
    improvement = ((base_rms - cand_rms) / base_rms) if base_rms > 0 else 0.0
    print("[PUERTA 1/5] mejora ERB: %s — RMS base %.2f dB -> candidato "
          "%.2f dB (mejora %.1f%%, exigida >= %.0f%%; objetivo <= %.2f dB "
          "desde linea base %.2f dB)"
          % ("PASS" if gates["erb_improvement"]["passed"] else "FAIL",
             base_rms, cand_rms, 100.0 * improvement,
             100.0 * AB_MIN_IMPROVEMENT, TARGET_RMS_DB, BASELINE_RMS_DB))
    print("[PUERTA 2/5] coherencia: %s — media base %.3f -> candidato "
          "%.3f (caida permitida <= %.2f)%s"
          % ("PASS" if gates["coherence"]["passed"] else "FAIL",
             base_sum["mean_coherence"], cand_sum["mean_coherence"],
             AB_COHERENCE_SLACK,
             " — con base 0.0 esta puerta no discrimina hoy"
             if base_sum["mean_coherence"] == 0.0 else ""))
    print("[PUERTA 3/5] sin resonancia nueva: %s — base [%s]; candidato "
          "[%s]"
          % ("PASS" if gates["no_new_resonance"]["passed"] else "FAIL",
             fmt_resonances(base_sum["mean_error_db"]),
             fmt_resonances(cand_sum["mean_error_db"])))
    print("[PUERTA 4/5] sin clip: %s — peor chunk candidato "
          "levelMaxDbfs %.2f dBFS (clip en >= %.1f dBFS); chunks con clip "
          "%d/%d (%s)"
          % ("PASS" if gates["no_clip"]["passed"] else "FAIL",
             cand_sum["level_max_dbfs"], CLIP_THRESHOLD_DBFS,
             clipped, total_chunks,
             ", ".join("%s: %d/%d" % p for p in per_session)))
    s8 = (gates["region_rms"], gates["apex_not_working_harder"],
          gates["loudness_match"])
    print("[PUERTA 5/5] S8 endurecido: %s — (a) regiones: %s; (b) APEX: "
          "%s; (c) sonoridad: %s"
          % ("PASS" if all(g["passed"] for g in s8) else "FAIL",
             gates["region_rms"]["detail"],
             gates["apex_not_working_harder"]["detail"],
             gates["loudness_match"]["detail"]))

    verdict = "PROMOTED" if result["verdict"] == "promote" else "ROLLED_BACK"
    print("[%s] veredicto: %s (seed=%d)"
          % ("PROMOTED" if verdict == "PROMOTED" else "ROLLED_BACK",
             verdict, args.seed))
    if verdict == "ROLLED_BACK":
        failed = [name for name, g in (
            ("puerta 1 (mejora ERB)", gates["erb_improvement"]),
            ("puerta 2 (coherencia)", gates["coherence"]),
            ("puerta 3 (resonancia nueva)", gates["no_new_resonance"]),
            ("puerta 4 (clip)", gates["no_clip"]),
            ("puerta 5 (S8 endurecido)",
             {"passed": all(g["passed"] for g in s8)}),
        ) if not g["passed"]]
        print("         puertas que bloquean: %s" % "; ".join(failed))

    if args.json_out:
        report = {
            "verdict": verdict,
            "seed": args.seed,
            "synthetic": synthetic,
            "baseline": args.baseline,
            "candidate": args.candidate,
            "baseline_profile": (baseline_profile or {}).get("path"),
            "candidate_profile": (candidate_profile or {}).get("path"),
            "base_rms_db": base_rms,
            "candidate_rms_db": cand_rms,
            "improvement": improvement,
            "base_coherence": base_sum["mean_coherence"],
            "candidate_coherence": cand_sum["mean_coherence"],
            "clipped_chunks": clipped,
            "total_chunks": total_chunks,
            "doors": {
                "erb_improvement": gates["erb_improvement"],
                "coherence": gates["coherence"],
                "no_new_resonance": gates["no_new_resonance"],
                "no_clip": gates["no_clip"],
                "region_rms": gates["region_rms"],
                "apex_not_working_harder":
                    gates["apex_not_working_harder"],
                "loudness_match": gates["loudness_match"],
            },
        }
        try:
            Path(args.json_out).write_text(
                json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8")
        except OSError as exc:
            fail("no se pudo escribir --json-out %s: %s"
                 % (args.json_out, exc))

    return 0 if verdict == "PROMOTED" else 1


if __name__ == "__main__":
    sys.exit(main())
