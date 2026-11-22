#!/usr/bin/env python3
"""
genezi_calibrate.py — Calibrador ESS Farina ($0) del Sony Genezi MHC-PZ1D
para FÍSICA NUEVA (Fase 2, v2.5.472).

Ajusta los 21 coeficientes Laguerre-Volterra K2 + la respuesta H_b (24 bandas
ERB) desde grabaciones Farina hechas con el Moto G85, y emite los arreglos
que carga nativeLoadNewPhysicsCalibration(tri21L, tri21R, hb24, headroom24,
fLo, fHi).

NO modifica ~/workspace/genezi_physics_id.py (herramienta del dueño): replica
su matemática Farina (ESS 10 s 20 Hz–20 kHz, dt2/dt3, ventana h1 250 ms /
h2 50 ms) de forma autocontenida.

Uso:
  1. Generar los estímulos:
       python3 tools/genezi_calibrate.py --ess-out /tmp/ess
     (ess_lo_48k.wav, ess_hi_48k.wav, ess_inv_48k.npy)
  2. Reproducir ess_lo y ess_hi del Moto G85 al AUDIO IN del Genezi
     (<700 mV RMS), grabar cada uno con el mic del G85 a 15 cm del woofer.
  3. Ajustar:
       python3 tools/genezi_calibrate.py rec_lo.wav rec_hi.wav \\
           --inv /tmp/ess/ess_inv_48k.npy --out genezi_np_cal.json
  4. Verificación sin hardware (kernel Laguerre sintético conocido):
       python3 tools/genezi_calibrate.py --selftest

Supuestos honestos (documentados, no escondidos):
  * H2 se modela como FIR 1-D sobre x^2 (misma hipótesis que el propio
    ajuste Laguerre): y_h2 = h2 (x^2). El fit ajusta los 21 coeficientes
    Laguerre para reproducir esa salida H2.
  * hb24 es RESPUESTA RELATIVA (0 dB a 1 kHz): la ganancia absoluta la
    gobierna Matched-LUFS en otro lado, no este calibrador.
  * headroom24 = 6.0 dB plano (default conservador); refinarlo exige medir
    el techo de clip por banda, que este script no mide.
  * La recurrencia Laguerre y el pasa-bajos un polo a 1.5 kHz son IDÉNTICOS
    a app/src/main/cpp/neuromorphic/laguerre_volterra_h2.hpp — el fit y el
    runtime comparten la base exacta. (Nota: el alfa efectivo a 48 kHz es
    exp(-1/(0.0125*48000)) = 0.9983, tau 12.5 ms/polo; el "0.92" de la prosa
    del paquete no coincide con su propia fórmula; manda la fórmula.)
"""
import argparse
import json
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve

FS = 48000
F1, F2 = 20.0, 20000.0
T_SWEEP = 10.0

# Centros ERB de OmegaSigmaOperator (Band24Profile::kCentersHz).
ERB_CENTERS_HZ = [
    72.0, 102.0, 136.0, 174.0, 217.0, 266.0, 321.0, 384.0,
    455.0, 535.0, 626.0, 729.0, 845.0, 977.0, 1126.0, 1295.0,
    1486.0, 1703.0, 1948.0, 2226.0, 2540.0, 2896.0, 3300.0, 11500.0,
]


def fit_laguerre_k2_genezi(x_drive, y_meas_h2, fs=48000, P=6, tau=0.0125):
    """
    Ajusta los 21 coeficientes de Laguerre-Volterra K2 directamente para el Sony Genezi
    a partir de la señal de excitación x_drive (<1.5 kHz) y el residuo H2 extraído
    por Farina (desfasado -1.0034 s). Costo en C++: ~35 MAC/muestra.
    """
    alpha = np.exp(-1.0 / (tau * fs))
    s = np.sqrt(1.0 - alpha**2)
    N = len(x_drive)
    U = np.zeros((N, P), dtype=np.float64)
    u = np.zeros(P, dtype=np.float64)
    for n in range(N):
        prev = u[0]
        u[0] = s * x_drive[n] + alpha * u[0]
        for m in range(1, P):
            cp = u[m]
            u[m] = alpha * (u[m] - u[m - 1]) + prev
            prev = cp
        U[n, :] = u

    idx_i, idx_j = np.triu_indices(P)
    Phi = np.empty((N, len(idx_i)), dtype=np.float64)
    for col, (i, j) in enumerate(zip(idx_i, idx_j)):
        mult = 1.0 if i == j else 2.0
        Phi[:, col] = mult * U[:, i] * U[:, j]

    # Pre-distorsión inversa K2 = -H2 (acotada a ||k||_1 <= 0.35 para pasar puerta IVK1)
    k2 = -np.linalg.solve(Phi.T @ Phi + 1e-4 * np.eye(len(idx_i)), Phi.T @ y_meas_h2)
    l1 = np.sum(np.abs(k2))
    if l1 > 0.35:
        k2 *= (0.35 / l1)
    return k2.astype(np.float32)


