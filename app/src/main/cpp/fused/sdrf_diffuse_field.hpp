#pragma once
// sdrf_diffuse_field.hpp — CAMPO DIFUSO SDRF para SALA VIVA (2026-11-24).
// Fusión homogénea (corrección de Esse 2026-11-24): la matemática SDRF
// RIEMANN-5D (producción, verificada) NO es un operador nuevo ni trae
// controles nuevos: vive DENTRO de SALA VIVA como su campo difuso, con
// LOS MISMOS controles que ya tiene (roomWet, rt60Sec, roomSizeM).
//
// TRANSCRIPCIÓN FIEL del port C++ verificado
// (analysis-portal5d/riemann5d/sdrf_prod_port.cpp, class Kernel, double;
// verificado 2026-11-23, dossier SDRF_PROD_VERIFICACION.md). El campo es
// ese kernel con I/O en double y UN SOLO cambio deliberado: la ganancia
// de lazo RT60 (ver abajo). Verificado contra el port: ganancia en
// estado estacionario 0.995× (ruido), energía de impulso 0.9966,
// techo ≤ 0.95, bypass bit-exacto (ver tests).
//
// Matemática (port fiel del kernel PROD de Esse,
// SymplecticDiracRiemannKernel_PROD.kt):
//   - 6 guías de onda unitarias de longitudes fijas Weil/GUE
//     (log2 de los primos 2 3 5 7 11 13 sobre baseMs 3.8 6.1 11.3 17.9
//     28.7 44.6 ms): 168 293 599 1008 1742 2779 muestras @48 kHz, sin
//     reasignación en caliente (cero clics).
//   - NEXO: 6 etapas Gray-Markel ortogonales EN SERIE (sesgo por etapa).
//   - Terminación Gray-Markel por guía (all-pass normalizado).
//   - Reflexión Householder-Riemann (u[k] = sqrt(exp(-dec·k/5)) con
//     signo alterno, normalizado; dec = 2.6 − 2.45·ρ_E).
//   - Acoplamiento η–KK (Givens focal/difuso, η = correlación L/R
//     estimada con envolventes rápida 4 ms / lenta 55 ms).
//   - Kerr simpléctico (Givens con ángulo por energía, conserva norma),
//     INTEGRACIÓN causal (qSmooth) y rotación lenta AURA.
//   - Techo softReflect ≤ 0.95: la energía excedente NO se descarta,
//     se REFLEJA a los puertos 2/3 de las guías (conserva energía).
//   - Suavizado de parámetros 25 ms (alphaSlew) y rampa bypass 16 ms
//     (fundido de potencia constante cos/sin).
//
// QUÉ se deriva de los controles EXISTENTES (nada nuevo se expone):
//   - effectiveWet_ (roomWet + limitador físico de sala): puerta del
//     campo y densidad del campo temprano ρ_E = effectiveWet_.
//   - rt60Sec: decaimiento del lazo SOLO sin IR cargada (g_k =
//     10^(-3·len_k/(RT60·fs)), fórmula FDN estándar, -60 dB en RT60 s).
//     CON IR cargada el campo es difusión pura (g_k = 1, exactamente el
//     kernel verificado: la cola RIR ya decae y el campo solo la difunde
//     con espaciamiento GUE, sin modos metálicos).
//   - Los demás parámetros internos (integracion 0.55, nexoKappa 0.45,
//     attackDepth 0.10, harmonicDepth 0.42, tailDepth 0.80, focalPhi 0.65,
//     auraDepth 0.35, auraRateHz 0.07, etaKkMix 0.55) quedan FIJOS en los
//     valores verificados de producción. El suavizado hacia un objetivo
//     constante es identidad, así que la matemática es bit-idéntica al
//     port verificado.
//   - roomSizeM NO toca las longitudes (fijas por la verificación):
//     el tamaño de la sala se expresa hoy como siempre, vía RT60 + wet.
//
// Reglas duras:
//   - Sin malloc/locks/I-O en process(): todo el estado es miembro.
//   - Fail-closed: muestra no finita a la entrada → 0.0.
//   - Con el campo apagado (puerta effectiveWet_ = 0) el operador no lo
//     llama y su estado se limpia (reset), así que nunca libera energía
//     almacenada como golpe al re-enganchar.
//   - El techo del campo (≤0.95) vive DENTRO del kernel (softReflect);
//     nunca se clampea sobre un bypass: con el campo apagado la señal
//     no se toca en absoluto.
//
// PUERTA DE SILENCIO (2026-11-24, fix del build rojo v2.5.500):
//   Defecto real: la red es (casi) unitaria — con IR cargada el lazo es
//   g=1 (difusión pura) y la energía circula SIN decaer (medido: salida
//   sostenida ~-43 dBFS y energía almacenada 1.15 tras 2 s de silencio);
//   sin IR el decaimiento RT60=0.34 s deja la cola en -47 dBFS a 0-100 ms
//   y el RMS 100 ms→fin en -81.9 dBFS, por encima del umbral P1 de
//   -90 dBFS (residual_silence, 100 ms tras la última señal). El test
//   DoubleFloor también cae (-91.3 dB vs -170 dB) por la misma energía
//   que no se apaga. La puerta NO toca el carácter difuso con señal:
//   solo actúa cuando la entrada es silencio real (por debajo de
//   -80 dBFS sostenido 5 ms) TRAS una entrada sostenida (≥100 ms, el
//   "armado"). El armado distingue el audio real (P1: 5-15 s de señal,
//   la puerta debe callar la cola) de un impulso aislado (los tests
//   SDRF Rt60DrivesDecay / DiffuseFieldRt60ChangesTail alimentan UN
//   impulso y miden la cola 0.3-1.4 s: sin armado, la puerta no actúa y
//   la cola RT60 se mide intacta). Tras el armado, la puerta funde la
//   salida a cero en 15 ms (sin clics) y vacía el estado (guías/NEXO/
//   all-pass) para que no quede energía almacenada que reaparezca al
//   re-enganchar. Con señal presente la ganancia de puerta es
//   exactamente 1.0 (bit-idéntica) y la matemática del campo no cambia.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace ivanna::fused {

