#!/usr/bin/env python3
"""validate_ivanna_profile.py — Valida un archivo .ivanna.json contra el
schema v1.0 (docs/IVANNA_OMEGA_FORMAT_SCHEMA.md).

Uso:
    python3 scripts/validate_ivanna_profile.py archivo.ivanna.json [otro...]
    python3 scripts/validate_ivanna_profile.py --live-session sesion.ivanna.json

Modos:
  * Por defecto (FLAC): perfil medido de un archivo de audio. duration_s > 0
    y el fingerprint FLAC completo son obligatorios; las claves ERB de
    sesión en vivo son desconocidas aquí.
  * --live-session (S4, release 510): perfil de sesión en vivo (micrófono
    ERB, sin FLAC). duration_s = 0 permitido; las claves FLAC del
    fingerprint pasan a ser opcionales (si están presentes se siguen
    verificando sus rangos); las claves ERB (erb_bands_measured,
    mean_coherence, spectral_error_rms_db, detected_issues) pasan a ser
    obligatorias; y derivation.eq debe documentar el mapeo clamp+residual
    (contener "clamp" y "residual"). Las verificaciones que sí aplican a
    un perfil vivo se mantienen duras: EQ por slot en [-2, 2],
    net_gain_db == 0.0, trim_db == -(suma de slots) (±0.01),
    operadores en [0, 1].

Salida 0 = válido; 1 = inválido (lista cada error). Solo stdlib.
No lee audio: valida estructura, tipos, rangos y consistencia interna
(ratios de energía que suman 1.0, net_gain_db == 0.0, split coherente
con el fingerprint). Que un archivo valide NO prueba que sus números
se midieran de un FLAC real: eso lo garantiza `derivation.*` (procedencia
auditable) y el procedimiento de medición, no este script.
"""
import argparse
import json
import sys

ERRORS = []

# Claves del fingerprint FLAC (modo por defecto) y ERB (modo --live-session).
FLAC_FP_KEYS = ["loudness_lufs_integrated", "loudness_rms_dbfs",
                "peak_dbfs", "crest_db", "spectrum_db_rel",
                "transient_energy_ratio", "tonal_energy_ratio",
                "stereo_correlation", "stereo_width_side_mid",
                "sample_rate_hz", "channels"]
ERB_FP_KEYS = ["erb_bands_measured", "mean_coherence",
               "spectral_error_rms_db", "detected_issues"]


def err(path, msg):
    ERRORS.append(f"{path}: {msg}")


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def check_keys(obj, required, path, allowed=None):
    if not isinstance(obj, dict):
        err(path, "debe ser un objeto")
        return False
    for k in required:
        if k not in obj:
            err(path, f"falta la clave obligatoria '{k}'")
    allowed_set = set(allowed) if allowed is not None else set(required)
    for k in obj:
        if k not in allowed_set:
            err(f"{path}.{k}", "clave desconocida en schema v1.0")
    return True


def check_str_list(obj, key, path):
    """Lista de strings no vacíos (o lista vacía); None si falta/no es lista."""
    if key not in obj:
        return None  # check_keys ya reportó la falta
    v = obj.get(key)
    if not isinstance(v, list):
        err(f"{path}.{key}", f"debe ser una lista, es {type(v).__name__}")
        return None
    for i, item in enumerate(v):
        if not isinstance(item, str) or not item:
            err(f"{path}.{key}[{i}]", "debe ser string no vacío")
    return v


def check_erb_bands(obj, key, path):
    """erb_bands_measured: {banda: {coherence en [0,1], error_db número}}."""
    if key not in obj:
        return None  # check_keys ya reportó la falta
    v = obj.get(key)
    if not isinstance(v, dict):
        err(f"{path}.{key}", f"debe ser un objeto, es {type(v).__name__}")
        return None
    for band, meas in v.items():
        bpath = f"{path}.{key}.{band}"
        if check_keys(meas, ["coherence", "error_db"], bpath):
            check_num(meas, "coherence", bpath, 0, 1)
            if "error_db" in meas and not is_num(meas.get("error_db")):
                err(f"{bpath}.error_db",
                    f"debe ser número, es {type(meas.get('error_db')).__name__}")
    return v


