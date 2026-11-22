// fused_spatial.cpp — OPERADOR ESPACIAL ÚNICO (ORBE·Ω). Ver fused_spatial.hpp
// para el mapa de la cadena y lo que NO se envuelve (y por qué).
//
// Camino caliente (F2/486): todo double, sin conversión float en el
// camino. Trozos de 128 frames (Ivanna::BLOCK_SIZE), motores en orden,
// saneo fail-closed (no finito → 0.0). Sin malloc/locks/I-O en process().
#include "fused/fused_spatial.hpp"

#include <algorithm>
#include <time.h>

namespace ivanna::fused {

namespace {
constexpr float kPiF = 3.14159265358979323846f;
constexpr float kDegToRad = kPiF / 180.0f;
// F0 (v2.5.471): reloj monotónico en ms, base CLOCK_BOOTTIME — la misma
// base que SensorEvent.timestamp / SystemClock.elapsedRealtimeNanos, así
// la latencia sensor→aplicado es una resta directa. CLOCK_MONOTONIC no
// sirve aquí: difiere de BOOTTIME por el tiempo en suspensión.
float nowMsBoottime() noexcept {
    struct timespec ts;
    if (::clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return -1.0f;
    return static_cast<float>(ts.tv_sec) * 1000.0f +
           static_cast<float>(ts.tv_nsec) / 1.0e6f;
}
// Rutas del dataset HRTF medido (las mismas que prueba el ctor de
// HrtfManager): en host no existen y se cae a síntesis analítica.
constexpr const char* kHrtfDatasetPath = "/data/adb/ivanna_omega/hrtf_dataset.ihr1";
constexpr const char* kHrtfKemarPath = "/data/adb/ivanna_omega/kemar.ihr1";
}  // namespace

FusedSpatial::FusedSpatial() {
    field_.resize(kChunk);  // pre-reserva única (ctor, no camino caliente)
    controls_.clamp();
}

void FusedSpatial::prepare(double sampleRate) noexcept {
    const double sr = (std::isfinite(sampleRate) && sampleRate > 8000.0 &&
                       sampleRate <= 384000.0)
                          ? sampleRate
                          : 48000.0;
    sampleRate_ = sr;
    const float fsr = static_cast<float>(sr);

    upmixer_.prepare(fsr);
    hoaDecoder_.prepare(fsr, 8);
    if (!hrtf_.hasDataset()) {
        // En host no existen (síntesis analítica, seguro); en el teléfono el
        // ctor de HrtfManager ya probó estas rutas al arrancar.
        if (!hrtf_.loadFromDataset(kHrtfDatasetPath))
            hrtf_.loadFromDataset(kHrtfKemarPath);
    }
    // PARCHE v2.6 (Fallo 6 / punto 2): la envolvente wet/dry del HRTF
    // se reconfigura con la SR real (si no, a 96/192 kHz las rampas
    // duran la mitad / cuarta parte de lo diseñado en ms).
    hrtf_.prepare(fsr);
    orbe_.prepare(fsr);
    wfs_.init(fsr, kChunk, 16);
    xtc_.prepare(fsr, 2048);
    auxField_.prepare(fsr);
    decomposer_.prepare(fsr, kChunk);
    trackerFusion_.prepare(fsr);
    headTracker_.reset();

    wfsMix_ = 0.0;
    lastAperture_ = -1.0f;
    lastPoseYawDeg_ = 1.0e9f;
    lastPosePitchDeg_ = 1.0e9f;
    lastPoseRollDeg_ = 1.0e9f;
    lastXtcYawDeg_ = 1.0e9f;
    // Frontera de stream (Fallo 3d): los pendientes del acumulador se
    // vacían; un reset es un reinicio, no una continuación.
    inPendCount_ = 0;
    outPendHead_ = 0;
    outPendCount_ = 0;
    applyControls();
    prepared_ = true;
}

void FusedSpatial::setControls(const SpatialControls& c) noexcept {
    controls_ = c;
    controls_.clamp();
    if (prepared_) applyControls();
}

void FusedSpatial::applyControls() noexcept {
    const bool speakers = isSpeakers();
    upmixer_.setUpmixingEnabled(controls_.upmixEnabled);
    upmixer_.setImmersivity(controls_.upmixImmersivity);

    // Puerta Genezi (Fix 1): en SPEAKERS el wet HRTF efectivo es 0.
    hrtf_.setWetDry(effectiveHrtfWet());
    if (effectiveHrtfWet() <= 0.0f) hrtf_.flushHistory();

    // Distancia de escena HRTF (ley 1/d con rampa; identidad en 1.0 m).
    // setSceneDistance solo acepta 0.5..2.0 m: más cerca, identidad.
    if (controls_.distanceM >= 0.5f && controls_.distanceM <= 2.0f)
        hrtf_.setSceneDistance(controls_.distanceM);
    else
        hrtf_.setSceneDistance(1.0f);

    OrbeParams op;
    op.amount = effectiveOrbeAmount();  // 0 efectivo en SPEAKERS
    op.distanceM = controls_.distanceM;
    op.spread = std::clamp(controls_.width, 0.0f, 1.5f);
    op.proximityDb = 3.0f;
    op.diffuse = 0.5f;
    op.headTracking = controls_.headTracking;
    op.headRadiusM = kHeadRadiusM;
    op.bandCount = 24;
    orbe_.setParams(op);

    xtc_.setEngage(speakers && controls_.xtcEnabled);

    // Preset AuxGeneziRoute GeneziRoom (auxPreset): xover 120 Hz, coherencia
    // 1.0, campo activado. PARCHE v2.6 (Fallo 4 / punto 3): solo tiene
    // efecto en SPEAKERS cuando controls_.auxEnabled es true; con
    // auxEnabled==false (default) el AuxField no corre en SPEAKERS-neutro
    // y la salida sigue siendo bit-exacta (test SpeakersModeGate).
    const ivanna::spatial::AuxRouteParams auxPreset =
        ivanna::spatial::auxPreset(ivanna::spatial::AuxPresetKind::GeneziRoom);
    auxField_.setParams(speakers && controls_.auxEnabled, auxPreset.auxXoverHz, auxPreset.auxCoherence);

    applyPose(true);
}

void FusedSpatial::applyPose(bool force) noexcept {
    // Azimut del control + yaw del tracker (solo con headTracking),
    // escalado por parallaxAmount (SpatialControls, v2.5.472): con 0 el
    // tracker no mueve el sonido; con 1 es el comportamiento actual.
    // HrtfManager::setHeadPose re-sintetiza el banco y dispara crossfade:
    // solo se llama cuando algún ángulo cambia >0.25° (nunca por bloque).
    // SO(3) completo: yaw + pitch + roll del tracker, todos escalados.
    const float par = controls_.parallaxAmount;
    float yawDeg = controls_.azimuthDeg;
    float pitchDeg = 0.0f;
    float rollDeg = 0.0f;
    if (controls_.headTracking) {
        const ivanna::spatial::FusedPose pose = trackerFusion_.latest();
        yawDeg += pose.yawDeg * par;
        pitchDeg = pose.pitchDeg * par;
        rollDeg = pose.rollDeg * par;
    }
    if (force || std::fabs(yawDeg - lastPoseYawDeg_) > 0.25f ||
        std::fabs(pitchDeg - lastPosePitchDeg_) > 0.25f ||
        std::fabs(rollDeg - lastPoseRollDeg_) > 0.25f) {
        const float yawRad = yawDeg * kDegToRad;
        orbe_.setHeadPose(yawRad);
        hrtf_.setHeadPose(yawRad, pitchDeg * kDegToRad, rollDeg * kDegToRad);
        lastPoseYawDeg_ = yawDeg;
        lastPosePitchDeg_ = pitchDeg;
        lastPoseRollDeg_ = rollDeg;
        // F0 (v2.5.471): cierra la latencia end-to-end sensor→bloque
        // renderizado. Este es el primer bloque que suena con la pose nueva:
        // la diferencia con el timestamp del sensor es el retardo
        // giro-a-sonido que mide la cadena de seguimiento. Solo si hubo
        // pose alimentada (sensorTs válido); sin sensor no hay medición.
        const float appliedMs = nowMsBoottime();
        if (appliedMs >= 0.0f) {
            poseAppliedTsMs_.store(appliedMs, std::memory_order_relaxed);
            poseAppliedYawDeg_.store(yawDeg, std::memory_order_relaxed);
            const float sensorMs = poseSensorTsMs_.load(std::memory_order_relaxed);
            if (sensorMs > -1.0e8f) {
                const float lat = appliedMs - sensorMs;
                if (lat >= 0.0f && lat < 10000.0f)
                    poseLatencyMs_.store(lat, std::memory_order_relaxed);
            }
        }
    }
    // El XTC sigue a la cabeza: azimut + corrimiento lateral estimado.
    // El azimut también se escala por parallaxAmount (con 0, el XTC no
    // sigue a la cabeza).
    if (isSpeakers() && controls_.headTracking) {
        float az = 0.0f, lat = 0.0f;
        trackerFusion_.offsetForSpatial(az, lat);
        az *= controls_.parallaxAmount;
        lat *= controls_.parallaxAmount;
        if (force || std::fabs(az - lastXtcYawDeg_) > 0.25f) {
            xtc_.setHeadOffset(az, lat);
            lastXtcYawDeg_ = az;
        }
    }
}

void FusedSpatial::feedRotationVector(const float rv[4], float timestampMs) noexcept {
    if (!rv) return;
    // F0 (v2.5.471): timestamps de seguimiento. timestampMs es el instante
    // del sensor (ROTATION_VECTOR); nowMsBoottime() es el instante en que
    // el callback JNI entrega la pose. La diferencia es la latencia
    // sensor→JNI; la end-to-end se cierra en applyPose().
    if (std::isfinite(timestampMs)) {
        poseSensorTsMs_.store(timestampMs, std::memory_order_relaxed);
        const float jniMs = nowMsBoottime();
        if (jniMs >= 0.0f) {
            poseJniTsMs_.store(jniMs, std::memory_order_relaxed);
            const float jniLat = jniMs - timestampMs;
            if (jniLat >= 0.0f && jniLat < 10000.0f)
                poseJniLatencyMs_.store(jniLat, std::memory_order_relaxed);
        }
    }
    headTracker_.update(rv, timestampMs);
    trackerFusion_.updateRotationVector(rv, timestampMs);
}

float FusedSpatial::trackerYawDeg() const noexcept {
    return trackerFusion_.latest().yawDeg;
}

float FusedSpatial::trackerPitchDeg() const noexcept {
    return trackerFusion_.latest().pitchDeg;
}

float FusedSpatial::trackerRollDeg() const noexcept {
    return trackerFusion_.latest().rollDeg;
}

// Alimentación de sensores sueltos a la fusión (hilo de sensor). Hoy
// IvannaHeadTracker solo alimenta el vector de rotación; el giroscopio /
// acelerómetro / magnetómetro quedan cableados en nativo y pendientes del
// alimentador Kotlin (sin ellos la fusión gyro+RV no se ejerce en vivo,
// pero el RV solo ya da yaw+pitch+roll completos).
void FusedSpatial::feedGyro(const float gyro[3], float timestampMs) noexcept {
    if (!gyro) return;
    trackerFusion_.updateGyro(gyro, timestampMs);
}

void FusedSpatial::feedAccel(const float acc[3], float timestampMs) noexcept {
    if (!acc) return;
    trackerFusion_.updateAccel(acc, timestampMs);
}

void FusedSpatial::feedMagnetometer(const float mag[3], float timestampMs) noexcept {
    if (!mag) return;
    trackerFusion_.updateMagnetometer(mag, timestampMs);
}

// Traslación del oyente para el parallax por traslación (ARCore donde
// llegue; sin ella, (0,0,0)). La geometría efectiva por banda la
// recalcula NearFieldHeadModel (sourceRelativeToListener).
void FusedSpatial::setListenerTranslation(float xM, float yM, float zM) noexcept {
    trackerFusion_.setListenerTranslation(xM, yM, zM);
}

void FusedSpatial::listenerTranslation(float out[3]) const noexcept {
    trackerFusion_.listenerTranslation(out);
}

void FusedSpatial::trackingTelemetry(float out[5]) const noexcept {
    if (!out) return;
    out[0] = poseLatencyMs_.load(std::memory_order_relaxed);
    out[1] = poseJniLatencyMs_.load(std::memory_order_relaxed);
    out[2] = poseAppliedYawDeg_.load(std::memory_order_relaxed);
    out[3] = poseSensorTsMs_.load(std::memory_order_relaxed);
    out[4] = poseAppliedTsMs_.load(std::memory_order_relaxed);
}

void FusedSpatial::reset() noexcept {
    // Re-prepare: cada motor vuelve a su estado inicial sin allocs nuevos
    // (salvo el vector de altavoces del decoder, ya dimensionado).
    // No toca controls_: el reset es de estado DSP, no de UI.
    const double sr = sampleRate_;
    prepare(sr);
}

void FusedSpatial::process(double* left, double* right, int frames) noexcept {
    if (!left || !right || frames <= 0) return;
    // FALLO 4 (2026-11-18): retorno temprano SOLO cuando no hay nada
    // que fundir. isNeutral() (controles) no basta: al pasar a neutro
    // los motores siguen en su cola de fundido de salida — ORBE 15 ms
    // (orbe_.isActive()), WFS (wfsMix_ > 0), XTC 20 ms
    // (xtc_.isActive()) y AuxField 20 ms (auxField_.isActive(), que
    // además es true siempre en SPEAKERS) — y processChunk() es quien
    // las corre hasta 0. Salir aquí las cortaba de golpe (clic) y en
    // SPEAKERS-neutro el AuxField no llegaba a ejecutarse nunca.
    // PARCHE v2.6: usa hasActiveTail() (método público, misma condición).
    if (isNeutral() && !hasActiveTail()) return;  // bit-transparente, ganancia exactamente 1.0
    if (!prepared_) prepare(48000.0);  // fail-closed: auto-prepare a 48 kHz

    // Acumulación exacta de 128 (Fallo 3d): processChunk() solo recibe
    // trozos completos de kChunk, así que el HRTF (AudioBuffer de 128
    // fijos) jamás se rellena con ceros. La salida sale de la cola
    // outPend_ con retraso ≤127 muestras; si la cola está vacía (primer
    // trozo parcial de un stream) esa muestra sale en seco, sin tocar.
    // Con llamadas múltiplo de 128 (el camino vivo) no hay retraso ni
    // seco: cada trozo se procesa y se emite en la misma llamada.
    // (El retorno temprano de arriba es del FALLO 4: con colas de fundido
    // pendientes se entra aquí aunque isNeutral() sea true.)
    for (int i = 0; i < frames; ++i) {
        const double dl = left[i];
        const double dr = right[i];
        inPendL_[static_cast<std::size_t>(inPendCount_)] =
            std::isfinite(dl) ? dl : 0.0;
        inPendR_[static_cast<std::size_t>(inPendCount_)] =
            std::isfinite(dr) ? dr : 0.0;
        ++inPendCount_;
        if (inPendCount_ == kChunk) {
            processChunk(inPendL_.data(), inPendR_.data(), kChunk);
            for (int j = 0; j < kChunk; ++j) {
                const int w =
                    (outPendHead_ + outPendCount_ + j) % (2 * kChunk);
                outPendL_[static_cast<std::size_t>(w)] =
                    inPendL_[static_cast<std::size_t>(j)];
                outPendR_[static_cast<std::size_t>(w)] =
                    inPendR_[static_cast<std::size_t>(j)];
            }
            outPendCount_ += kChunk;
            inPendCount_ = 0;
        }
        if (outPendCount_ > 0) {
            const double vl = outPendL_[static_cast<std::size_t>(outPendHead_)];
            const double vr = outPendR_[static_cast<std::size_t>(outPendHead_)];
            outPendHead_ = (outPendHead_ + 1) % (2 * kChunk);
            --outPendCount_;
            left[i] = std::isfinite(vl) ? vl : 0.0;
            right[i] = std::isfinite(vr) ? vr : 0.0;
        }
        // Sin salida en cola: left[i]/right[i] quedan como entraron (seco).
    }
}

FusedSpatial::BinauralStage FusedSpatial::selectBinauralStage() const noexcept {
    // Fallo 3d: UNA sola etapa binaural por trozo. Prioridad:
    //   HOA (upmix+decoder: render completo de escena) >
    //   ORBE (campo cercano, el buque insignia) >
    //   HRTF (SOFA/síntesis, reserva genérica) >
    //   WFS (apertura de 2 objetos).
    // En SPEAKERS ninguna: la ruta de altavoz es XTC + AuxField.
    if (isSpeakers()) return BinauralStage::None;
    if (controls_.upmixEnabled) return BinauralStage::Hoa;
    if (effectiveOrbeAmount() > 0.0f || orbe_.isActive())
        return BinauralStage::Orbe;
    if (effectiveHrtfWet() > 0.0f) return BinauralStage::Hrtf;
    if (controls_.wfsSpread > 0.0f || wfsMix_ > 0.0f)
        return BinauralStage::Wfs;
    return BinauralStage::None;
}

void FusedSpatial::processChunk(double* l, double* r, int n) noexcept {
    // n == kChunk SIEMPRE (lo garantiza process() con el acumulador).
    const bool speakers = isSpeakers();
    applyPose(false);
    const BinauralStage stage = selectBinauralStage();

    // 1. Upmixer HOA + decoder binaural: SOLO si es la etapa elegida.
    //    Antes corría también en SPEAKERS (su salida ES binaural) y en
    //    serie con HRTF/ORBE/WFS.
    if (stage == BinauralStage::Hoa) {
        upmixer_.processBlock(l, r, field_.data(), static_cast<std::size_t>(n));
        hoaDecoder_.processBlock(field_, hoaL_.data(), hoaR_.data(),
                                 static_cast<std::size_t>(n));
        std::memcpy(l, hoaL_.data(), static_cast<std::size_t>(n) * sizeof(double));
        std::memcpy(r, hoaR_.data(), static_cast<std::size_t>(n) * sizeof(double));
    }

    // 2. HRTF: SOLO si es la etapa elegida (audífonos, wet > 0). Sin
    //    relleno con ceros: n == 128 exactos, el AudioBuffer se llena
    //    entero con audio real (Fallo 3d).
    if (stage == BinauralStage::Hrtf) {
        // F2/486: el HRTF ya es double — directo sobre el trozo, sin el
        // AudioBuffer float intermedio (n == 128 exactos, como antes).
        hrtf_.processBinauralScene(l, r, n);
    }

    // 3. WFS (2 objetos L/R a ±apertura). Solo renderiza si es la etapa
    //    elegida; si no lo es pero su mezcla venía activa (wfsMix_ > 0),
    //    corre su rampa DE SALIDA hacia 0 (fundido, no segundo render).
    //    El descomponedor mide el ancho real de la escena (sideRatio) y
    //    modula la apertura; la mezcla es de potencia constante con rampa
    //    suave por bloque (sin clic al entrar/salir).
    const double wfsTarget =
        (stage == BinauralStage::Wfs) ? static_cast<double>(controls_.wfsSpread) : 0.0;
    if (wfsTarget > 0.0f || wfsMix_ > 0.0f) {
        double* objs[4] = {objBuf_[0].data(), objBuf_[1].data(),
                          objBuf_[2].data(), objBuf_[3].data()};
        decomposer_.decompose(l, r, objs, static_cast<std::size_t>(n));
        const float widthFactor = 0.3f + 0.9f * decomposer_.sideRatio();
        const float aperture = kWfsMaxApertureM * wfsTarget * widthFactor;
        if (std::fabs(aperture - lastAperture_) > 0.02f) {
            // setObject solo mueve objetivos (delay/ganancia suavizados por
            // muestra); jamás resetea historiales ni aloca.
            wfs_.setObject(0, -aperture, kWfsObjectY, 1.0f);
            wfs_.setObject(1, +aperture, kWfsObjectY, 1.0f);
            lastAperture_ = aperture;
        }
        std::fill(wfsL_.begin(), wfsL_.begin() + n, 0.0);
        std::fill(wfsR_.begin(), wfsR_.begin() + n, 0.0);
        const double* inputs[2] = {l, r};
        wfs_.process(inputs, 2, wfsL_.data(), wfsR_.data(), n);
        // PARCHE v2.6 (punto 3): fundido WFS por muestra (no interpolación
        // lineal por bloque): wfsMix_ avanza hacia wfsTarget con paso
        // 1/(0.015*SR) por muestra (constante de tiempo 15 ms), así el
        // cambio de mezcla es suave y no llega al objetivo en un solo
        // bloque de 2.67 ms (que era brusco al entrar/salir).
        const double wfsStep =
            1.0 / (0.015 * sampleRate_);
        for (int i = 0; i < n; ++i) {
            if (wfsMix_ < wfsTarget)
                wfsMix_ = std::min(wfsMix_ + wfsStep, wfsTarget);
            else if (wfsMix_ > wfsTarget)
                wfsMix_ = std::max(wfsMix_ - wfsStep, wfsTarget);
            const double t = wfsMix_;
            const double gWet = std::sin(t * (kPiF * 0.5));
            const double gDry = std::cos(t * (kPiF * 0.5));
            l[i] = gDry * l[i] + gWet * wfsL_[static_cast<std::size_t>(i)];
            r[i] = gDry * r[i] + gWet * wfsR_[static_cast<std::size_t>(i)];
        }
    }

    // 4. ORBE (campo cercano). Solo renderiza si es la etapa elegida;
    //    si no lo es pero sigue activo, corre su fade de salida de 15 ms
    //    (fundido, no segundo render). amount efectivo 0 en SPEAKERS →
    //    passthrough bit-exacto una vez completado el fade.
    if (stage == BinauralStage::Orbe) {
        if (effectiveOrbeAmount() > 0.0f || orbe_.isActive())
            orbe_.process(l, r, n);
    } else if (orbe_.isActive()) {
        orbe_.process(l, r, n);  // solo fade de salida
    }

    // 5. Crosstalk canceller (solo altavoces; su fade de salida de
    //    20 ms corre también al salir de SPEAKERS mientras isActive(),
    //    igual que el AuxField — si no, wetSmooth_ quedaba congelado
    //    > 0.5 y la cola no terminaba nunca (Fallo 4)).
    if ((speakers && controls_.xtcEnabled) || xtc_.isActive())
        xtc_.processBlock(l, r, n);

    // 6. Coherencia de campo AUX (solo altavoces con auxEnabled; su fade
    //    de salida de 20 ms corre también al salir de SPEAKERS mientras
    //    isActive()). PARCHE v2.6 (punto 3): sin auxEnabled el AuxField no
    //    corre en SPEAKERS-neutro y la salida sigue bit-exacta.
    if ((speakers && controls_.auxEnabled) || auxField_.isActive())
        auxField_.process(l, r, static_cast<std::size_t>(n));
}

}  // namespace ivanna::fused
