#!/usr/bin/env python3
"""validate_ivanna_profile.py — Valida un archivo .ivanna.json contra el
schema v1.0 (docs/IVANNA_OMEGA_FORMAT_SCHEMA.md).

Uso:
    python3 scripts/validate_ivanna_profile.py archivo.ivanna.json [otro...]

Salida 0 = válido; 1 = inválido (lista cada error). Solo stdlib.
No lee audio: valida estructura, tipos, rangos y consistencia interna
(ratios de energía que suman 1.0, net_gain_db == 0.0, split coherente
con el fingerprint). Que un archivo valide NO prueba que sus números
se midieran de un FLAC real: eso lo garantiza `derivation.*` (procedencia
auditable) y el procedimiento de medición, no este script.
"""
import json
import sys

ERRORS = []


def err(path, msg):
    ERRORS.append(f"{path}: {msg}")


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def check_keys(obj, required, path):
    if not isinstance(obj, dict):
        err(path, "debe ser un objeto")
        return False
    for k in required:
        if k not in obj:
            err(path, f"falta la clave obligatoria '{k}'")
    for k in obj:
        if k not in required:
            err(f"{path}.{k}", "clave desconocida en schema v1.0")
    return True


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


def validate(data, path="$"):
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
    check_num(data, "duration_s", path, 0, 1e9, lo_exclusive=True)

    fp = data.get("fingerprint")
    if check_keys(fp, ["loudness_lufs_integrated", "loudness_rms_dbfs",
                       "peak_dbfs", "crest_db", "spectrum_db_rel",
                       "transient_energy_ratio", "tonal_energy_ratio",
                       "stereo_correlation", "stereo_width_side_mid",
                       "sample_rate_hz", "channels"], "$.fingerprint"):
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


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    rc = 0
    for fname in argv[1:]:
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
        validate(data, "$")
        if ERRORS:
            print(f"{fname}: INVÁLIDO ({len(ERRORS)} errores)")
            for e in ERRORS:
                print(f"  - {e}")
            rc = 1
        else:
            track = data.get("track")
            artist = data.get("artist")
            print(f"{fname}: VÁLIDO (schema v1.0, {artist} — {track})")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
