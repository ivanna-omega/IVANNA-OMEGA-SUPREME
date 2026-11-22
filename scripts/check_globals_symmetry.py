#!/usr/bin/env python3
"""
check_globals_symmetry.py — Simetría de los atomics g_* entre el proceso
app (libivanna_omega.so) y el proceso audioserver (libomega_effect.so).

Lección 389 (2026-11-10): cada atomic nuevo g_* necesita definición en
AMBOS lados; si falta el stub, el NDK falla con `undefined symbol` y la
CI no publica. Este check es el guard de regresión de esa lección.

Invariante: para cada módulo con definición en AMBOS lados, los atomics
g_* (nombre, tipo, valor inicial) son IDÉNTICOS.

Pares bridge/effect (ambos archivos existen): holos, hyper, orbe, wfs.
Módulo hrtf: la definición app vive en audio_orchestrator.cpp (no en un
bridge) y la effect en hrtf_globals_effect.cpp — se compara igual.
Casos app-only documentados (nunca referenciados en el effect):
  omni: g_omni_audiogram_dirty, g_omni_yaw_rad.
  upmixing: g_upmixing_enabled, g_upmixing_immersivity (módulo entero).

Falla (exit 1) si:
- Un par existente difiere (nombre, tipo o valor inicial).
- Un atomic app-only aparece referenciado en el effect (omega_effect.cpp
  o *_globals_effect.cpp) — dejaría de ser app-only.
- Un atomic nuevo aparece en un solo lado sin estar en el registro
  APP_ONLY (asimetría no documentada).

Si un atomic deja de ser app-only (se cablea al effect), se QUITA del
registro y el check exige la simetría completa.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP_ROOT = ROOT / "app" / "src" / "main" / "cpp"

# Pares (lado app, lado effect) — ambos archivos existen.
PAIRS = [
    ("holos_controls_bridge.cpp", "holos_globals_effect.cpp"),
    ("hyper_controls_bridge.cpp", "hyper_globals_effect.cpp"),
    ("orbe_controls_bridge.cpp", "orbe_globals_effect.cpp"),
    ("wfs_controls_bridge.cpp", "wfs_globals_effect.cpp"),
]

# hrtf: la definición app no vive en un bridge sino en audio_orchestrator.
HRTF_APP = "audio_orchestrator.cpp"
HRTF_EFFECT = "hrtf_globals_effect.cpp"
HRTF_PREFIX = "g_hrtf_"

# Atomics app-only, documentados. Se verifican dos cosas: que sigan sin
# referencia en el effect, y que ningún atomic nuevo en un solo lado
# aparezca sin estar aquí.
APP_ONLY = {
    "omni": {"g_omni_audiogram_dirty", "g_omni_yaw_rad"},
    "upmixing": {"g_upmixing_enabled", "g_upmixing_immersivity"},
}

EFFECT_FILES = ["omega_effect.cpp"]


def effect_files() -> list:
    """Archivos del proceso audioserver (se calcula aquí, no en import,
    para que un CPP_ROOT monkeado en tests funcione)."""
    return EFFECT_FILES + sorted(
        p.name for p in CPP_ROOT.glob("*_globals_effect.cpp")
    )

_DEF_RE = re.compile(
    r"std::atomic<(\w+)>\s+(g_\w+)\s*(?:\{\s*([^}]*?)\s*\}|=\s*([^;]+?)\s*;)"
)


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def extract_globals(path: Path) -> dict:
    """name -> (type, init) de cada `std::atomic<T> g_x{init};`."""
    src = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    out = {}
    for m in _DEF_RE.finditer(src):
        init = m.group(3) if m.group(3) is not None else m.group(4)
        out[m.group(2)] = (m.group(1), re.sub(r"\s+", " ", init.strip()))
    return out


def _word_re(name: str) -> re.Pattern:
    return re.compile(r"\b" + re.escape(name) + r"\b")


def main() -> int:
    findings: list[str] = []

    # 1. Pares bridge/effect: definiciones idénticas.
    for bridge_name, effect_name in PAIRS:
        bridge = extract_globals(CPP_ROOT / bridge_name)
        effect = extract_globals(CPP_ROOT / effect_name)
        module = bridge_name.split("_")[0]
        for name in sorted(set(bridge) | set(effect)):
            if name not in bridge:
                findings.append(
                    f"  - {module}: {name} solo en effect "
                    f"({effect_name}); falta en {bridge_name}"
                )
            elif name not in effect:
                if name not in APP_ONLY.get(module, set()):
                    findings.append(
                        f"  - {module}: {name} solo en bridge "
                        f"({bridge_name}); falta en {effect_name} "
                        f"y no está en APP_ONLY"
                    )
            elif bridge[name] != effect[name]:
                findings.append(
                    f"  - {module}: {name} difiere — "
                    f"bridge {bridge[name]} vs effect {effect[name]}"
                )

    # 2. hrtf: app en audio_orchestrator.cpp, effect en hrtf_globals_effect.
    hrtf_app = {
        k: v for k, v in extract_globals(CPP_ROOT / HRTF_APP).items()
        if k.startswith(HRTF_PREFIX)
    }
    hrtf_effect = extract_globals(CPP_ROOT / HRTF_EFFECT)
    for name in sorted(set(hrtf_app) | set(hrtf_effect)):
        if name not in hrtf_app:
            findings.append(
                f"  - hrtf: {name} solo en effect ({HRTF_EFFECT}); "
                f"falta en {HRTF_APP}"
            )
        elif name not in hrtf_effect:
            findings.append(
                f"  - hrtf: {name} solo en app ({HRTF_APP}); "
                f"falta en {HRTF_EFFECT}"
            )
        elif hrtf_app[name] != hrtf_effect[name]:
            findings.append(
                f"  - hrtf: {name} difiere — app {hrtf_app[name]} "
                f"vs effect {hrtf_effect[name]}"
            )

    # 3. App-only: no deben aparecer referenciados en el effect.
    effect_src = {}
    for fname in effect_files():
        p = CPP_ROOT / fname
        if p.exists():
            effect_src[fname] = strip_comments(
                p.read_text(encoding="utf-8", errors="replace")
            )
    for module, names in APP_ONLY.items():
        for name in sorted(names):
            for fname, src in effect_src.items():
                if _word_re(name).search(src):
                    findings.append(
                        f"  - {module}: {name} es app-only pero se "
                        f"referencia en el effect ({fname})"
                    )

    # 4. El registro APP_ONLY debe ser exacto: cada entrada debe existir
    #    en su bridge (si la razón deja de aplicar, el check lo dice).
    for module, names in APP_ONLY.items():
        bridge_path = CPP_ROOT / f"{module}_controls_bridge.cpp"
        if not bridge_path.exists():
            findings.append(
                f"  - APP_ONLY[{module}]: no existe "
                f"{module}_controls_bridge.cpp"
            )
            continue
        defined = extract_globals(bridge_path)
        for name in sorted(names):
            if name not in defined:
                findings.append(
                    f"  - APP_ONLY[{module}]: {name} ya no está "
                    f"definido en {module}_controls_bridge.cpp "
                    f"(registro obsoleto)"
                )

    if findings:
        print("[FAIL] check_globals_symmetry: asimetría g_* app/effect")
        for f in findings:
            print(f)
        return 1
    print("[PASS] check_globals_symmetry: 0 asimetrías "
          f"({len(PAIRS)} pares + hrtf + {sum(len(v) for v in APP_ONLY.values())} app-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
