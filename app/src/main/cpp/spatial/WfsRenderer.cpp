// WfsRenderer.cpp — ver header para la descripción del modelo físico y la
// arquitectura glitch-free (eliminación de micro-cortes).
#include "WfsRenderer.hpp"

// Cadena de proteccion profesional instanciada en el pipeline real (mision
// WFS, RESTAURADA 2026-09-24 — cc23618c "Volterra H2/Hexagon cDSP/ATI" la
// habia eliminado por accidente, revirtiendo a un softLimit() suelto sin DC
// removal ni denormal guard; la clase seguia intacta en WfsRenderer.hpp,
// solo le faltaba el cableado): DC removal + denormal guard + soft limit +
// true-peak, aplicada por muestra a la salida WFS antes de mezclarse.
// Cubierta por test_wfs_adaptive_protection.
// FALLO 6 (2026-11-18): la cadena vivía aquí como thread_local compartido
// (g_wfsProtection + g_wfsProtectionInit + g_wfsProtectionSampleRate) entre
// todas las instancias del mismo hilo. Ahora es miembro por instancia
// (WfsRenderer::wfsProtection_, ver header) y la inicialización lazy por SR
// vive en WfsRenderer::ensureWfsProtection(). El thread_local se ELIMINA:
// su semántica de compartir estado entre instancias ERA el bug, así que no
// se conserva como legacy.

#if defined(__x86_64__) || defined(__i386__)
  #include <immintrin.h>
#endif

namespace {
// Guarda anti-denormales FTZ/DAZ (mismo patron que hrtf_convolver.cpp):
// las lineas de delay y los one-poles de suavizado arrastran residuos
// subnormales (~1e-38) donde la CPU degrada 10-100x por microcode assist
// -> picos de carga en el hilo de audio -> underruns = micro-cortes.
inline void enableWfsDenormalGuard() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
    uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
#endif
}
} // namespace