def one_pole_lowpass_1500(x, fs=FS):
    """El mismo pasa-bajos un polo a 1.5 kHz que LaguerreVolterraH2 (C++)."""
    coef = 1.0 - np.exp(-2.0 * np.pi * 1500.0 / fs)
    y = np.empty_like(x, dtype=np.float64)
    lp = 0.0
    for n in range(len(x)):
        lp += coef * (x[n] - lp)
        y[n] = lp
    return y


def generate_farina_ess(fs=FS, f1=F1, f2=F2, T=T_SWEEP, amp=0.25):
    """ESS de Farina + filtro inverso analítico (idéntico a genezi_physics_id.py)."""
    N = int(fs * T)
    t = np.arange(N, dtype=np.float64) / fs
    R = np.log(f2 / f1)
    K = (2.0 * np.pi * f1 * T) / R
    phase = K * (np.exp((t / T) * R) - 1.0)
    sweep = amp * np.sin(phase)
    fade = int(0.02 * fs)
    w = np.ones(N)
    w[:fade] = 0.5 * (1 - np.cos(np.pi * np.arange(fade) / fade))
    w[-fade:] = 0.5 * (1 + np.cos(np.pi * np.arange(fade) / fade))
    sweep *= w
    inv = sweep[::-1] * np.exp(-(t[::-1] / T) * R)
    norm = np.max(np.abs(fftconvolve(sweep, inv)))
    inv /= norm
    return sweep, inv


def extract_h1_h2_h3(rec_wav, inv, fs=FS, f1=F1, f2=F2, T=T_SWEEP):
    """Separa h1 (t=0), h2 (dt2) y h3 (dt3) de una única grabación (Farina)."""
    ir = fftconvolve(rec_wav.astype(np.float64), inv, mode="full")
    t_peak = int(np.argmax(np.abs(ir)))
    R = np.log(f2 / f1)
    dt2 = int(round(T * np.log(2.0) / R * fs))
    dt3 = int(round(T * np.log(3.0) / R * fs))
    win_h1 = int(0.250 * fs)
    win_hk = int(0.050 * fs)
    h1 = ir[t_peak - int(0.005 * fs): t_peak + win_h1]
    p2 = t_peak - dt2
    p3 = t_peak - dt3
    h2 = ir[p2 - int(0.005 * fs): p2 + win_hk]
    h3 = ir[p3 - int(0.005 * fs): p3 + win_hk]
    return h1, h2, h3


def verify_square_law(h1_low, h2_low, h1_high, h2_high):
    """H2 debe subir +12 dB cuando H1 sube +6 dB (pendiente 2.00 = física real)."""
    d_h1 = 20.0 * np.log10(np.linalg.norm(h1_high) / (np.linalg.norm(h1_low) + 1e-12))
    d_h2 = 20.0 * np.log10(np.linalg.norm(h2_high) / (np.linalg.norm(h2_low) + 1e-12))
    slope = d_h2 / max(d_h1, 1e-6)
    print(f"ΔH1 = {d_h1:.2f} dB | ΔH2 = {d_h2:.2f} dB | Pendiente H2/H1 = {slope:.2f} (teórico = 2.00)")
    return abs(slope - 2.0) < 0.25, slope


def hb24_from_h1(h1, fs=FS):
    """Respuesta relativa H_b en las 24 bandas ERB, 0 dB a 1 kHz."""
    nfft = 1 << 15
    spec = np.fft.rfft(h1 * np.hanning(len(h1)), nfft)
    freqs = np.fft.rfftfreq(nfft, 1.0 / fs)
    mag = np.abs(spec) + 1e-12
    ref = np.interp(1000.0, freqs, mag)
    hb = []
    for fc in ERB_CENTERS_HZ:
        m = np.interp(fc, freqs, mag)
        hb.append(float(20.0 * np.log10(m / ref)))
    return np.array(hb, dtype=np.float64)