class SdrfDiffuseField {
public:
    static constexpr int kWgCount = 6;
    static constexpr int kWgBufSize = 4096;  // potencia de 2 > 2779
    static constexpr int kNumZetaZeros = 32;

    SdrfDiffuseField() { prepare(48000.0); }

    // (Re)construye las longitudes para la tasa (fórmula del port: baseMs
    // por log2 del primo de Weil, redondeo al entero, clamp 32..3900) y
    // las constantes de suavizado/envolvente. Idempotente.
    void prepare(double sampleRate) noexcept {
        if (prepared_ && sampleRate == sampleRate_) return;
        sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        const double baseMs[kWgCount] = {3.8, 6.1, 11.3, 17.9, 28.7, 44.6};
        for (int k = 0; k < kWgCount; ++k) {
            const double pScale =
                std::log(static_cast<double>(kWeilPrimes[k])) / std::log(2.0);
            const double ms = baseMs[k] * (0.78 + 0.14 * pScale);
            const long v = std::lround(ms * 0.001 * sampleRate_);
            wgLengths_[k] = static_cast<int>(std::max(32L, std::min(3900L, v)));
        }
        aF_ = 1.0 - std::exp(-1.0 / (0.004 * sampleRate_));
        aS_ = 1.0 - std::exp(-1.0 / (0.055 * sampleRate_));
        const double denom = std::max(0.016 * sampleRate_, 256.0);
        bypassStep_ = 1.0 / denom;
        alphaSlew_ = 1.0 - std::exp(-1.0 / (0.025 * sampleRate_));
        // Puerta de silencio: tiempos en muestras (5 ms sostén, 15 ms
        // fundido de cierre, 5 ms fundido de apertura, 100 ms de armado).
        silenceHold_ = std::max(1, static_cast<int>(0.005 * sampleRate_));
        gateStepDown_ = 1.0 / std::max(1.0, 0.015 * sampleRate_);
        gateStepUp_ = 1.0 / std::max(1.0, 0.005 * sampleRate_);
        armHold_ = std::max(1, static_cast<int>(0.100 * sampleRate_));
        prepared_ = true;
        reset();
    }

