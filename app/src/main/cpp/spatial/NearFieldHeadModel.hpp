// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// NearFieldHeadModel — modelo de cabeza esférica en campo cercano (ORBE).
//
// Física: cabeza rígida esférica de radio a = 8.75 cm (el mismo que ya usa
// HeadphoneCrossfeed.hpp), velocidad del sonido c = 343 m/s. Para una fuente
// a azimut θ (0 = frontal, + = derecha) y distancia r, el ITD y el ILD se
// calculan con el modelo geométrico de rayos sobre la esfera (Kuhn /
// Duda & Martens): el camino al oído ipsilateral es directo; al oído
// contralateral el sonido bordea la esfera (tangente + arco). En campo
// lejano (r → ∞) el ITD se reduce exactamente a Woodworth:
//   ITD = (a/c)·(θ + sinθ)          (|θ| ≤ 90°)
//   ITD = (a/c)·(π − θ + sinθ)      (|θ| > 90°)
// y el ILD en graves → 0. Al acercarse la fuente, el ITD crece (~+12 % a
// 0.15 m / 90°) y el ILD en graves —casi nulo en campo lejano— crece hasta
// ~15–20 dB por divergencia esférica (presión ∝ 1/d) más sombra cefálica.
// Esto es física de difracción medida desde los años 90, no una invención.
//
// Funciones puras, sin estado, sin alloc: seguras en el hilo de audio y
// testeables sin audio (ver tests/test_near_field_head_model.cpp).
// Convenciones: itdSeconds > 0 significa que el oído IZQUIERDO llega más
// tarde (fuente a la derecha, θ > 0). ildDb > 0 significa que el oído
// DERECHO es más intenso (fuera a la derecha). Ambas son antisimétricas
// en θ. proximityShelfDb es el realce de graves del oído ipsilateral
// respecto a una referencia de 1 m, acotado a +6 dB.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>