def read_wav_mono_or_stereo(path):
    """Devuelve (fs, array float64 forma (N,) o (N, 2)) normalizado a [-1, 1]."""
    fs, data = wavfile.read(path)
    if fs != FS:
        raise ValueError(f"{path}: se esperaba {FS} Hz, trae {fs} Hz")
    if data.dtype == np.int16:
        x = data.astype(np.float64) / 32768.0
    elif data.dtype == np.int32:
        x = data.astype(np.float64) / 2147483648.0
    else:
        x = data.astype(np.float64)
    return fs, x


def fit_channel(x_drive_lp, h2_kernel):
    """y_h2 = h2 ⊛ x^2 (kernel con su pico alineado a lag 0); ajusta tri21."""
    # El kernel extraído trae 5 ms de pre-ring: el pico (llegada directa H2)
    # está en el índice 0.005*fs; se alinea a lag 0.
    pre = int(0.005 * FS)
    h2k = h2_kernel[pre:]
    y_h2 = fftconvolve(h2k, x_drive_lp ** 2, mode="full")[: len(x_drive_lp)]
    return fit_laguerre_k2_genezi(x_drive_lp, y_h2, fs=FS), y_h2


def calibrate(rec_lo_path, rec_hi_path, inv_path):
    inv = np.load(inv_path)
    _, rec_lo = read_wav_mono_or_stereo(rec_lo_path)
    _, rec_hi = read_wav_mono_or_stereo(rec_hi_path)
    lo_chs = [rec_lo[:, 0], rec_lo[:, 1]] if rec_lo.ndim == 2 else [rec_lo]
    hi_chs = [rec_hi[:, 0], rec_hi[:, 1]] if rec_hi.ndim == 2 else [rec_hi]
    if len(lo_chs) != len(hi_chs):
        raise ValueError("rec_lo y rec_hi deben tener el mismo número de canales")

    # Excitación conocida: el ESS lo (amp 0.18), pasa-bajos un polo 1.5 kHz.
    sweep_lo, _ = generate_farina_ess(amp=0.18)
    x_lp = one_pole_lowpass_1500(sweep_lo)

    tri, hb = [], []
    ok_square = True
    for ch, (lo, hi) in enumerate(zip(lo_chs, hi_chs)):
        h1_lo, h2_lo, _ = extract_h1_h2_h3(lo, inv)
        h1_hi, h2_hi, _ = extract_h1_h2_h3(hi, inv)
        ok, slope = verify_square_law(h1_lo, h2_lo, h1_hi, h2_hi)
        ok_square = ok_square and ok
        print(f"Canal {ch}: pendiente cuadrática {slope:.2f} -> {'OK' if ok else 'SOSPECHOSO (no es H2 del Genezi)'}")
        k2, _ = fit_channel(x_lp, h2_lo)
        tri.append(k2)
        hb.append(hb24_from_h1(h1_lo))
        print(f"Canal {ch}: tri21 ||k||_1 = {np.sum(np.abs(k2)):.4f} (puerta <= 0.35)")

    tri_l = tri[0]
    tri_r = tri[1] if len(tri) > 1 else tri[0]
    hb24 = hb[0] if len(hb) == 1 else (hb[0] + hb[1]) / 2.0
    headroom24 = np.full(24, 6.0)
    return {
        "tri21L": [float(v) for v in tri_l],
        "tri21R": [float(v) for v in tri_r],
        "hb24": [float(v) for v in hb24],
        "headroom24": [float(v) for v in headroom24],
        "fLo": 50.0,
        "fHi": 18500.0,
        "squareLawOk": bool(ok_square),
    }


def fmt_c_array(name, vals, typ="f"):
    body = ", ".join(f"{v:.6g}{typ}" for v in vals)
    return f"static const float {name}[{len(vals)}] = {{ {body} }};"