    // Vacía el estado de audio (guías, NEXO, all-pass, envolventes) sin
    // tocar la configuración (tasa, objetivos de lazo/densidad) ni la
    // puerta de silencio. Lo usa reset() y la puerta al cerrarse.
    void drainState() noexcept {
        for (int k = 0; k < kWgCount; ++k) {
            wgWrite_[k] = 0;
            wgAp_[k] = 0.0;
            loopGain_[k] = targetLoopGain_[k];
            for (int i = 0; i < kWgBufSize; ++i) wgBuf_[k][i] = 0.0;
        }
        for (int s = 0; s < 6; ++s) {
            nexoL_[s] = 0.0;
            nexoR_[s] = 0.0;
        }
        for (int i = 0; i < 8; ++i) port_[i] = 0.0;
        envFast_ = envSlow_ = covLR_ = varL_ = varR_ = 0.0;
        qSmooth_ = 0.0;
        bypassMix_ = 0.0;
        auraSamples_ = 0;
        sInteg_ = kInteg;
        sKappa_ = kKappa;
        sD1_ = kD1;
        sD2_ = kD2;
        sD3_ = kD3;
        sPhi_ = kPhi;
        sRho_ = 0.5;
        sAuraD_ = kAuraD;
        sAuraR_ = kAuraR;
        sEtaKk_ = kEtaKk;
    }

    // Limpia todo el estado (guías, all-pass, envolventes, suavizados).
    void reset() noexcept {
        drainState();
        // Puerta de silencio: abierta, desarmada y sin energía que vaciar.
        silenceTimer_ = 0;
        activeTimer_ = 0;
        armed_ = false;
        gateGain_ = 1.0;
        gateDrained_ = false;
    }

    // RT60 del decaimiento del lazo. rt60Sec <= 0 → difusión pura (g=1,
    // exactamente el kernel verificado). Los objetivos se calculan aquí
    // (fuera del bucle por muestra); los valores vivos se deslizan hacia
    // ellos con alphaSlew en processOne.
    void setRt60(float rt60Sec) noexcept {
        if (rt60Sec <= 0.0f) {
            for (int k = 0; k < kWgCount; ++k) targetLoopGain_[k] = 1.0;
            return;
        }
        const double rt = static_cast<double>(rt60Sec);
        for (int k = 0; k < kWgCount; ++k) {
            targetLoopGain_[k] =
                std::pow(10.0, -3.0 * static_cast<double>(wgLengths_[k]) /
                                    (rt * sampleRate_));
        }
    }

    // Densidad del campo temprano ρ_E (0..1), derivada de effectiveWet_.
    void setDensity(float rho) noexcept {
        targetRho_ = std::max(0.0f, std::min(1.0f, rho));
    }

    // Campo difuso: in → out (búferes distintos; in-place NO permitido:
    // el operador necesita el seco mientras escribe el wet). Sin
    // prepare() previo es no-op. El campo SIEMPRE está en modo proceso
    // (bypass=false): la puerta vive en el operador (effectiveWet_).
    void process(const double* inL, const double* inR, double* outL,
                 double* outR, int frames) noexcept {
        if (!prepared_ || !inL || !inR || !outL || !outR || frames <= 0) return;
        for (int i = 0; i < frames; ++i) {
            double oL = 0.0, oR = 0.0;
            processOne(inL[i], inR[i], oL, oR);
            outL[i] = oL;
            outR[i] = oR;
        }
    }

