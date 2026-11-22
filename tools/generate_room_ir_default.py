#!/usr/bin/env python3
"""Genera app/src/main/cpp/fused/room_ir_default.hpp a partir de rir_0101.wav.

Sala elegida: rir_0101.wav (dataset RIR de 200 habitaciones, metadata.csv):
  - volumen 46.0 m3 (3.5 x 2.9 x 4.6 m): la sala pequeña del dataset.
  - distancia fuente-mic 3.70 m: la más próxima a los 3.5 m reales de Esse
    al Genezi (sigma 3.5 m).
  - RT60 0.34 s: sala seca, tipo salón amueblado.
Cadena:
  1) WAV estéreo 16 kHz / 16-bit. Onset = primera muestra > 1% del pico.
  2) Recorte: la cola empieza en onset + 25 ms (solo reverberación tardía;
     el sonido directo y las primeras reflexiones NO van a la cola UPOLA,
     porque el camino WOLA ya lleva el directo; si entraran, habría un eco
     de 21.5 ms).
  3) Remuestreo 16 kHz -> 48 kHz (x3, interpolación lineal; la cola es
     ruido difuso en caída, el error de interpolación es despreciable y la
     cola se atenúa en HF por absorción del aire de todos modos).
  4) 3200 muestras @48 kHz = 66.7 ms (el cargador usa 128..3199 = 3072
     taps = 64 ms de cola UPOLA).
  5) MIMO 2x2 desde el estéreo real: LL = izquierda, RR = derecha,
     LR = izquierda x 0.3, RL = derecha x 0.3. El 0.3 es la correlación
     cruzada media de un campo difuso entre dos puntos separados por el
     ancho de cabeza (sinc(kd) promediado 100-2000 Hz ≈ 0.25-0.35).
Uso: python3 tools/generate_room_ir_default.py
"""
import math
import struct
import wave
from pathlib import Path

REPO = Path("/home/hatch/workspace/ivanna")
SRC = REPO / "app/src/main/assets/ivanna_omega/rir/rir_0101.wav"
OUT = REPO / "app/src/main/cpp/fused/room_ir_default.hpp"

TRIM_MS_AFTER_ONSET = 25.0
OUT_LEN = 3200
CROSSFEED = 0.3


def read_stereo_16k(path):
    w = wave.open(str(path))
    assert w.getnchannels() == 2 and w.getsampwidth() == 2, "se esperaba estéreo 16-bit"
    sr = w.getframerate()
    n = w.getnframes()
    raw = w.readframes(n)
    s = struct.unpack(f"<{len(raw) // 2}h", raw)
    left = [s[i] / 32768.0 for i in range(0, len(s), 2)]
    right = [s[i] / 32768.0 for i in range(1, len(s), 2)]
    return sr, left, right


def onset_idx(ch, peak):
    thr = 0.01 * peak
    for i, v in enumerate(ch):
        if abs(v) > thr:
            return i
    return 0


def resample3x(x):
    """Lineal x3: 16 kHz -> 48 kHz. out[k] interpola x[k/3]."""
    out = []
    n = len(x)
    for k in range(3 * n):
        pos = k / 3.0
        i = int(pos)
        frac = pos - i
        a = x[i]
        b = x[i + 1] if i + 1 < n else 0.0
        out.append(a + frac * (b - a))
    return out


def main():
    sr, left, right = read_stereo_16k(SRC)
    assert sr == 16000, sr
    peak = max(max(abs(v) for v in left), max(abs(v) for v in right))
    on = min(onset_idx(left, peak), onset_idx(right, peak))
    start = on + int(TRIM_MS_AFTER_ONSET * sr / 1000.0)
    need16 = (OUT_LEN + 2) // 3 + 2
    seg_l = left[start:start + need16]
    seg_r = right[start:start + need16]
    assert len(seg_l) >= need16 and len(seg_r) >= need16, "RIR demasiado corta"
    up_l = resample3x(seg_l)[:OUT_LEN]
    up_r = resample3x(seg_r)[:OUT_LEN]

    def rms(v):
        return math.sqrt(sum(x * x for x in v) / len(v))

    print(f"onset={on} muestras ({on / sr * 1000:.1f} ms), recorte desde {start} "
          f"(onset+{TRIM_MS_AFTER_ONSET:.0f} ms)")
    print(f"cola: {OUT_LEN} muestras @48k = {OUT_LEN / 48000 * 1000:.1f} ms")
    print(f"pico L={max(abs(v) for v in up_l):.4f} R={max(abs(v) for v in up_r):.4f} "
          f"rms L={rms(up_l):.4f} R={rms(up_r):.4f}")

    def fmt(name, vals):
        lines = [f"static constexpr float {name}[{len(vals)}] = {{"]
        for i in range(0, len(vals), 8):
            lines.append("    " + ", ".join(f"{v:.7f}f" for v in vals[i:i + 8]) + ",")
        lines.append("};")
        return "\n".join(lines)

    hdr = f"""// room_ir_default.hpp — COLA DE SALA POR DEFECTO (v2.5.483, GENERADO).
// Generado por tools/generate_room_ir_default.py desde
// app/src/main/assets/ivanna_omega/rir/rir_0101.wav (dataset RIR, 200 salas):
//   sala 3.5 x 2.9 x 4.6 m (46.0 m3), distancia 3.70 m, RT60 0.34 s.
//   Elegida por ser la sala pequeña del dataset con distancia más próxima
//   a los 3.5 m reales de Esse al Genezi.
// Cadena: onset (muestra {on}) + 25 ms de recorte (solo cola tardía, sin
// directo ni primeras reflexiones) -> remuestreo lineal 16 kHz -> 48 kHz
// -> 3200 muestras (66.7 ms; el cargador UPOLA usa 128..3199 = 64 ms).
// MIMO 2x2: LL = izquierda, RR = derecha, LR/RL = cruce x0.3 (correlación
// de campo difuso entre oídos, sinc(kd) promediado 100-2000 Hz ≈ 0.25-0.35).
// NO editar a mano: regenerar con el script.
#pragma once

namespace ivanna::newphys {{

inline constexpr int kRoomIrDefaultLen = {OUT_LEN};  // muestras @48 kHz por canal

{fmt("kRoomIrDefaultLL", up_l)}

{fmt("kRoomIrDefaultLR", [v * CROSSFEED for v in up_l])}

{fmt("kRoomIrDefaultRL", [v * CROSSFEED for v in up_r])}

{fmt("kRoomIrDefaultRR", up_r)}

}}  // namespace ivanna::newphys
"""
    OUT.write_text(hdr)
    print(f"escrito {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