namespace ivanna::spatial {

template <typename Scalar>
bool WfsRendererT<Scalar>::init(float sampleRate, int blockSize, int numSpeakers) noexcept {
    if (sampleRate <= 0.f || blockSize <= 0) return false;
    if (numSpeakers < 4) numSpeakers = 4;
    if (numSpeakers > kMaxSpeakers) numSpeakers = kMaxSpeakers;
    sampleRate_  = sampleRate;
    blockSize_   = blockSize;
    // Fase 2 (geometría 3D real): si ya se cargó un layout explícito
    // (setSpeakerLayout3D llamado antes de esta init(), p.ej. desde
    // omega_apply_snapshot antes de la primera activación perezosa de
    // WFS) se respeta su numSpeakers_ (7) — init() nunca pisa una
    // geometría real ya cargada con el parámetro genérico.
    if (!explicitLayout_) {
        numSpeakers_ = numSpeakers;
    }

    // Delay máximo necesario: fuente más lejana permitida + diámetro del
    // array (peor caso geométrico) + margen de un bloque.
    const float maxDistS = (kMaxObjectDist + 2.f * kArrayRadiusM) / kSpeedOfSound;
    maxDelayTap_ = static_cast<int>(maxDistS * sampleRate_) + blockSize_ + 4;

    if (!explicitLayout_) {
        rebuildGeometry();
    }

    // FIX (guarda de anti-aliasing espacial, antes inalcanzable): separación
    // real entre altavoces adyacentes = circunferencia / N. Con los valores
    // por defecto (16 altavoces, radio 1.5 m) da f_alias ≈ 291 Hz.
    constexpr float kPi = 3.14159265358979323846f;
    const float speakerSpacingM = (2.f * kPi * kArrayRadiusM) / static_cast<float>(numSpeakers_);
    aliasGuard_.init(speakerSpacingM, static_cast<float>(sampleRate_));

    // Preasignación TOTAL: kMaxObjects líneas de fuente + kMaxObjects ×
    // numSpeakers taps. process() jamás asigna memoria.
    for (auto& slot : slots_) {
        slot = SourceSlot{};
        slot.line.assign(static_cast<size_t>(maxDelayTap_), Scalar(0.0));
    }
    taps_.assign(static_cast<size_t>(kMaxObjects) * static_cast<size_t>(numSpeakers_),
                 DelayTap{});
    numActiveObjects_ = 0;
    return true;
}

// FALLO 6 (2026-11-18): antes era una función libre sobre el thread_local
// g_wfsProtection; ahora es miembro: cada instancia inicializa SU cadena
// solo cuando cambia la SR (init()/reset() no la tocan, misma semántica
// lazy que antes, pero sin estado compartido entre instancias).
template <typename Scalar>
void WfsRendererT<Scalar>::ensureWfsProtection(float sampleRate) noexcept {
    if (!wfsProtectionInit_ || wfsProtectionSampleRate_ != sampleRate) {
        wfsProtection_.init(sampleRate);
        wfsProtectionSampleRate_ = sampleRate;
        wfsProtectionInit_ = true;
    }
}

template <typename Scalar>
void WfsRendererT<Scalar>::setSpeakerLayout3D(const float* dx, const float* dyFwd, const float* dz,
                                      int count) noexcept {
    if (dx == nullptr || dyFwd == nullptr || dz == nullptr || count <= 0) return;
    if (count > kMaxSpeakers) count = kMaxSpeakers;
    numSpeakers_ = count;
    explicitLayout_ = true;
    for (int s = 0; s < count; ++s) {
        speakerRelX_[static_cast<size_t>(s)] = dx[s];
        speakerRelY_[static_cast<size_t>(s)] = dyFwd[s];
        speakerRelZ_[static_cast<size_t>(s)] = dz[s];
        // Azimut/ILD/ITD desde el plano horizontal (mismo modelo que
        // rebuildGeometry: pan = -sin(az) con az medido desde atrás). La
        // altura no participa en ITD/ILD (simplificación estándar: los
        // cues interaurales por defecto no codifican elevación).
        const float az = std::atan2(-dx[s], -dyFwd[s]); // atrás=+π, coherente con rebuildGeometry
        speakerAzimuth_[static_cast<size_t>(s)] = az;
        const float pan = -std::sin(az);
        const float theta = (pan + 1.f) * 0.25f * 3.14159265358979323846f;
        // Ganancias constant-power en Scalar: en float, cos/sin float y el
        // literal 1.41421356f dan el valor histórico exacto.
        const Scalar thetaS = static_cast<Scalar>(theta);
        speakerGainL_[static_cast<size_t>(s)] = std::cos(thetaS) * Scalar(1.4142135623730951);
        speakerGainR_[static_cast<size_t>(s)] = std::sin(thetaS) * Scalar(1.4142135623730951);
        const int itd = static_cast<int>(kMaxItdS * sampleRate_ * std::fabs(pan) + 0.5f);
        if (pan >= 0.f) { speakerItdL_[static_cast<size_t>(s)] = itd; speakerItdR_[static_cast<size_t>(s)] = 0; }
        else            { speakerItdL_[static_cast<size_t>(s)] = 0;   speakerItdR_[static_cast<size_t>(s)] = itd; }
    }

    // Explicit room geometry must drive the spatial-alias limit too. Using
    // the virtual circular-array spacing here can set an unrelated cutoff.
    float minSpacingM = std::numeric_limits<float>::max();
    for (int a = 0; a < count; ++a) {
        for (int b = a + 1; b < count; ++b) {
            const float deltaX = dx[a] - dx[b];
            const float deltaY = dyFwd[a] - dyFwd[b];
            const float deltaZ = dz[a] - dz[b];
            const float spacing = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
            if (spacing > 1.0e-4f) minSpacingM = std::min(minSpacingM, spacing);
        }
    }
    // Degenerate geometry resets to the documented 8 kHz fallback rather
    // than retaining a cutoff from a previous valid layout.
    const float effectiveSpacing =
        (std::isfinite(minSpacingM) && minSpacingM < std::numeric_limits<float>::max())
            ? minSpacingM : 0.0f;
    aliasGuard_.init(effectiveSpacing, sampleRate_);
}

template <typename Scalar>
void WfsRendererT<Scalar>::reset() noexcept {
    // Reset explícito (cambio de sesión/ruta): limpia historiales. No se usa
    // en movimiento de fuentes — solo aquí está permitido borrar historia.
    for (auto& slot : slots_) {
        for (auto& v : slot.line) v = Scalar(0.0);
        slot.writePos = 0;
        slot.actEnv = slot.actTarget = slot.active ? 1.f : 0.f;
    }
    for (auto& h : itdHist_)
        for (auto& v : h) v = Scalar(0.0);
    for (auto& p : itdPos_) p = 0;
    aliasGuard_.reset();
}

template <typename Scalar>
void WfsRendererT<Scalar>::rebuildGeometry() noexcept {
    constexpr float kPi = 3.14159265358979323846f;
    for (int s = 0; s < numSpeakers_; ++s) {
        // Array circular completo, AZIMUT MEDIDO DESDE ATRÁS (-y), sentido
        // horario: el frente (+y) queda exactamente en az=±π donde pan=0 →
        // ganancias L/R iguales (simetría frontal exacta con N par).
        const float az = 2.f * kPi * static_cast<float>(s) / static_cast<float>(numSpeakers_) - kPi;
        speakerAzimuth_[s] = az;
        // ILD senoidal clásico (tangente del azimut → ganancias constant-power).
        // Con az medido desde atrás, el altavoz más próximo a +x tiene
        // az=-π/2; con pan=-sin(az) ese altavoz da pan=+1 → ILD a derecha.
        const float pan = -std::sin(az);                   // -1 (izq) … +1 (der)
        const float theta = (pan + 1.f) * 0.25f * kPi;     // 0…π/2
        const Scalar thetaS = static_cast<Scalar>(theta);
        speakerGainL_[s] = std::cos(thetaS) * Scalar(1.4142135623730951);  // normalizado a ±3 dB máx
        speakerGainR_[s] = std::sin(thetaS) * Scalar(1.4142135623730951);
        // ITD: retardo interaural máximo cuando el altavoz está lateral.
        const int itd = static_cast<int>(kMaxItdS * sampleRate_ * std::fabs(pan) + 0.5f);
        if (pan >= 0.f) { speakerItdL_[s] = itd; speakerItdR_[s] = 0; }
        else            { speakerItdL_[s] = 0;   speakerItdR_[s] = itd; }
    }
}

template <typename Scalar>
int WfsRendererT<Scalar>::findSlot(int id) const noexcept {
    for (int i = 0; i < kMaxObjects; ++i)
        if (slots_[static_cast<size_t>(i)].active && slots_[static_cast<size_t>(i)].id == id)
            return i;
    return -1;
}

template <typename Scalar>
int WfsRendererT<Scalar>::allocSlot() noexcept {
    for (int i = 0; i < kMaxObjects; ++i)
        if (!slots_[static_cast<size_t>(i)].active) return i;
    return -1;
}

template <typename Scalar>
void WfsRendererT<Scalar>::setObject(int id, float x, float y, float gain) noexcept {
    setObject4D(id, x, y, 0.f, 0.f, 0.f, 0.f, gain, 0.f);
}

template <typename Scalar>
void WfsRendererT<Scalar>::setObject4D(int id, float x, float y, float z,
                              float vx, float vy, float vz,
                              float gain, float roomCoupling) noexcept {
    if (id < 0) return;
    if (!std::isfinite(x)) x = 0.f;
    if (!std::isfinite(y)) y = 1.5f;
    if (!std::isfinite(z)) z = 0.f;
    if (!std::isfinite(vx)) vx = 0.f;
    if (!std::isfinite(vy)) vy = 0.f;
    if (!std::isfinite(vz)) vz = 0.f;
    if (!std::isfinite(gain)) gain = 1.f;
    if (!std::isfinite(roomCoupling)) roomCoupling = 0.f;

    // Clamp de distancia física 3D (una fuente a 1 km rompería la aproximación).
    const float d = std::sqrt(x * x + y * y + z * z);
    if (d > kMaxObjectDist) {
        const float sc = kMaxObjectDist / d;
        x *= sc; y *= sc; z *= sc;
    }
    z = std::clamp(z, -2.5f, 3.5f);
    roomCoupling = std::clamp(roomCoupling, 0.f, 0.85f);

    int si = findSlot(id);
    if (si >= 0) {
        // ── Movimiento de fuente existente: SOLO se recalculan los
        // objetivos de delay/ganancia. El historial de audio NO se toca;
        // los valores efectivos persiguen al objetivo con un one-pole por
        // muestra en process() → glide sin click (antes: rebuildDelays()
        // borraba todas las líneas = micro-corte garantizado).
        SourceSlot& slot = slots_[static_cast<size_t>(si)];
        slot.x = x; slot.y = y; slot.z = z;
        slot.vx = vx; slot.vy = vy; slot.vz = vz;
        slot.gain = gain;
        slot.roomCoupling = roomCoupling;
        slot.actTarget = Scalar(1.0);   // revive si estaba en fade-out
        computeTapTargets(si);
        return;
    }

    // ── Nacimiento de fuente nueva: línea fresca (ya está a ceros desde
    // init() o desde su último retiro completo), smooth snappeado al
    // objetivo (no hay audio previo que hacer glisar — el nacimiento es
    // naturalmente limpio porque la línea empieza en silencio físico).
    si = allocSlot();
    if (si < 0) return;         // array de fuentes lleno — degradar en silencio
    SourceSlot& slot = slots_[static_cast<size_t>(si)];
    slot.id = id; slot.active = true;
    slot.x = x; slot.y = y; slot.z = z;
    slot.vx = vx; slot.vy = vy; slot.vz = vz;
    slot.gain = gain;
    slot.roomCoupling = roomCoupling;
    slot.elevState = 0.f;
    slot.actEnv = Scalar(1.0); slot.actTarget = Scalar(1.0); slot.envFrom = Scalar(1.0); slot.envDelta = Scalar(0.0);
    slot.writePos = 0;
    ++numActiveObjects_;
    computeTapTargets(si);
    // Snap inicial: sin historial previo no hay nada que suavizar.
    for (int s = 0; s < numSpeakers_; ++s) {
        DelayTap& tap = taps_[static_cast<size_t>(si) * static_cast<size_t>(numSpeakers_)
                              + static_cast<size_t>(s)];
        tap.delaySmooth = tap.delayTarget;
        tap.gainSmooth  = tap.gainTarget;
    }
}

template <typename Scalar>
void WfsRendererT<Scalar>::setRoomDimensions(float widthM, float depthM, float heightM, float wallAbsorption) noexcept {
    if (std::isfinite(widthM)  && widthM  > 1.5f) roomWidthM_     = std::clamp(widthM,  2.0f, 30.0f);
    if (std::isfinite(depthM)  && depthM  > 1.5f) roomDepthM_     = std::clamp(depthM,  2.0f, 40.0f);
    if (std::isfinite(heightM) && heightM > 1.5f) roomHeightM_    = std::clamp(heightM, 2.0f, 15.0f);
    if (std::isfinite(wallAbsorption))            wallAbsorption_ = std::clamp(wallAbsorption, 0.05f, 0.95f);
}

template <typename Scalar>
void WfsRendererT<Scalar>::removeObject(int id) noexcept {
    const int si = findSlot(id);
    if (si < 0) return;
    // NO se borra la línea ni el estado: se marca fade-out. process() baja
    // actEnv a 0 a lo largo de un bloque y libera la ranura al final —
    // cortar una fuente sonando en seco era el otro click clásico.
    slots_[static_cast<size_t>(si)].actTarget = Scalar(0.0);
}

template <typename Scalar>
void WfsRendererT<Scalar>::clearObjects() noexcept {
    for (auto& slot : slots_)
        if (slot.active) slot.actTarget = Scalar(0.0);
}

template <typename Scalar>
void WfsRendererT<Scalar>::computeTapTargets(int si) noexcept {
    const SourceSlot& o = slots_[static_cast<size_t>(si)];
    const Scalar od = std::sqrt(o.x * o.x + o.y * o.y);
    for (int s = 0; s < numSpeakers_; ++s) {
        DelayTap& tap = taps_[static_cast<size_t>(si) * static_cast<size_t>(numSpeakers_)
                              + static_cast<size_t>(s)];
        // Posición del altavoz: geometría 3D real explícita (Fase 2) si se
        // configuró vía setSpeakerLayout3D(), si no el array circular
        // generado por rebuildGeometry() (comportamiento original).
        Scalar sx, sy, sz;
        if (explicitLayout_) {
            sx = static_cast<Scalar>(speakerRelX_[static_cast<size_t>(s)]);
            sy = static_cast<Scalar>(speakerRelY_[static_cast<size_t>(s)]);
            sz = static_cast<Scalar>(speakerRelZ_[static_cast<size_t>(s)]);
        } else {
            sx = -static_cast<Scalar>(kArrayRadiusM) * std::sin(static_cast<Scalar>(speakerAzimuth_[s]));
            sy = -static_cast<Scalar>(kArrayRadiusM) * std::cos(static_cast<Scalar>(speakerAzimuth_[s]));
            sz = Scalar(0.0); // fuentes y array circular viven a la altura del oído
        }
        const Scalar dx = o.x - sx, dy = o.y - sy, dzh = o.z - sz; // Fase 6: altura perceptual real 3D
        const Scalar d = std::sqrt(dx * dx + dy * dy + dzh * dzh); // distancia 3D real (Fase 2 + Fase 6)
        // Delay fraccionario objetivo (interpolación lineal en la lectura).
        const Scalar delaySampF = (d / static_cast<Scalar>(kSpeedOfSound)) * sampleRate_;
        const Scalar maxTap = static_cast<Scalar>(maxDelayTap_ - blockSize_ - 4);
        tap.delayTarget = delaySampF < Scalar(0.0) ? Scalar(0.0) : (delaySampF > maxTap ? maxTap : delaySampF);
        // Driving function WFS 2.5D (aprox.): atenuación de onda cilíndrica
        // 1/sqrt(d) referida a kRefDistanceM, por peso de focalización coseno.
        // FIX (geometría 3D real, 2026-09-19): cosA dividía por kArrayRadiusM
        // (constante del array circular, exacta ahí por construcción) incluso
        // con layout explícito, donde la distancia horizontal real de cada
        // altavoz varía mucho (subwoofer ~0.5 m, satélites elevados ~4 m) —
        // desbalanceaba el peso de foco entre altavoces. Se usa la distancia
        // horizontal REAL de cada altavoz (spkDist), idéntica a kArrayRadiusM
        // en el caso circular (sin cambio ahí) y correcta en el explícito.
        Scalar focus = Scalar(1.0);
        const Scalar spkDist = std::sqrt(sx * sx + sy * sy);
        if (od > Scalar(1e-6) && spkDist > Scalar(1e-6)) {
            const Scalar cosA = (o.x * sx + o.y * sy) / (od * spkDist);
            focus = cosA > Scalar(0.0) ? cosA : Scalar(0.0);
        }
        const Scalar att = std::sqrt(static_cast<Scalar>(kRefDistanceM) / (d > static_cast<Scalar>(kRefDistanceM) ? d : static_cast<Scalar>(kRefDistanceM)));
        tap.gainTarget = o.gain * att * focus;
    }
}

template <typename Scalar>
void WfsRendererT<Scalar>::process(const Scalar* const* objectInputs, int numObjects,
                          Scalar* outL, Scalar* outR, int frames) noexcept {
    if (frames <= 0 || outL == nullptr || outR == nullptr) return;
    if (numActiveObjects_ == 0) return;
    if (objectInputs == nullptr || numObjects <= 0) return;
    enableWfsDenormalGuard();   // FTZ/DAZ: sin microcode assists en el hilo RT

    const int M = maxDelayTap_;
    const Scalar invFrames = Scalar(1.0) / static_cast<Scalar>(frames);
    const bool softSuspended = (enabledTarget_ <= Scalar(0.0) && enabledMix_ <= Scalar(0.0));

    // ── 0) Envolventes de activación: rampa lineal por bloque hacia el
    //       objetivo (fade-out glitch-free en removeObject; las fuentes
    //       vivas permanecen en 1). Se calcula el paso ANTES del bloque.
    //       En Soft Suspension (softSuspended == true), mantenemos alimentadas
    //       las líneas de propagación slot.line con el audio real entrante
    //       y retornamos antes del bucle O(altavoces × objetos) de síntesis,
    //       garantizando continuidad acústica exacta al reactivar WFS.
    int inputIdx = 0;
    for (int si = 0; si < kMaxObjects; ++si) {
        SourceSlot& slot = slots_[static_cast<size_t>(si)];
        if (!slot.active) continue;
        slot.envFrom  = slot.actEnv;
        slot.envDelta = slot.actTarget - slot.actEnv;   // tramo del bloque
        // Mapeo input i → i-ésima fuente viva en orden de ranura: idéntico
        // al orden de registro de la versión anterior. Las fuentes en
        // fade-out NO consumen entrada (su señal ya no llega del caller).
        if (slot.actTarget > Scalar(0.0)) {
            const Scalar* in = (inputIdx < numObjects) ? objectInputs[inputIdx] : nullptr;
            ++inputIdx;
            // ── 1) Driving: escribir la fuente en SU línea (una por fuente).
            //       Si el caller no pasó entrada para una fuente viva, se
            //       escribe silencio — físicamente la fuente calló; mantener
            //       historia consistente (leer historia vieja sería un loop
            //       del pasado, audible como estutter).
            int wp = slot.writePos;
            if (in != nullptr) {
                for (int n = 0; n < frames; ++n) {
                    // GUARDA NaN/Inf (Fase 7: recuperación tras fallo): un
                    // NaN entrando a la línea circular la envenena PARA
                    // SIEMPRE (NaN*0=NaN — el historial nunca se limpia solo)
                    // y softLimit() no filtra NaN (todas sus ramas lo
                    // propagan). Se sanea en la ÚNICA puerta de entrada:
                    // muestra no finita -> silencio físico, el campo se
                    // recupera solo en el siguiente bloque.
                    const Scalar v = in[n];
                    slot.line[static_cast<size_t>(wp)] = std::isfinite(v) ? v : Scalar(0.0);
                    wp = (wp + 1) % M;
                }
            } else {
                for (int n = 0; n < frames; ++n) {
                    slot.line[static_cast<size_t>(wp)] = Scalar(0.0);
                    wp = (wp + 1) % M;
                }
            }
            slot.writePos = wp;
        }
    }

    if (softSuspended) {
        // Keep every stateful part of the wet chain clocked while bypassed.
        // Delay lines above receive the live input; the binaural ITD histories
        // and post-field filters receive silence so old programme material
        // cannot leak back after a long bypass interval.
        for (int n = 0; n < frames; ++n) {
            for (int s = 0; s < numSpeakers_; ++s) {
                auto& hist = itdHist_[static_cast<size_t>(s)];
                int& hp = itdPos_[static_cast<size_t>(s)];
                hist[static_cast<size_t>(hp)] = Scalar(0.0);
                hp = (hp + 1) & 255;
            }
            Scalar silentL = Scalar(0.0);
            Scalar silentR = Scalar(0.0);
            aliasGuard_.process(silentL, silentR);
            ensureWfsProtection(sampleRate_);
            wfsProtection_.process(silentL, silentR);
        }
        return;
    }

    // ── 2+3) Síntesis del campo y mezcla binaural en UN solo pase por
    //        muestra. Los taps leen la línea de SU fuente con su propio
    //        delay fraccionario suavizado y su ganancia suavizada; el
    //        one-pole por muestra (kSmoothCoeff) hace que cualquier
    //        movimiento de la fuente sea un glide continuo — no hay salto
    //        de fase ni de amplitud, por tanto NO hay micro-corte.
    const Scalar norm = Scalar(1.0) / std::sqrt(static_cast<Scalar>(numSpeakers_));
    for (int n = 0; n < frames; ++n) {
        // Avance de envolventes por muestra (rampa lineal exacta en 1 bloque).
        // Envolvente con forma smoothstep t^2*(3-2t): derivada CERO al
        // inicio y al fin del fade -> el retiro/nacimiento de una fuente no
        // produce el click de esquina de la rampa lineal. f(1)=1 exacto.
        const Scalar tt = static_cast<Scalar>(n + 1) * invFrames;
        const Scalar fshape = tt * tt * (Scalar(3.0) - Scalar(2.0) * tt);
        for (int si = 0; si < kMaxObjects; ++si) {
            SourceSlot& slot = slots_[static_cast<size_t>(si)];
            if (!slot.active || slot.envDelta == Scalar(0.0)) continue;
            slot.actEnv = slot.envFrom + slot.envDelta * fshape;
            if (slot.actEnv < Scalar(0.0)) slot.actEnv = Scalar(0.0);
            if (slot.actEnv > Scalar(1.0)) slot.actEnv = Scalar(1.0);
        }

        Scalar accL = Scalar(0.0), accR = Scalar(0.0);
        for (int s = 0; s < numSpeakers_; ++s) {
            Scalar spk = Scalar(0.0);
            for (int si = 0; si < kMaxObjects; ++si) {
                const SourceSlot& slot = slots_[static_cast<size_t>(si)];
                if (!slot.active) continue;
                DelayTap& tap = taps_[static_cast<size_t>(si) * static_cast<size_t>(numSpeakers_)
                                      + static_cast<size_t>(s)];
                // writePos apunta una posición DESPUÉS de la última muestra
                // escrita; la muestra n del bloque vive (frames-1-n) atrás,
                // más el delay físico suavizado del par (fuente→altavoz).
                const Scalar ds = tap.delaySmooth;
                const int di = static_cast<int>(ds);
                const Scalar frac = ds - static_cast<Scalar>(di);
                const int back = frames - 1 - n + di;
                int rp  = (slot.writePos - back) % M;
                if (rp < 0) rp += M;
                int rp2 = rp - 1; if (rp2 < 0) rp2 += M;
                const Scalar a = slot.line[static_cast<size_t>(rp)];
                const Scalar b = slot.line[static_cast<size_t>(rp2)];
                Scalar directWave = (a + (b - a) * frac);
                if (slot.roomCoupling > Scalar(1.0e-4)) {
                    const Scalar reflGain = slot.roomCoupling * (Scalar(1.0) - static_cast<Scalar>(wallAbsorption_)) * Scalar(0.22);
                    const int reflSamples = std::min(M - frames - 4, std::max(8, static_cast<int>((roomWidthM_ / kSpeedOfSound) * sampleRate_)));
                    int rpRefl = (rp - reflSamples) % M;
                    if (rpRefl < 0) rpRefl += M;
                    directWave += slot.line[static_cast<size_t>(rpRefl)] * reflGain;
                }
                spk += directWave * tap.gainSmooth * slot.actEnv;
                // Suavizado por muestra: persecución glitch-free del objetivo.
                tap.delaySmooth += (tap.delayTarget - tap.delaySmooth) * kSmoothCoeff;
                tap.gainSmooth  += (tap.gainTarget  - tap.gainSmooth)  * kSmoothCoeff;
                // Snap: error sub-audible -> clavar al objetivo. Elimina la
                // persecucion asintotica en regimen denormal y la deriva.
                if (std::fabs(tap.delayTarget - tap.delaySmooth) < Scalar(5e-3))
                    tap.delaySmooth = tap.delayTarget;
                if (std::fabs(tap.gainTarget - tap.gainSmooth) < Scalar(1e-4))
                    tap.gainSmooth = tap.gainTarget;
            }
            // ITD entero por altavoz + ILD constant-power.
            auto& hist = itdHist_[static_cast<size_t>(s)];
            int& hp = itdPos_[static_cast<size_t>(s)];
            hist[static_cast<size_t>(hp)] = spk;
            const Scalar vL = hist[static_cast<size_t>((hp - speakerItdL_[s]) & 255)];
            const Scalar vR = hist[static_cast<size_t>((hp - speakerItdR_[s]) & 255)];
            accL += vL * speakerGainL_[s];
            accR += vR * speakerGainR_[s];
            hp = (hp + 1) & 255;
        }
        // FIX (guarda de anti-aliasing espacial, antes definida pero jamás
        // llamada desde ningún lado del árbol): se aplica al campo YA
        // reconstruido (acumulado sobre los altavoces virtuales), antes del
        // limitador — es la reconstrucción de onda la que necesita
        // suavizarse por encima de f_alias, no la señal ya comprimida.
        aliasGuard_.process(accL, accR);
        // Anti-clip (=> anti-tronidos): lineal bajo |1.0|, compresion suave arriba.
        // GUARDA NaN/Inf de salida: si pese a todo el campo acumulado no es
        // finito (estado corrupto heredado de antes del saneo de entrada),
        // se sustituye por silencio en ESTE bloque — la cadena downstream
        // (mixer/effect/salida hardware) jamás recibe un NaN desde WFS.
        const Scalar outLRaw = accL * norm, outRRaw = accR * norm;
        // WfsProtectionChain REAL (ya no declarada-sin-usar): sanea NaN/Inf,
        // quita DC y subnormales, y limita suave — imposible clipear aunque
        // la suma coherente de altavoces supere 0 dBFS.
        ensureWfsProtection(sampleRate_);
        Scalar pl = outLRaw, pr = outRRaw;   // copias mutables (los Raw son const)
        wfsProtection_.process(pl, pr);
        outL[n] += pl;
        outR[n] += pr;
    }

    // ── 4) Retiro diferido: las fuentes cuyo fade-out terminó se liberan
    //       DESPUÉS del bloque (su línea queda a ceros de forma natural:
    //       durante el fade escribimos... su señal real; al liberar, la
    //       línea se limpia para su próximo nacimiento — hilo de control,
    //       fuera del pase caliente).
    const Scalar dtBlock = (sampleRate_ > Scalar(0.0)) ? (static_cast<Scalar>(frames) / sampleRate_) : Scalar(0.0);
    for (int si = 0; si < kMaxObjects; ++si) {
        SourceSlot& slot = slots_[static_cast<size_t>(si)];
        if (!slot.active) continue;
        if (slot.actTarget == Scalar(0.0) && slot.actEnv <= Scalar(0.0)) {
            slot.active = false;
            slot.id = -1;
            for (auto& v : slot.line) v = Scalar(0.0);
            slot.writePos = 0;
            --numActiveObjects_;
            continue;
        }
        // Fase 6: Integración cinemática continua 4D (sin salto de fase ni reset de línea)
        if (dtBlock > Scalar(0.0) && (std::fabs(slot.vx) > Scalar(1.0e-5) || std::fabs(slot.vy) > Scalar(1.0e-5) || std::fabs(slot.vz) > Scalar(1.0e-5))) {
            slot.x += slot.vx * dtBlock;
            slot.y += slot.vy * dtBlock;
            slot.z = std::clamp(slot.z + slot.vz * dtBlock, Scalar(-2.5), Scalar(3.5));
            const Scalar d = std::sqrt(slot.x * slot.x + slot.y * slot.y + slot.z * slot.z);
            if (d > static_cast<Scalar>(kMaxObjectDist)) {
                const Scalar sc = static_cast<Scalar>(kMaxObjectDist) / d;
                slot.x *= sc; slot.y *= sc; slot.z *= sc;
            }
            computeTapTargets(si);
        }
    }
}

template <typename Scalar>
void WfsRendererT<Scalar>::blendWithBypass(const Scalar* dryL, const Scalar* dryR,
                                   const Scalar* wetL, const Scalar* wetR,
                                   Scalar* outL, Scalar* outR, int frames) noexcept {
    if (frames <= 0 || dryL == nullptr || dryR == nullptr ||
        wetL == nullptr || wetR == nullptr || outL == nullptr || outR == nullptr) return;
    const Scalar step = Scalar(1.0) / (sampleRate_ * Scalar(0.015));
    for (int n = 0; n < frames; ++n) {
        if (enabledMix_ < enabledTarget_)      enabledMix_ = std::min(enabledMix_ + step, enabledTarget_);
        else if (enabledMix_ > enabledTarget_) enabledMix_ = std::max(enabledMix_ - step, enabledTarget_);
        const Scalar t = enabledMix_;
        const Scalar shaped = t * t * (Scalar(3.0) - Scalar(2.0) * t);
        // Exact dry bypass is also a fault-containment boundary: IEEE-754
        // NaN * 0 remains NaN, so a contaminated wet renderer must never
        // leak into the dry signal after the 15 ms fade has completed.
        if (shaped <= Scalar(0.0)) {
            outL[n] = dryL[n];
            outR[n] = dryR[n];
        } else if (shaped >= Scalar(1.0)) {
            outL[n] = wetL[n];
            outR[n] = wetR[n];
        } else {
            outL[n] = dryL[n] * (Scalar(1.0) - shaped) + wetL[n] * shaped;
            outR[n] = dryR[n] * (Scalar(1.0) - shaped) + wetR[n] * shaped;
        }
    }
}

} // namespace ivanna::spatial

// Instanciaciones explícitas (las definiciones viven en este .cpp):
// float = ruta legacy bit-idéntica; double = núcleo de la labor grande.
template class ivanna::spatial::WfsRendererT<float>;
template class ivanna::spatial::WfsRendererT<double>;
template struct ivanna::wfs::SpatialAliasGuardT<float>;
template struct ivanna::wfs::SpatialAliasGuardT<double>;
template struct ivanna::wfs::WfsProtectionChainT<float>;
template struct ivanna::wfs::WfsProtectionChainT<double>;