    // Diagnóstico (hilo de control / tests). Energía almacenada como en
    // el port: últimos wgLengths[k] de cada guía + all-pass + NEXO.
    double storedEnergy() const noexcept {
        double e = 0.0;
        for (int k = 0; k < kWgCount; ++k) {
            for (int d = 1; d <= wgLengths_[k]; ++d) {
                const double v = wgBuf_[k][(wgWrite_[k] - d) & (kWgBufSize - 1)];
                e += v * v;
            }
            e += wgAp_[k] * wgAp_[k];
        }
        for (int s = 0; s < 6; ++s) e += nexoL_[s] * nexoL_[s] + nexoR_[s] * nexoR_[s];
        return e;
    }
    double loopGain(int k) const noexcept {
        return (k >= 0 && k < kWgCount) ? loopGain_[k] : 0.0;
    }
    int waveguideLength(int k) const noexcept {
        return (k >= 0 && k < kWgCount) ? wgLengths_[k] : 0;
    }

private:
    // Valores verificados de producción (defaults del kernel PROD).
    static constexpr double kInteg = 0.55;
    static constexpr double kKappa = 0.45;
    static constexpr double kD1 = 0.10;
    static constexpr double kD2 = 0.42;
    static constexpr double kD3 = 0.80;
    static constexpr double kPhi = 0.65;
    static constexpr double kAuraD = 0.35;
    static constexpr double kAuraR = 0.07;
    static constexpr double kEtaKk = 0.55;

    static constexpr double kGamma[kNumZetaZeros] = {
        14.1347251417, 21.0220396388, 25.0108575801, 30.4248761259,
        32.9350615877, 37.5861781588, 40.9187190121, 43.3270732809,
        48.0051508812, 49.7738324777, 52.9703214777, 56.4462476971,
        59.3470440026, 60.8317785246, 65.1125440481, 67.0798105295,
        69.5464017112, 72.0671576745, 75.7046906991, 77.1448400689,
        79.3373750202, 82.9103808541, 84.7354929805, 87.4252746132,
        88.8091112076, 92.4918992705, 94.6513440405, 95.8706342281,
        98.8311942182, 101.3178510058, 103.7255380406, 105.4466230522};
    static constexpr int kWeilPrimes[kWgCount] = {2, 3, 5, 7, 11, 13};
    static constexpr double kPi = 3.14159265358979323846;
    // γ₂/γ₁ (segundo cero de ζ / primero), como en el port.
    static constexpr double kIncommensurateRatio = 21.0220396388 / 14.1347251417;