namespace ivanna {

namespace nearfield {

inline constexpr double kHeadRadiusM = 0.0875;  // a = 8.75 cm
inline constexpr double kSoundSpeedMps = 343.0; // c
inline constexpr double kPi = 3.14159265358979323846;

// Longitud del camino acústico (rayos) desde una fuente a un oído.
// earSign: +1 oído derecho (+a, 0), −1 oído izquierdo (−a, 0).
// Fuente en (r·sinθ, r·cosθ): θ = 0 frontal, +θ hacia la derecha.
// El camino es el mínimo entre el rayo directo (si no atraviesa la
// esfera) y los dos caminos de borde (tangente + arco). El primero en
// llegar es el camino más corto (Fermat).
// headRadiusM: radio de cabeza personalizado (PsiFieldRuntime / ORBE);
// por defecto KEMAR 8.75 cm — sin él, bit-idéntico al modelo original.
inline double pathLengthToEar(double thetaRad, double r, double earSign,
                             double headRadiusM = kHeadRadiusM) noexcept {
    if (!std::isfinite(thetaRad) || !std::isfinite(r)) return 1.0;
    const double a = std::clamp(std::isfinite(headRadiusM) ? headRadiusM : kHeadRadiusM,
                               0.05, 0.15);
    const double rr = std::clamp(r, a + 0.01, 100.0);
    const double th = std::clamp(thetaRad, -kPi, kPi);
    const double sx = rr * std::sin(th);
    const double sy = rr * std::cos(th);
    const double ex = earSign * a;
    const double ey = 0.0;
    // Rayo directo.
    const double dx = sx - ex, dy = sy - ey;
    const double direct = std::sqrt(dx * dx + dy * dy);
    // ¿El segmento S→E atraviesa la esfera? Distancia del origen al segmento.
    // El punto del segmento es S − t·(S−E): t = (S·(S−E))/|S−E|².
    // Solo cuenta si el punto más cercano es INTERIOR (0 < t < 1): en los
    // extremos el punto es la fuente (fuera) o el oído (sobre la esfera,
    // distancia exactamente a) y el rayo directo no está bloqueado. Sin
    // esta distinción, el ruido double en el extremo hacía que el oído
    // ipsilateral pareciera bloqueado y el ITD colapsara a ~0.
    const double segLen2 = dx * dx + dy * dy;
    bool blocked = false;
    if (segLen2 > 1e-12) {
        const double t = (sx * dx + sy * dy) / segLen2;  // proyección del origen
        if (t > 1e-6 && t < 1.0 - 1e-6) {
            const double cx = sx - t * dx, cy = sy - t * dy;
            blocked = (cx * cx + cy * cy) < (a * a * (1.0 - 1e-6));
        }
    }
    double best = blocked ? 1.0e30 : direct;
    // Caminos de borde: tangente desde S + arco hasta el oído.
    const double tangLen = std::sqrt(rr * rr - a * a);
    const double gamma = std::atan2(sy, sx);              // ángulo de la fuente
    const double beta = std::acos(a / rr);                 // ángulo OS–OT
    const double earAng = std::atan2(ey, ex);              // 0 (der) o π (izq)
    for (int s = -1; s <= 1; s += 2) {
        const double tangAng = gamma + (double)s * beta;
        double dAng = tangAng - earAng;
        while (dAng > kPi) dAng -= 2.0 * kPi;
        while (dAng < -kPi) dAng += 2.0 * kPi;
        const double total = tangLen + a * std::fabs(dAng);
        if (total < best) best = total;
    }
    return best;
}

// ITD esférico (segundos): retardo del oído izquierdo menos el derecho.
// θ > 0 (fuente a la derecha) → ITD > 0. En r → ∞ coincide con Woodworth.
inline double itdSeconds(double thetaRad, double r,
                        double headRadiusM = kHeadRadiusM) noexcept {
    if (!std::isfinite(thetaRad) || !std::isfinite(r)) return 0.0;
    // FIX (CI 7e7fffee, test ItdIsZeroOnMedianPlane): en el plano medio
    // (θ == 0) la geometría es EXACTAMENTE simétrica y el ITD es 0.0
    // exacto. Sin el corto, las dos ramas de pathLengthToEar (earAng 0
    // vs π, con atan2/acos y envuelta de ángulo) difieren en el último
    // bit double (~1e-17 m) y el ITD sale ~1e-19 s en vez de 0.0. En
    // float el residuo se redondeaba a 0.0f y el test pasaba; en double
    // sobrevive. El corto devuelve el valor exacto, no una tolerancia.
    if (thetaRad == 0.0) return 0.0;
    const double pathL = pathLengthToEar(thetaRad, r, -1.0, headRadiusM);
    const double pathR = pathLengthToEar(thetaRad, r, +1.0, headRadiusM);
    return (pathL - pathR) / kSoundSpeedMps;
}

// Woodworth de campo lejano, para referencia y tests (r → ∞).
inline double itdWoodworthFarField(double thetaRad) noexcept {
    if (!std::isfinite(thetaRad)) return 0.0;
    const double a = kHeadRadiusM, c = kSoundSpeedMps;
    const double th = std::fabs(std::clamp(thetaRad, -kPi, kPi));
    if (th <= kPi * 0.5) return (a / c) * (th + std::sin(th));
    return (a / c) * (kPi - th + std::sin(th));
}

// ILD esférico (dB, derecho menos izquierdo): > 0 si la fuente está a la
// derecha. Suma divergencia esférica (1/d, distancia directa fuente→oído)
// y sombra cefálica dependiente de la frecuencia (ka = 2πfa/c).
inline double ildDb(double thetaRad, double r, double freqHz,
                   double headRadiusM = kHeadRadiusM) noexcept {
    if (!std::isfinite(thetaRad) || !std::isfinite(r) || !std::isfinite(freqHz)) return 0.0;
    const double a = std::clamp(std::isfinite(headRadiusM) ? headRadiusM : kHeadRadiusM,
                               0.05, 0.15);
    const double rr = std::clamp(r, a + 0.01, 100.0);
    const double th = std::clamp(thetaRad, -kPi, kPi);
    const double sx = rr * std::sin(th);
    const double sy = rr * std::cos(th);
    const double dR = std::sqrt((sx - a) * (sx - a) + sy * sy);  // fuente→oído der
    const double dL = std::sqrt((sx + a) * (sx + a) + sy * sy);  // fuente→oído izq
    const double spreadingDb = 20.0 * std::log10(dL / dR);
    const double f = std::clamp(freqHz, 0.0, 20000.0);
    const double ka = 2.0 * kPi * f * a / kSoundSpeedMps;
    // La sombra atenúa al oído CONTRALATERAL: suma al ILD si la fuente está
    // a la derecha (atenúa L) y resta si está a la izquierda (atenúa R).
    const double shadowMagDb = 20.0 * std::log10(1.0 + 0.25 * ka * ka * std::sin(th) * std::sin(th));
    const double shadowDb = (std::sin(th) >= 0.0) ? shadowMagDb : -shadowMagDb;
    return spreadingDb + shadowDb;
}

// Realce de proximidad esférico (dB) del oído ipsilateral respecto a una
// referencia de 1 m: solo graves (shelf con esquina ~300 Hz), acotado a
// +6 dB. A r ≥ 1 m es 0. Es el mismo fenómeno que el efecto de proximidad
// de un micrófono direccional, entre la fuente virtual y cada oído.
inline double proximityShelfDb(double r, double freqHz) noexcept {
    if (!std::isfinite(r) || !std::isfinite(freqHz)) return 0.0;
    if (r >= 1.0) return 0.0;
    const double rr = std::clamp(r, 0.10, 1.0);
    const double raw = 20.0 * std::log10(1.0 / rr);
    const double capped = std::min(raw, 6.0);
    const double f = std::clamp(freqHz, 0.0, 20000.0);
    const double shape = 1.0 / (1.0 + (f / 300.0) * (f / 300.0));
    return capped * shape;
}

// ── Parallax por traslación (v2.5.472, SO(3) + posición) ────────────────
// El oyente se mueve y la fuente virtual queda fija en el mundo: la
// geometría fuente→oído se recalcula respecto a la posición del oyente.
// listener: (x, y, z) en metros, marco centrado en la cabeza (+x frente,
// +y izquierda, +z arriba). azimuthRad/distM: azimut y distancia de la
// fuente respecto al centro de la cabeza SIN trasladar. Con listener
// (0,0,0) el azimut y la distancia efectivos son idénticos a la entrada
// (sin parallax inventado). La fuente vive en el plano horizontal (z=0);
// la z del oyente solo acorta/alarga la distancia efectiva.
struct ListenerVec {
    double x = 0.0, y = 0.0, z = 0.0;
};

inline void sourceRelativeToListener(double azimuthRad, double distM,
                                     const ListenerVec& listener,
                                     double& azOut, double& distOut) noexcept {
    const double r = std::isfinite(distM) ? distM : 1.0;
    const double th = std::isfinite(azimuthRad) ? azimuthRad : 0.0;
    const double sx = r * std::sin(th);
    const double sy = r * std::cos(th);
    const double rx = sx - listener.x;
    const double ry = sy - listener.y;
    const double rz = 0.0 - listener.z;
    azOut = std::atan2(rx, ry);
    distOut = std::sqrt(rx * rx + ry * ry + rz * rz);
}

// ITD/ILD con el oyente trasladado: mismo modelo esférico, con el azimut
// y la distancia efectivos. Sin traslación, bit-idénticos a itdSeconds /
// ildDb (la vía sin trasladar no se toca).
inline double itdSecondsTranslated(double azimuthRad, double distM,
                                  const ListenerVec& listener,
                                  double headRadiusM = kHeadRadiusM) noexcept {
    double az = 0.0, d = 1.0;
    sourceRelativeToListener(azimuthRad, distM, listener, az, d);
    return itdSeconds(az, d, headRadiusM);
}

inline double ildDbTranslated(double azimuthRad, double distM, double freqHz,
                             const ListenerVec& listener,
                             double headRadiusM = kHeadRadiusM) noexcept {
    double az = 0.0, d = 1.0;
    sourceRelativeToListener(azimuthRad, distM, listener, az, d);
    return ildDb(az, d, freqHz, headRadiusM);
}

// ── Envoltorios float (F2/486, compat) ────────────────────────────────────
// Misma matemática en double, salida redondeada a float. Los usan los
// tests y los llamadores legacy (ORBE, PsiField) hasta su conversión.
inline float pathLengthToEar(float thetaRad, float r, float earSign,
                             float headRadiusM = static_cast<float>(kHeadRadiusM)) noexcept {
    return static_cast<float>(pathLengthToEar(static_cast<double>(thetaRad),
                                              static_cast<double>(r),
                                              static_cast<double>(earSign),
                                              static_cast<double>(headRadiusM)));
}

inline float itdSeconds(float thetaRad, float r,
                        float headRadiusM = static_cast<float>(kHeadRadiusM)) noexcept {
    return static_cast<float>(itdSeconds(static_cast<double>(thetaRad),
                                         static_cast<double>(r),
                                         static_cast<double>(headRadiusM)));
}

inline float itdWoodworthFarField(float thetaRad) noexcept {
    return static_cast<float>(itdWoodworthFarField(static_cast<double>(thetaRad)));
}

inline float ildDb(float thetaRad, float r, float freqHz,
                   float headRadiusM = static_cast<float>(kHeadRadiusM)) noexcept {
    return static_cast<float>(ildDb(static_cast<double>(thetaRad),
                                    static_cast<double>(r),
                                    static_cast<double>(freqHz),
                                    static_cast<double>(headRadiusM)));
}

inline float proximityShelfDb(float r, float freqHz) noexcept {
    return static_cast<float>(proximityShelfDb(static_cast<double>(r),
                                               static_cast<double>(freqHz)));
}

}  // namespace nearfield

}  // namespace ivanna