def check_num(obj, key, path, lo, hi, lo_exclusive=False, hi_exclusive=False):
    if key not in obj:
        return None  # check_keys ya reportó la falta
    v = obj.get(key)
    if not is_num(v):
        err(f"{path}.{key}", f"debe ser número, es {type(v).__name__}")
        return None
    bad_lo = v < lo if not lo_exclusive else v <= lo
    bad_hi = v > hi if not hi_exclusive else v >= hi
    if bad_lo or bad_hi:
        excl_lo = "(" if lo_exclusive else "["
        excl_hi = ")" if hi_exclusive else "]"
        err(f"{path}.{key}", f"{v} fuera de {excl_lo}{lo}, {hi}{excl_hi}")
    return v


def check_str(obj, key, path):
    if key not in obj:
        return None  # check_keys ya reportó la falta
    v = obj.get(key)
    if not isinstance(v, str) or not v:
        err(f"{path}.{key}", "debe ser string no vacío")
    return v


def validate_fingerprint_flac(fp):
    """Fingerprint FLAC (modo por defecto): todas las claves obligatorias."""
    check_num(fp, "loudness_lufs_integrated", "$.fingerprint", -70, 0)
    check_num(fp, "loudness_rms_dbfs", "$.fingerprint", -90, 0)
    check_num(fp, "peak_dbfs", "$.fingerprint", -120, 0, lo_exclusive=True)
    check_num(fp, "crest_db", "$.fingerprint", 0, 40)
    spec = fp.get("spectrum_db_rel")
    bands = ["sub", "low_60_120", "lowmid", "mid_500_1k", "highmid",
             "high_2k_4k", "presence_4k_8k", "air", "ultra"]
    if check_keys(spec, bands, "$.fingerprint.spectrum_db_rel"):
        for b in bands:
            check_num(spec, b, "$.fingerprint.spectrum_db_rel", -80, 0)
    tr = check_num(fp, "transient_energy_ratio", "$.fingerprint", 0, 1)
    to = check_num(fp, "tonal_energy_ratio", "$.fingerprint", 0, 1)
    if tr is not None and to is not None and abs((tr + to) - 1.0) > 0.01:
        err("$.fingerprint",
            f"transient+tonal = {tr + to}, debe ser 1.0 (±0.01)")
    check_num(fp, "stereo_correlation", "$.fingerprint", -1, 1)
    check_num(fp, "stereo_width_side_mid", "$.fingerprint", 0, 2)
    sr = fp.get("sample_rate_hz")
    if sr not in (44100, 48000, 96000, 192000):
        err("$.fingerprint.sample_rate_hz",
            f"{sr!r} no es una tasa soportada (44100/48000/96000/192000)")
    if fp.get("channels") not in (1, 2):
        err("$.fingerprint.channels", "debe ser 1 o 2")


def validate_fingerprint_live(fp):
    """Fingerprint de sesión en vivo (S4): claves ERB obligatorias, claves
    FLAC opcionales (si están presentes, se verifican sus rangos igual
    que en modo FLAC). Sin medición FLAC no hay ratios de energía que
    sumar ni tasa de muestreo que exigir."""
    if not check_keys(fp, ERB_FP_KEYS, "$.fingerprint",
                      allowed=ERB_FP_KEYS + FLAC_FP_KEYS):
        return
    check_erb_bands(fp, "erb_bands_measured", "$.fingerprint")
    check_num(fp, "mean_coherence", "$.fingerprint", 0, 1)
    # RMS de un error espectral: no negativo; 100 dB es cota de cordura.
    check_num(fp, "spectral_error_rms_db", "$.fingerprint", 0, 100)
    check_str_list(fp, "detected_issues", "$.fingerprint")
    # Claves FLAC opcionales: solo se verifican si están presentes.
    if "loudness_lufs_integrated" in fp:
        check_num(fp, "loudness_lufs_integrated", "$.fingerprint", -70, 0)
    if "loudness_rms_dbfs" in fp:
        check_num(fp, "loudness_rms_dbfs", "$.fingerprint", -90, 0)
    if "peak_dbfs" in fp:
        check_num(fp, "peak_dbfs", "$.fingerprint", -120, 0, lo_exclusive=True)
    if "crest_db" in fp:
        check_num(fp, "crest_db", "$.fingerprint", 0, 40)
    if "spectrum_db_rel" in fp:
        spec = fp.get("spectrum_db_rel")
        bands = ["sub", "low_60_120", "lowmid", "mid_500_1k", "highmid",
                 "high_2k_4k", "presence_4k_8k", "air", "ultra"]
        if check_keys(spec, bands, "$.fingerprint.spectrum_db_rel"):
            for b in bands:
                check_num(spec, b, "$.fingerprint.spectrum_db_rel", -80, 0)
    tr = to = None
    if "transient_energy_ratio" in fp:
        tr = check_num(fp, "transient_energy_ratio", "$.fingerprint", 0, 1)
    if "tonal_energy_ratio" in fp:
        to = check_num(fp, "tonal_energy_ratio", "$.fingerprint", 0, 1)
    if tr is not None and to is not None and abs((tr + to) - 1.0) > 0.01:
        err("$.fingerprint",
            f"transient+tonal = {tr + to}, debe ser 1.0 (±0.01)")
    if "stereo_correlation" in fp:
        check_num(fp, "stereo_correlation", "$.fingerprint", -1, 1)
    if "stereo_width_side_mid" in fp:
        check_num(fp, "stereo_width_side_mid", "$.fingerprint", 0, 2)
    if "sample_rate_hz" in fp:
        sr = fp.get("sample_rate_hz")
        if sr not in (44100, 48000, 96000, 192000):
            err("$.fingerprint.sample_rate_hz",
                f"{sr!r} no es una tasa soportada (44100/48000/96000/192000)")
    if "channels" in fp and fp.get("channels") not in (1, 2):
        err("$.fingerprint.channels", "debe ser 1 o 2")