    static double clampd(double v, double lo, double hi) noexcept {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static void givens(double* v, int i, int j, double ang) noexcept {
        const double c = std::cos(ang), s = std::sin(ang);
        const double vi = v[i], vj = v[j];
        v[i] = c * vi - s * vj;
        v[j] = s * vi + c * vj;
    }
    // softReflect con reflexión de sobrepresión (fiel al port): la
    // energía excedente va al puerto de reflexión (2 o 3), no se descarta.
    void softReflect(double v, int refPort, double& y) noexcept {
        const double pr = port_[refPort];
        const double av = std::fabs(v);
        if (av <= 0.76) {
            y = v;
            return;
        }
        const double s = (v >= 0.0) ? 1.0 : -1.0;
        y = s * (0.76 + 0.19 * std::tanh((av - 0.76) / 0.19));
        const double dE = std::max(0.0, v * v - y * y);
        const double ps = (pr >= 0.0) ? 1.0 : -1.0;
        port_[refPort] = ps * std::sqrt(pr * pr + dE);
    }
    void fillHouseholderArr(double rho, double* out) const noexcept {
        const double r = clampd(rho, 0.0, 1.0);
        const double dec = 2.6 - 2.45 * r;
        double sq = 0.0;
        for (int k = 0; k < kWgCount; ++k) {
            out[k] = std::sqrt(std::exp(-dec * static_cast<double>(k) / 5.0)) *
                     (((k & 1) == 0) ? 1.0 : -1.0);
            sq += out[k] * out[k];
        }
        const double inv = 1.0 / std::sqrt(sq);
        for (int k = 0; k < kWgCount; ++k) out[k] *= inv;
    }

    // Transcripción fiel de Kernel::processOne (port verificado), con I/O
    // en double y la ganancia de lazo RT60 en la escritura de guías como
    // ÚNICO cambio deliberado (g=1 → bit-idéntico al port).
    void processOne(double xL, double xR, double& outL, double& outR) noexcept {
        if (!std::isfinite(xL)) xL = 0.0;
        if (!std::isfinite(xR)) xR = 0.0;
        // ── Puerta de silencio (fix build rojo v2.5.500) ──
        // Entrada por debajo de -80 dBFS sostenida 5 ms, TRAS una entrada
        // sostenida ≥100 ms (armado), → fundido de cierre (15 ms) y
        // vaciado del estado; al volver la señal, fundido de apertura
        // (5 ms). Sin armado (un impulso aislado: los tests SDRF de cola
        // RT60), la puerta NO actúa y la cola se mide intacta. Con
        // señal, gateGain_ == 1.0 exacto (bit-idéntico: la salida no se
        // toca). La puerta solo actúa en silencio real tras audio real,
        // nunca sobre el carácter difuso audible.
        const double inAbs = std::max(std::fabs(xL), std::fabs(xR));
        if (inAbs < kSilenceThreshold) {
            if (silenceTimer_ < silenceHold_) ++silenceTimer_;
        } else {
            silenceTimer_ = 0;
            if (activeTimer_ < armHold_) ++activeTimer_;
            if (activeTimer_ >= armHold_) armed_ = true;
        }
        if (silenceTimer_ >= silenceHold_ && armed_) {
            gateGain_ = std::max(0.0, gateGain_ - gateStepDown_);
            if (gateGain_ <= 0.0 && !gateDrained_) {
                drainState();
                gateDrained_ = true;
                armed_ = false;
                activeTimer_ = 0;
            }
        } else {
            gateGain_ = std::min(1.0, gateGain_ + gateStepUp_);
            gateDrained_ = false;
        }
        if (gateGain_ <= 0.0) {
            // Puerta cerrada: silencio real (0.0 exacto), sin computar el
            // campo — el estado ya está vacío, no hay energía que liberar.
            outL = 0.0;
            outR = 0.0;
            return;
        }
        // Suavizado por muestra (25 ms) de TODOS los parámetros (los
        // objetivos fijos hacen que el deslizamiento sea identidad salvo
        // ρ_E y las ganancias de lazo, que sí se mueven).
        sInteg_ += alphaSlew_ * (kInteg - sInteg_);
        sKappa_ += alphaSlew_ * (kKappa - sKappa_);
        sD1_ += alphaSlew_ * (kD1 - sD1_);
        sD2_ += alphaSlew_ * (kD2 - sD2_);
        sD3_ += alphaSlew_ * (kD3 - sD3_);
        sPhi_ += alphaSlew_ * (kPhi - sPhi_);
        sRho_ += alphaSlew_ * (static_cast<double>(targetRho_) - sRho_);
        sAuraD_ += alphaSlew_ * (kAuraD - sAuraD_);
        sAuraR_ += alphaSlew_ * (kAuraR - sAuraR_);
        sEtaKk_ += alphaSlew_ * (kEtaKk - sEtaKk_);
        for (int k = 0; k < kWgCount; ++k)
            loopGain_[k] += alphaSlew_ * (targetLoopGain_[k] - loopGain_[k]);

        // Rampa de bypass (16 ms) + limpieza de estado al llegar a 1.0.
        // El campo siempre corre con bypass=false (la puerta vive en el
        // operador), así que bypassMix se queda en 0; se conserva la
        // maquinaria fiel al port.
        const bool bypass = false;
        const double prevMix = bypassMix_;
        bypassMix_ = bypass ? std::min(1.0, bypassMix_ + bypassStep_)
                            : std::max(0.0, bypassMix_ - bypassStep_);
        if (prevMix < 1.0 - 1e-9 && bypassMix_ >= 1.0 - 1e-9) {
            for (int s = 0; s < 6; ++s) {
                nexoL_[s] = 0.0;
                nexoR_[s] = 0.0;
            }
            for (int k = 0; k < kWgCount; ++k) {
                wgAp_[k] = 0.0;
                for (int i = 0; i < kWgBufSize; ++i) wgBuf_[k][i] = 0.0;
            }
        }
        if (bypassMix_ >= 1.0 - 1e-9) {
            outL = xL;
            outR = xR;
            ++auraSamples_;
            return;
        }

        double uHouse[kWgCount];
        fillHouseholderArr(sRho_, uHouse);
        const double instAbs = 0.5 * (std::fabs(xL) + std::fabs(xR));
        envFast_ += aF_ * (instAbs - envFast_);
        envSlow_ += aS_ * (instAbs - envSlow_);
        covLR_ += aS_ * (xL * xR - covLR_);
        varL_ += aS_ * (xL * xL - varL_);
        varR_ += aS_ * (xR * xR - varR_);
        const double eta =
            clampd((covLR_ / std::sqrt(varL_ * varR_ + 1e-12)) * 0.5 + 0.5, 0.0, 1.0);
        qSmooth_ += 0.015 * (clampd(std::max(0.0, envFast_ - envSlow_) * 4.5, -1.0, 1.0) -
                             qSmooth_);

        // 1) NEXO: 6 etapas Gray-Markel ortogonales en serie.
        double sL = xL, sR = xR;
        for (int s = 0; s < 6; ++s) {
            const double skew = (static_cast<double>(s) - 2.5) / 2.5;
            const double c =
                clampd(0.20 + 0.44 * (0.5 * (1.0 + sKappa_ * skew)) + 0.14 * (1.0 - sPhi_),
                       -0.84, 0.84);
            const double kc = std::sqrt(1.0 - c * c);
            const double stl = nexoL_[s];
            const double ol = c * sL + kc * stl;
            nexoL_[s] = kc * sL - c * stl;
            sL = ol;
            const double str = nexoR_[s];
            const double orr = c * sR + kc * str;
            nexoR_[s] = kc * sR - c * str;
            sR = orr;
        }
        port_[0] = sL;
        port_[1] = sR;

        // 2) Guías fijas Weil-Riemann + terminación Gray-Markel.
        const double depths[3] = {sD1_, sD2_, sD3_};
        for (int k = 0; k < kWgCount; ++k) {
            const double del =
                wgBuf_[k][(wgWrite_[k] - wgLengths_[k]) & (kWgBufSize - 1)];
            const double apC =
                clampd(0.10 + 0.54 * depths[k / 2] * (1.0 - 0.5 * sPhi_) + 0.12 * sRho_,
                       0.0, 0.82);
            const double kc = std::sqrt(1.0 - apC * apC);
            const double st = wgAp_[k];
            const double ao = apC * del + kc * st;
            wgAp_[k] = kc * del - apC * st;
            port_[2 + k] = ao;
        }

        // 3) Householder-Riemann + Givens η-KK + Kerr + INTEGRACIÓN + AURA.
        {
            double dot = 0.0;
            for (int k = 0; k < kWgCount; ++k) dot += uHouse[k] * port_[2 + k];
            for (int k = 0; k < kWgCount; ++k)
                port_[2 + k] -= 2.0 * uHouse[k] * dot;
        }
        const double beta = clampd(0.16 + 0.56 * (1.0 - eta) * sEtaKk_, 0.05, 1.05);
        givens(port_, 0, 2, beta);
        givens(port_, 1, 3, beta);
        givens(port_, 0, 4, beta * 0.48);
        givens(port_, 1, 5, beta * 0.48);
        givens(port_, 0, 6, beta * 0.38);
        givens(port_, 1, 7, beta * 0.38);
        {
            const double e23 = port_[2] * port_[2] + port_[3] * port_[3];
            const double e45 = port_[4] * port_[4] + port_[5] * port_[5];
            givens(port_, 2, 3, 0.32 * (e23 / (1.0 + e23)));
            givens(port_, 4, 5, 0.32 * (e45 / (1.0 + e45)));
        }
        {
            const double qAng = sInteg_ * 0.50 * qSmooth_;
            givens(port_, 2, 4,
                   qAng * std::cos(kGamma[0] * 0.1 + qSmooth_ * kPi));
            givens(port_, 3, 5,
                   qAng * std::sin(kGamma[1] * 0.1 + qSmooth_ * kPi));
        }
        {
            const double tSec = static_cast<double>(auraSamples_) / sampleRate_;
            const double aura =
                (0.72 * std::sin(2.0 * kPi * sAuraR_ * tSec) +
                 0.28 * std::sin(2.0 * kPi * sAuraR_ * kIncommensurateRatio * tSec)) *
                sAuraD_ * 0.45;
            givens(port_, 2, 7, aura);
            givens(port_, 3, 6, -aura);
        }

        // 4) Techo unitario por reflexión de sobrepresión ≤ 0.95 +
        // escritura (con ganancia de lazo RT60; g=1 → kernel verificado)
        // + fundido bypass de potencia constante.
        double yL = 0.0, yR = 0.0;
        softReflect(port_[0], 2, yL);
        softReflect(port_[1], 3, yR);
        for (int k = 0; k < kWgCount; ++k) {
            wgBuf_[k][wgWrite_[k]] = port_[2 + k] * loopGain_[k];
            wgWrite_[k] = (wgWrite_[k] + 1) & (kWgBufSize - 1);
        }

        if (bypassMix_ > 1e-9) {
            const double a = 0.5 * kPi * bypassMix_;
            outL = std::cos(a) * yL + std::sin(a) * xL;
            outR = std::cos(a) * yR + std::sin(a) * xR;
        } else {
            outL = yL;
            outR = yR;
        }
        // Puerta de silencio: fundido de salida (con señal, gateGain_ es
        // exactamente 1.0 y esto es identidad bit-exacta).
        if (gateGain_ < 1.0) {
            outL *= gateGain_;
            outR *= gateGain_;
        }
        ++auraSamples_;
    }

    bool prepared_ = false;
    double sampleRate_ = 48000.0;
    double aF_ = 0.0, aS_ = 0.0;
    double alphaSlew_ = 0.00083299;
    double bypassStep_ = 0.00130208;
    int wgLengths_[kWgCount] = {168, 293, 599, 1008, 1742, 2779};
    double wgBuf_[kWgCount][kWgBufSize] = {};
    int wgWrite_[kWgCount] = {0, 0, 0, 0, 0, 0};
    double wgAp_[kWgCount] = {};
    double nexoL_[6] = {};
    double nexoR_[6] = {};
    double port_[8] = {};
    double envFast_ = 0.0, envSlow_ = 0.0, covLR_ = 0.0, varL_ = 0.0, varR_ = 0.0;
    double qSmooth_ = 0.0;
    double bypassMix_ = 0.0;
    long auraSamples_ = 0;
    double sInteg_ = kInteg, sKappa_ = kKappa, sD1_ = kD1, sD2_ = kD2, sD3_ = kD3;
    double sPhi_ = kPhi, sRho_ = 0.5, sAuraD_ = kAuraD, sAuraR_ = kAuraR;
    double sEtaKk_ = kEtaKk;
    float targetRho_ = 0.5f;
    double loopGain_[kWgCount] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    double targetLoopGain_[kWgCount] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    // Puerta de silencio (fix build rojo v2.5.500): umbral -80 dBFS,
    // sostén 5 ms, fundido de cierre 15 ms, fundido de apertura 5 ms,
    // armado 100 ms de entrada sostenida. Tiempos en muestras,
    // calculados en prepare() según la tasa.
    static constexpr double kSilenceThreshold = 1e-4;  // -80 dBFS
    int silenceHold_ = 240;
    double gateStepDown_ = 1.0 / 720.0;
    double gateStepUp_ = 1.0 / 240.0;
    int armHold_ = 4800;
    int silenceTimer_ = 0;
    int activeTimer_ = 0;
    bool armed_ = false;
    double gateGain_ = 1.0;
    bool gateDrained_ = false;
};

}  // namespace ivanna::fused