def selftest():
    """Kernel Laguerre sintético conocido -> genera y -> ajusta -> verifica recuperación.
    El fit devuelve la PRE-DISTORSIÓN INVERSA (K2 = -H2), así que el objetivo
    es -k_true, no k_true."""
    rng = np.random.default_rng(1234)
    k_true = rng.normal(0.0, 0.03, size=21).astype(np.float32)
    k_true *= 0.30 / np.sum(np.abs(k_true))  # ||k||_1 = 0.30 (pasa puerta)
    # Generar y con la misma recurrencia que el fit (y que el C++).
    fs = FS
    t = np.arange(2 * fs, dtype=np.float64) / fs
    x = 0.5 * np.sin(2 * np.pi * 68.5 * t) + 0.2 * np.sin(2 * np.pi * 120.0 * t)
    x_lp = one_pole_lowpass_1500(x)
    alpha = np.exp(-1.0 / (0.0125 * fs))
    s = np.sqrt(1.0 - alpha**2)
    P = 6
    u = np.zeros(P)
    y = np.empty_like(x_lp)
    idx_i, idx_j = np.triu_indices(P)
    for n in range(len(x_lp)):
        prev = u[0]
        u[0] = s * x_lp[n] + alpha * u[0]
        for m in range(1, P):
            cp = u[m]
            u[m] = alpha * (u[m] - u[m - 1]) + prev
            prev = cp
        y2 = 0.0
        for col, (i, j) in enumerate(zip(idx_i, idx_j)):
            mult = 1.0 if i == j else 2.0
            y2 += mult * k_true[col] * u[i] * u[j]
        y[n] = y2
    k_fit = fit_laguerre_k2_genezi(x_lp, y, fs=fs)
    k_target = -k_true  # el fit devuelve la inversa (pre-distorsión)
    err = np.linalg.norm(k_fit - k_target) / (np.linalg.norm(k_target) + 1e-12)
    print(f"selftest: ||k_fit - (-k_true)|| / ||k_true|| = {err:.4f} (umbral 0.05)")
    print(f"selftest: ||k_fit||_1 = {np.sum(np.abs(k_fit)):.4f} (puerta <= 0.35)")
    return err < 0.05


def main(argv=None):
    ap = argparse.ArgumentParser(description="Calibrador Genezi ESS $0 -> tri21 + hb24 (Física Nueva)")
    ap.add_argument("rec_lo", nargs="?", help="grabación ESS lo (wav 48 kHz, mono/estéreo)")
    ap.add_argument("rec_hi", nargs="?", help="grabación ESS hi (+6 dB, wav 48 kHz)")
    ap.add_argument("--inv", default="ess_inv_48k.npy", help="filtro inverso Farina (.npy)")
    ap.add_argument("--out", default="genezi_np_cal.json", help="JSON de salida")
    ap.add_argument("--ess-out", help="genera ess_lo/hi + inv en DIR y sale")
    ap.add_argument("--selftest", action="store_true", help="verifica el ajuste con un kernel sintético")
    args = ap.parse_args(argv)

    if args.selftest:
        return 0 if selftest() else 1
    if args.ess_out:
        import os
        os.makedirs(args.ess_out, exist_ok=True)
        sweep_lo, inv = generate_farina_ess(amp=0.18)
        sweep_hi, _ = generate_farina_ess(amp=0.36)
        wavfile.write(os.path.join(args.ess_out, "ess_lo_48k.wav"), FS, (sweep_lo * 32767).astype(np.int16))
        wavfile.write(os.path.join(args.ess_out, "ess_hi_48k.wav"), FS, (sweep_hi * 32767).astype(np.int16))
        np.save(os.path.join(args.ess_out, "ess_inv_48k.npy"), inv)
        print(f"Listo en {args.ess_out}: reproduce ess_lo/hi del Moto G85 al Genezi y graba con el mic.")
        return 0
    if not args.rec_lo or not args.rec_hi:
        ap.error("faltan rec_lo y rec_hi (o usa --ess-out / --selftest)")
    result = calibrate(args.rec_lo, args.rec_hi, args.inv)
    with open(args.out, "w") as f:
        json.dump(result, f, indent=2)
    print(f"Guardado {args.out} (squareLawOk={result['squareLawOk']})")
    print(fmt_c_array("kNpTri21L", result["tri21L"]))
    print(fmt_c_array("kNpTri21R", result["tri21R"]))
    print("static const double kNpHb24[24] = { " + ", ".join(f"{v:.3f}" for v in result["hb24"]) + " };")
    print("static const double kNpHeadroom24[24] = { " + ", ".join(f"{v:.3f}" for v in result["headroom24"]) + " };")
    print(f"fLo={result['fLo']} fHi={result['fHi']}  -> nativeLoadNewPhysicsCalibration(tri21L, tri21R, hb24, headroom24, fLo, fHi)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
