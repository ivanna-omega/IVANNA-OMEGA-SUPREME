// physical_head_model.hpp — MODELO FÍSICO DE CABEZA: DISPERSIÓN EXACTA POR
// ESFERA RÍGIDA (piso físico de ORBE·Ω, 2026-11-18).
//
// Reemplaza la heurística "síntesis analítica Rayleigh" de HrtfManager
// (ILD = clamp(sin θ · escala · 0.25, ±0.45), ITD entero en muestras):
// aquí la HRTF sale de resolver la ecuación de onda, no de ajustar una curva.
//
// PROBLEMA RESUELTO (exacto): onda plana armónica incidente sobre una esfera
// rígida de radio a, presión en un punto de la superficie. Con convención
// temporal e^{-iωt} y γ = ángulo entre la DIRECCIÓN DE PROPAGACIÓN (-s,
// s = dirección hacia la fuente) y el punto de observación:
//
//   H(ka, γ) = Σ_{n=0}^{∞} (2n+1) iⁿ [ jₙ(ka) − jₙ′(ka)/hₙ′(ka) · hₙ(ka) ] Pₙ(cos γ)
//
//   jₙ, yₙ : Bessel esféricas; hₙ = jₙ + i·yₙ (Hankel 1ª especie, onda saliente);
//   Pₙ     : Legendre; ka = 2πf·a/c; H = p_superficie / p_campo_libre(centro).
//
// La condición de contorno rígida (velocidad radial nula en r = a) fija el
// coeficiente de dispersión Aₙ = −jₙ′(ka)/hₙ′(ka); ningún parámetro se ajusta
// a datos. Límites analíticos que el test verifica:
//   ka → 0 : H → 1 (sin sombra a baja frecuencia).
//   γ = π (punto frontal, de cara a la fuente), ka → ∞ : |H| → 2 (presión doble).
//   γ = 0 (antípoda), ka grande : |H| ≈ 0.74 — "bright spot": la esfera NO
//   produce sombra profunda justo en la antípoda; la sombra profunda vive en
//   γ ≈ 10–40° (justo fuera del bright spot, que es estrecho, ~ka^{-1/3} rad:
//   −8…−16 dB a 8 kHz). A 90° de azimut el oído contralateral cae EXACTAMENTE
//   en el bright spot, y por eso la esfera subestima el ILD medido a alta
//   frecuencia. Esto es un artefacto real del modelo esférico (la cabeza
//   real no es una esfera) y es una de las razones por las que la esfera NO
//   reproduce la HRTF medida a alta frecuencia.
//
// CONVENCIÓN DE AZIMUT (oídos en el eje interaural, ±y; fuente en el plano
// horizontal, azimut θ = 0 frontal, + hacia la IZQUIERDA):
//   cos γ_L = −sin θ,  cos γ_R = +sin θ.
// A θ = +90° (fuente a la izquierda) el oído izquierdo es el frontal
// (γ_L = π, presión doble) y el derecho es la antípoda (bright spot).
// Esta es la misma convención que el SOFA MIT KEMAR (receptor 0 en +y).
//
// ALCANCE HONESTO: la esfera explica ITD/ILD de baja frecuencia y la sombra
// de alta frecuencia a grandes rasgos, pero NO tiene pabellón: no produce
// los notches espectrales de 5–10 kHz ni la resonancia del canal auditivo.
// Esos cues los modela el módulo compañero geodesic_pinna_model.hpp
// (interferencia geodésica por el borde del pabellón). Ver
// analysis-ivanna/PHYSICAL_HEAD_MODEL_2026-11-18.md para la comparación
// cuantitativa contra el SOFA MIT KEMAR medido.
//
// RENDIMIENTO: funciones puras, doble precisión, sin I/O, sin malloc en el
// camino caliente (el llamador pasa un Workspace pre-reservado). La serie se
// trunca en N = ceil(ka) + 20 (convergencia < 1e-12 verificada en test).
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace ivanna::physical {

// Radio de cabeza KEMAR (el mismo que fused_spatial.hpp::kHeadRadiusM).
// Nota: los receptores del SOFA MIT KEMAR están en y = ±0.09 m; la esfera
// usa 0.0875 m (valor del proyecto). La diferencia (2.5 mm) desplaza el ITD
// máximo ~2.9 % (~20 µs a 90°); se documenta, no se esconde.
inline constexpr double kHeadRadiusM = 0.0875;
inline constexpr double kSpeedOfSoundMps = 343.0;
inline constexpr double kMaxModelFrequencyHz = 16000.0;

// Orden de truncado de la serie para un ka dado (convergencia < 1e-12).
// N = ceil(ka) + 20, acotado a [1, 200].
int rigidSphereOrder(double ka) noexcept;

// Workspace pre-reservado: cero malloc por llamada a rigidSphereTransfer().
// Capacidad para ka <= 2π·20000·a/c con a <= 0.12 m (N <= ~90).
struct RigidSphereWorkspace {
    std::vector<double> jn, yn, jnDeriv, ynDeriv, legendre, jDown;
    std::vector<std::complex<double>> hn;
    void ensure(std::size_t n);
};

// Función de transferencia compleja de la esfera rígida en un punto de la
// superficie, H = p_sup / p_campo_libre(centro).
//   freqHz   : frecuencia (> 0; f <= 0 devuelve 1 + 0i, límite ka -> 0).
//   cosGamma : coseno del ángulo entre dirección de propagación y el punto.
//   radiusM  : radio de la esfera (> 0; <= 0 devuelve 0, fail-closed).
//   speedOfSoundMps : c (> 0; <= 0 devuelve 0, fail-closed).
std::complex<double> rigidSphereTransfer(double freqHz, double cosGamma,
                                         double radiusM, double speedOfSoundMps,
                                         RigidSphereWorkspace& ws) noexcept;

// Variante cómoda (reserva su propio workspace; NO usar en el camino RT).
std::complex<double> rigidSphereTransfer(double freqHz, double cosGamma,
                                         double radiusM = kHeadRadiusM,
                                         double speedOfSoundMps = kSpeedOfSoundMps);

// Transferencia binaural para una fuente en el plano horizontal.
//   azimuthRad : 0 = frontal, + hacia la izquierda (radianes).
// Devuelve {H_izquierdo, H_derecho}.
struct EarTransfer {
    std::complex<double> left;
    std::complex<double> right;
};
EarTransfer rigidSphereEarTransfer(double freqHz, double azimuthRad,
                                   double radiusM, double speedOfSoundMps,
                                   RigidSphereWorkspace& ws) noexcept;
EarTransfer rigidSphereEarTransfer(double freqHz, double azimuthRad,
                                   double radiusM = kHeadRadiusM,
                                   double speedOfSoundMps = kSpeedOfSoundMps);

// ILD en dB (20·log10 |H_L/H_R|) a una frecuencia y azimut.
double rigidSphereIldDb(double freqHz, double azimuthRad,
                        double radiusM = kHeadRadiusM,
                        double speedOfSoundMps = kSpeedOfSoundMps) noexcept;

// ITD (µs) por retardo de grupo de la fase interaural en [fLo, fHi].
// Ajuste lineal de la fase desenvuelta de H_L/H_R; válido mientras
// ITD·f < ~0.5 ciclos en la banda (usar banda baja, p. ej. 150–500 Hz).
// Positivo = el oído izquierdo llega antes (fuente a la izquierda).
double rigidSphereItdMicroseconds(double azimuthRad, double fLoHz, double fHiHz,
                                  double radiusM = kHeadRadiusM,
                                  double speedOfSoundMps = kSpeedOfSoundMps) noexcept;

// Síntesis de HRIR (512 taps por defecto) por IFFT de H(f) muestreada en una
// rejilla FFT. H está normalizada al campo libre en el centro, así que la
// HRIR es NO-CAUSAL: el frente llega al oído cercano ~a/c ANTES que al
// centro. Por eso se aplica un retardo común `delaySeconds` (típ. 2 ms) y la
// energía queda dentro de la ventana; sin él la IFFT circular envuelve la
// respuesta. outL/outR: buffers de numTaps doubles (numTaps potencia de 2
// no requerida; la FFT interna usa el siguiente >= 2·numTaps).
// Devuelve false si los argumentos son inválidos (sin tocar los buffers).
bool synthesizeRigidSphereHrir(double azimuthRad, double sampleRateHz, int numTaps,
                               double delaySeconds, double* outLeft, double* outRight,
                               double radiusM = kHeadRadiusM,
                               double speedOfSoundMps = kSpeedOfSoundMps) noexcept;

}  // namespace ivanna::physical