def validate(data, path="$", live=False):
    if not isinstance(data, dict):
        err(path, "la raíz debe ser un objeto")
        return
    check_keys(data, ["format", "format_version", "track", "artist",
                      "source", "duration_s", "fingerprint", "engine",
                      "derivation"], path)
    if data.get("format") != "IVANNA OMEGA":
        err("$.format", "debe ser exactamente 'IVANNA OMEGA'")
    if data.get("format_version") != "1.0":
        err("$.format_version", "debe ser exactamente '1.0' (única versión)")
    check_str(data, "track", path)
    check_str(data, "artist", path)
    check_str(data, "source", path)
    if live:
        # Sesión en vivo continua: duration_s = 0 permitido.
        check_num(data, "duration_s", path, 0, 1e9)
    else:
        check_num(data, "duration_s", path, 0, 1e9, lo_exclusive=True)

    fp = data.get("fingerprint")
    if live:
        validate_fingerprint_live(fp)
    elif check_keys(fp, FLAC_FP_KEYS, "$.fingerprint"):
        validate_fingerprint_flac(fp)

    en = data.get("engine")
    if check_keys(en, ["orbe_amount", "orbe_distance_m", "holos_amount",
                       "phi_amount", "volterra_enabled", "volterra_mix",
                       "eq_bands_db", "compressor", "exciter_amount",
                       "trim_db", "net_gain_db", "structural_split"],
                  "$.engine"):
        check_num(en, "orbe_amount", "$.engine", 0, 1)
        check_num(en, "orbe_distance_m", "$.engine", 0.15, 2.0)
        check_num(en, "holos_amount", "$.engine", 0, 1)
        check_num(en, "phi_amount", "$.engine", 0, 1)
        if not isinstance(en.get("volterra_enabled"), bool):
            err("$.engine.volterra_enabled", "debe ser boolean")
        check_num(en, "volterra_mix", "$.engine", 0, 1)
        eq = en.get("eq_bands_db")
        eq_bands = ["low_60_120", "mid_500_1k", "high_2k_4k", "presence_4k_8k"]
        if check_keys(eq, eq_bands, "$.engine.eq_bands_db"):
            for b in eq_bands:
                check_num(eq, b, "$.engine.eq_bands_db", -2, 2)
        comp = en.get("compressor")
        if check_keys(comp, ["threshold_db", "ratio", "attack_ms",
                             "release_ms"], "$.engine.compressor"):
            check_num(comp, "threshold_db", "$.engine.compressor", -60, 0)
            check_num(comp, "ratio", "$.engine.compressor", 1, 20)
            check_num(comp, "attack_ms", "$.engine.compressor", 0, 1000,
                      lo_exclusive=True)
            check_num(comp, "release_ms", "$.engine.compressor", 0, 5000,
                      lo_exclusive=True)
        check_num(en, "exciter_amount", "$.engine", 0, 1)
        check_num(en, "trim_db", "$.engine", -12, 12)
        if live and isinstance(eq, dict):
            # S4: en sesión en vivo el trim debe compensar exactamente el
            # neto de los slots EQ (v1.1: slots +4.0 dB, trim -4.0 dB).
            # Solo en modo live: el modo FLAC no lo exigía y no se relaja
            # ni se endurece su contrato.
            slots = [eq.get(b) for b in eq_bands]
            trim = en.get("trim_db")
            if all(is_num(s) for s in slots) and is_num(trim):
                net = sum(slots)
                if abs(trim + net) > 0.01:
                    err("$.engine.trim_db",
                        f"{trim} != -(suma de slots EQ = {net}): el trim "
                        "debe compensar el neto de los slots "
                        "(ganancia neta 0.0 dB)")
        ng = en.get("net_gain_db")
        if is_num(ng) and abs(ng) > 1e-9:
            err("$.engine.net_gain_db",
                f"{ng} != 0.0: la Regla de Oro Matched-LUFS exige ganancia neta 0.0 dB")
        elif not is_num(ng):
            err("$.engine.net_gain_db", "debe ser número")
        split = en.get("structural_split")
        if check_keys(split, ["transient_amount", "tonal_amount",
                              "transient_energy_ratio", "tonal_energy_ratio"],
                      "$.engine.structural_split"):
            check_num(split, "transient_amount", "$.engine.structural_split", 0, 1)
            check_num(split, "tonal_amount", "$.engine.structural_split", 0, 1)
            str_ = check_num(split, "transient_energy_ratio",
                             "$.engine.structural_split", 0, 1)
            sto = check_num(split, "tonal_energy_ratio",
                            "$.engine.structural_split", 0, 1)
            if isinstance(fp, dict):
                ftr = fp.get("transient_energy_ratio")
                fto = fp.get("tonal_energy_ratio")
                if is_num(str_) and is_num(ftr) and abs(str_ - ftr) > 0.001:
                    err("$.engine.structural_split.transient_energy_ratio",
                        f"{str_} != fingerprint ({ftr})")
                if is_num(sto) and is_num(fto) and abs(sto - fto) > 0.001:
                    err("$.engine.structural_split.tonal_energy_ratio",
                        f"{sto} != fingerprint ({fto})")

    dv = data.get("derivation")
    if check_keys(dv, ["eq", "compressor", "structural_split", "loudness",
                       "note"], "$.derivation"):
        for k in ["eq", "compressor", "structural_split", "loudness", "note"]:
            check_str(dv, k, "$.derivation")
        if live:
            # S4: el perfil vivo debe documentar el mapeo clamp+residual:
            # qué se propuso, qué cabe en los slots ±2 dB y qué residual
            # queda sin aplicar (v1.1: +7.0 dB en brillo/presencia).
            eq_doc = dv.get("eq")
            if isinstance(eq_doc, str):
                low = eq_doc.lower()
                if "clamp" not in low or "residual" not in low:
                    err("$.derivation.eq",
                        "en sesión en vivo debe documentar el mapeo "
                        "clamp+residual (faltan 'clamp' y/o 'residual')")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    parser = argparse.ArgumentParser(
        description="Valida archivos .ivanna.json contra el schema v1.0.")
    parser.add_argument("--live-session", action="store_true",
                        help="modo sesión en vivo (S4): duration_s = 0 "
                        "permitido, claves FLAC opcionales, claves ERB "
                        "obligatorias y derivation.eq con el mapeo "
                        "clamp+residual")
    parser.add_argument("files", nargs="+", help="archivos .ivanna.json")
    args = parser.parse_args(argv[1:])
    rc = 0
    for fname in args.files:
        ERRORS.clear()
        try:
            with open(fname, "r", encoding="utf-8") as f:
                data = json.load(f)
        except FileNotFoundError:
            print(f"{fname}: NO EXISTE")
            rc = 1
            continue
        except json.JSONDecodeError as e:
            print(f"{fname}: JSON INVÁLIDO ({e})")
            rc = 1
            continue
        validate(data, "$", live=args.live_session)
        if ERRORS:
            print(f"{fname}: INVÁLIDO ({len(ERRORS)} errores)")
            for e in ERRORS:
                print(f"  - {e}")
            rc = 1
        else:
            track = data.get("track")
            artist = data.get("artist")
            mode = "schema v1.0 live-session" if args.live_session else "schema v1.0"
            print(f"{fname}: VÁLIDO ({mode}, {artist} — {track})")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
