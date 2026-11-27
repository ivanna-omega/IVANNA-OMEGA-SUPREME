// test_fused_spatial.cpp — Operador Espacial Único (ORBE·Ω), Fusión Magistral.
// 48 kHz, tono 1 kHz. Mide con fused_measure.hpp (THD+N en double + null test).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_measure.hpp"
#include "../fused/fused_spatial.hpp"

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 48000;  // 1 s = 1000 ciclos exactos de 1 kHz
constexpr double kToneAmp = 0.7071067811865476;  // -3 dBFS

using ivanna::fused::FusedControls;
using ivanna::fused::FusedSpatial;
using ivanna::fused::SpatialControls;
using ivanna::OmegaOutputMode;
using namespace ivanna::fused::measure;

std::vector<double> makeTone(double amp = kToneAmp, double freqHz = 1000.0) {
    std::vector<double> x(static_cast<std::size_t>(kFrames));
    fillToneDouble(x.data(), x.size(), kSr, freqHz, amp);
    return x;
}

std::vector<double> makeDc(double v) {
    return std::vector<double>(static_cast<std::size_t>(kFrames), v);
}

// Ruido blanco determinista (xorshift64, semilla fija) en [-amp, +amp].
std::vector<double> makeNoise(double amp = 0.99) {
    std::vector<double> x(static_cast<std::size_t>(kFrames));
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    for (auto& v : x) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        const double u = static_cast<double>(s >> 11) / 9007199254740992.0;  // [0,1)
        v = (2.0 * u - 1.0) * amp;
    }
    return x;
}

SpatialControls engagedControls() {
    SpatialControls c;
    c.amount = 1.0f;
    c.azimuthDeg = 30.0f;
    c.distanceM = 1.0f;
    c.width = 1.0f;
    c.mode = OmegaOutputMode::HEADPHONES;
    c.wfsSpread = 0.5f;
    c.hrtfWet = 1.0f;
    c.headTracking = false;
    c.xtcEnabled = false;
    c.upmixEnabled = true;
    c.upmixImmersivity = 1.25f;
    c.clamp();
    return c;
}

void processBoth(FusedSpatial& s, std::vector<double>& l, std::vector<double>& r) {
    s.process(l.data(), r.data(), static_cast<int>(l.size()));
}

TEST(FusedSpatial, NeutralIsBitExact) {
    FusedControls fc;
    fc.setNeutral();
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(fc.spatial);
    ASSERT_TRUE(s.isNeutral());

    const std::vector<double> inL = makeTone();
    const std::vector<double> inR = makeTone();
    std::vector<double> outL = inL, outR = inR;
    processBoth(s, outL, outR);

    const NullResult nl = nullTest(inL.data(), outL.data(), inL.size());
    const NullResult nr = nullTest(inR.data(), outR.data(), inR.size());
    ASSERT_TRUE(nl.valid && nr.valid);
    EXPECT_TRUE(nl.bitExact) << "residuo L: " << nl.residualDb << " dB";
    EXPECT_TRUE(nr.bitExact) << "residuo R: " << nr.residualDb << " dB";
}

TEST(FusedSpatial, NeutralGainUnity) {
    FusedControls fc;
    fc.setNeutral();
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(fc.spatial);

    const std::vector<double> in = makeTone();
    std::vector<double> outL = in, outR = in;
    processBoth(s, outL, outR);

    const double rmsIn = std::sqrt([&] {
        double p = 0.0;
        for (double v : in) p += v * v;
        return p / static_cast<double>(in.size());
    }());
    const double rmsOut = std::sqrt([&] {
        double p = 0.0;
        for (double v : outL) p += v * v;
        return p / static_cast<double>(outL.size());
    }());
    EXPECT_NEAR(rmsOut, rmsIn, 1e-12);
}

TEST(FusedSpatial, NoNaNOrInf) {
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(engagedControls());
    ASSERT_FALSE(s.isNeutral());

    const std::vector<std::vector<double>> signals = {
        std::vector<double>(static_cast<std::size_t>(kFrames), 0.0),  // silencio
        makeDc(0.5),
        makeTone(0.99),
        makeNoise(0.99),
    };
    for (const auto& in : signals) {
        s.reset();
        std::vector<double> outL = in, outR = in;
        processBoth(s, outL, outR);
        EXPECT_TRUE(isFiniteBuffer(outL.data(), outL.size()));
        EXPECT_TRUE(isFiniteBuffer(outR.data(), outR.size()));
        EXPECT_LE(peakAbs(outL.data(), outL.size()), 1.5);
        EXPECT_LE(peakAbs(outR.data(), outR.size()), 1.5);
    }
}

TEST(FusedSpatial, ControlClamping) {
    FusedSpatial s;
    s.prepare(kSr);
    SpatialControls c;
    c.amount = 5.0f;
    c.azimuthDeg = 999.0f;
    c.distanceM = -3.0f;
    s.setControls(c);
    const SpatialControls got = s.controls();
    EXPECT_FLOAT_EQ(got.amount, 1.0f);
    EXPECT_FLOAT_EQ(got.azimuthDeg, 180.0f);
    EXPECT_FLOAT_EQ(got.distanceM, 0.15f);

    std::vector<double> l = makeTone(), r = makeTone();
    processBoth(s, l, r);
    EXPECT_TRUE(isFiniteBuffer(l.data(), l.size()));
    EXPECT_TRUE(isFiniteBuffer(r.data(), r.size()));
}

TEST(FusedSpatial, EngagedSmoke) {
    SpatialControls c;
    c.amount = 1.0f;
    c.azimuthDeg = 30.0f;
    c.distanceM = 1.0f;
    c.mode = OmegaOutputMode::HEADPHONES;  // resto: defaults (hrtfWet 1, wfs 0)
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(c);
    ASSERT_FALSE(s.isNeutral());

    const std::vector<double> inL = makeTone();
    const std::vector<double> inR = makeTone();
    std::vector<double> outL = inL, outR = inR;
    processBoth(s, outL, outR);

    const NullResult n = nullTest(inL.data(), outL.data(), inL.size());
    ASSERT_TRUE(n.valid);
    EXPECT_FALSE(n.bitExact);
    EXPECT_GT(n.maxAbsDiff, 1e-6);
    EXPECT_TRUE(isFiniteBuffer(outL.data(), outL.size()));
    EXPECT_TRUE(isFiniteBuffer(outR.data(), outR.size()));
    EXPECT_LT(peakAbs(outL.data(), outL.size()), 4.0);
    EXPECT_LT(peakAbs(outR.data(), outR.size()), 4.0);
}

TEST(FusedSpatial, ThdAtNeutral) {
    FusedControls fc;
    fc.setNeutral();
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(fc.spatial);

    const std::vector<double> in = makeTone();
    std::vector<double> outL = in, outR = in;
    processBoth(s, outL, outR);

    const ThdDoubleResult thd = measureThdDouble(outL.data(), outL.size(), kSr, 1000.0);
    const NullResult n = nullTest(in.data(), outL.data(), in.size());
    std::printf("[fused_spatial] THD+N neutro: %.1f dB (valid=%d, bitExact=%d)\n",
                thd.thdPlusNoiseDb, static_cast<int>(thd.valid),
                static_cast<int>(n.bitExact));
    ASSERT_TRUE(thd.valid);
    EXPECT_TRUE(thd.thdPlusNoiseDb <= -200.0 || n.bitExact);
}

// Puerta Genezi (Fix 1): en SPEAKERS la cadena binaural se neutraliza
// dentro del operador y la ruta de altavoz (XTC + AuxField) toma el relevo:
// mismo control, salida distinta a HEADPHONES, finita y acotada; y en
// neutro sigue siendo bit-exacta en ambos modos.
TEST(FusedSpatial, SpeakersModeGate) {
    const std::vector<double> in = makeTone();

    SpatialControls hp = engagedControls();
    hp.mode = OmegaOutputMode::HEADPHONES;
    hp.xtcEnabled = false;
    FusedSpatial sHp;
    sHp.prepare(kSr);
    sHp.setControls(hp);
    std::vector<double> outHpL = in, outHpR = in;
    processBoth(sHp, outHpL, outHpR);

    SpatialControls sp = engagedControls();
    sp.mode = OmegaOutputMode::SPEAKERS;
    sp.xtcEnabled = true;
    FusedSpatial sSp;
    sSp.prepare(kSr);
    sSp.setControls(sp);
    std::vector<double> outSpL = in, outSpR = in;
    processBoth(sSp, outSpL, outSpR);

    EXPECT_TRUE(isFiniteBuffer(outSpL.data(), outSpL.size()));
    EXPECT_TRUE(isFiniteBuffer(outSpR.data(), outSpR.size()));
    EXPECT_LE(peakAbs(outSpL.data(), outSpL.size()), 1.5);
    // La ruta de altavoz no es la binaural: las salidas deben diferir.
    const NullResult n = nullTest(outHpL.data(), outSpL.data(), in.size());
    ASSERT_TRUE(n.valid);
    EXPECT_FALSE(n.bitExact);

    // Neutro en SPEAKERS: bit-exacto también.
    FusedControls fc;
    fc.setNeutral();
    fc.spatial.mode = OmegaOutputMode::SPEAKERS;
    FusedSpatial sN;
    sN.prepare(kSr);
    sN.setControls(fc.spatial);
    std::vector<double> outNL = in, outNR = in;
    processBoth(sN, outNL, outNR);
    EXPECT_TRUE(nullTest(in.data(), outNL.data(), in.size()).bitExact);
}

// v2.5.471 (F2, parte segura): el flag headTracking tiene efecto audible
// cuando la pose está alimentada (feedRotationVector), y ninguno sin pose.
// Antes de la 471 nadie alimentaba la pose en Ruta A: el yaw quedaba en 0
// y el flag no se oía. El test alimenta un giro de 30° (vector de rotación
// eje Y) y exige que la salida con tracking ON difiera de la salida con
// tracking OFF, y que sin pose la salida ON sea idéntica a OFF (fail-closed,
// sin pose inventada).
TEST(FusedSpatial, HeadTrackingAudibleWhenPoseFed) {
    // Giro de 30° (yaw): eje Z (convención Z-up de yawDegrees).
    // rv = (0, 0, sin15°, cos15°); (0, sin15°, 0, cos15°) sería pitch, yaw 0.
    // Los timestamps son BOOTTIME reales (como SensorEvent.timestamp), todos
    // en el pasado: la fusión rechaza timestamps arbitrarios o futuros.
    struct timespec ts;
    ASSERT_EQ(::clock_gettime(CLOCK_BOOTTIME, &ts), 0);
    const float nowMs = static_cast<float>(ts.tv_sec) * 1000.0f +
                        static_cast<float>(ts.tv_nsec) / 1.0e6f;
    const float rv[4] = {0.0f, 0.0f, 0.2588190451f, 0.9659258263f};
    const std::vector<double> in = makeTone();
    // A: tracking ON + pose alimentada (10 eventos, 5 ms entre ellos).
    // upmixEnabled=false en los tres casos: con el upmixer HOA la etapa es
    // Hoa y la pose NO llega al render (el yaw solo alimenta ORBE/HRTF);
    // con upmix apagado la etapa es ORBE y el yaw es audible.
    SpatialControls cOn = engagedControls();
    cOn.headTracking = true;
    cOn.upmixEnabled = false;
    FusedSpatial sOn;
    sOn.prepare(kSr);
    sOn.setControls(cOn);
    for (int i = 0; i < 10; ++i)
        sOn.feedRotationVector(rv, nowMs - 100.0f + 5.0f * static_cast<float>(i));
    std::vector<double> outOnL = in, outOnR = in;
    processBoth(sOn, outOnL, outOnR);
    // B: tracking OFF (misma entrada, mismos controles salvo el flag).
    SpatialControls cOff = engagedControls();
    cOff.headTracking = false;
    cOff.upmixEnabled = false;
    FusedSpatial sOff;
    sOff.prepare(kSr);
    sOff.setControls(cOff);
    std::vector<double> outOffL = in, outOffR = in;
    processBoth(sOff, outOffL, outOffR);
    // C: tracking ON pero SIN pose (el yaw del tracker queda en 0: el
    // render usa solo el azimut del control).
    FusedSpatial sNoPose;
    sNoPose.prepare(kSr);
    sNoPose.setControls(cOn);
    std::vector<double> outNoPoseL = in, outNoPoseR = in;
    processBoth(sNoPose, outNoPoseL, outNoPoseR);
    // D: tracking ON + pose identidad (yaw 0): debe ser idéntica a C.
    // Es la propiedad fail-closed real: alimentar una pose neutra no
    // inventa ningún giro (el yaw del tracker es 0 en ambos casos).
    const float rvId[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    FusedSpatial sId;
    sId.prepare(kSr);
    sId.setControls(cOn);
    for (int i = 0; i < 10; ++i)
        sId.feedRotationVector(rvId, nowMs - 100.0f + 5.0f * static_cast<float>(i));
    std::vector<double> outIdL = in, outIdR = in;
    processBoth(sId, outIdL, outIdR);

    const NullResult nOnOff = nullTest(outOnL.data(), outOffL.data(), in.size());
    const NullResult nNoPoseId = nullTest(outNoPoseL.data(), outIdL.data(), in.size());
    const NullResult nOnNoPose = nullTest(outOnL.data(), outNoPoseL.data(), in.size());
    ASSERT_TRUE(nOnOff.valid);
    ASSERT_TRUE(nNoPoseId.valid);
    ASSERT_TRUE(nOnNoPose.valid);
    std::printf("[fused_spatial] headTracking: ON+pose vs OFF maxDiff=%.6g, "
                "ON sin pose vs identidad maxDiff=%.6g, ON+pose vs ON sin pose maxDiff=%.6g, yaw=%.2f deg\n",
                nOnOff.maxAbsDiff, nNoPoseId.maxAbsDiff, nOnNoPose.maxAbsDiff,
                sOn.trackerYawDeg());
    // El yaw fusionado debe reflejar el giro alimentado (~30°).
    EXPECT_NEAR(sOn.trackerYawDeg(), 30.0, 5.0);
    // Con pose, el tracking cambia el sonido de forma audible.
    EXPECT_FALSE(nOnOff.bitExact);
    EXPECT_GT(nOnOff.maxAbsDiff, 1e-3);
    // El giro de 30° cambia el sonido respecto al azimut solo.
    EXPECT_FALSE(nOnNoPose.bitExact);
    EXPECT_GT(nOnNoPose.maxAbsDiff, 1e-3);
    // Sin pose, el tracking no inventa nada: pose neutra == sin pose.
    EXPECT_TRUE(nNoPoseId.bitExact);
    EXPECT_EQ(nNoPoseId.maxAbsDiff, 0.0);
}

// F0 (v2.5.471): la latencia de seguimiento se mide y se expone.
// Se alimenta una pose con timestamp 5 ms en el pasado (base BOOTTIME,
// igual que SensorEvent.timestamp) y se exige que trackingTelemetry()
// reporte una latencia end-to-end sensata (≥0, <10 s) y el timestamp
// del sensor que se alimentó. Sin pose, la latencia es -1 (sin medición).
TEST(FusedSpatial, TrackingLatencyMeasured) {
    // Sin pose: sin medición.
    FusedSpatial sEmpty;
    sEmpty.prepare(kSr);
    sEmpty.setControls(engagedControls());
    float teleEmpty[5] = {0, 0, 0, 0, 0};
    sEmpty.trackingTelemetry(teleEmpty);
    EXPECT_EQ(teleEmpty[0], -1.0f);
    EXPECT_EQ(teleEmpty[1], -1.0f);

    // Con pose: 10 eventos con timestamps BOOTTIME reales, todos en el
    // pasado (100→55 ms atrás), para que la latencia sea positiva y sensata.
    struct timespec ts;
    ASSERT_EQ(::clock_gettime(CLOCK_BOOTTIME, &ts), 0);
    const float nowMs = static_cast<float>(ts.tv_sec) * 1000.0f +
                        static_cast<float>(ts.tv_nsec) / 1.0e6f;
    const float sensorMs = nowMs - 100.0f;
    const float rv[4] = {0.0f, 0.0f, 0.2588190451f, 0.9659258263f};
    SpatialControls c = engagedControls();
    c.headTracking = true;
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(c);
    for (int i = 0; i < 10; ++i)
        s.feedRotationVector(rv, sensorMs + 5.0f * static_cast<float>(i));
    const std::vector<double> in = makeTone();
    std::vector<double> outL = in, outR = in;
    processBoth(s, outL, outR);
    float tele[5] = {0, 0, 0, 0, 0};
    s.trackingTelemetry(tele);
    std::printf("[fused_spatial] tracking: latency=%.2f ms, jni=%.2f ms, "
                "yaw=%.2f deg, sensorTs=%.1f, appliedTs=%.1f\n",
                tele[0], tele[1], tele[2], tele[3], tele[4]);
    // La latencia end-to-end es la real medida (sensor→bloque renderizado).
    EXPECT_GE(tele[0], 0.0f);
    EXPECT_LT(tele[0], 10000.0f);
    EXPECT_GE(tele[1], 0.0f);
    EXPECT_LT(tele[1], 10000.0f);
    // El timestamp del sensor reportado es el alimentado (último evento).
    EXPECT_NEAR(tele[3], sensorMs + 45.0f, 1.0f);
    // El yaw aplicado refleja el giro.
    // El yaw aplicado es azimut (30°) + yaw del tracker (30°) = 60°.
    EXPECT_NEAR(tele[2], 60.0, 5.0);
}

// v2.5.472 (Fase 2, ítem E): parallaxAmount (SpatialControls) escala el
// yaw aplicado: yaw = azimut + trackerYaw × parallaxAmount. Con 0 el
// tracker no mueve el sonido: la salida es bit-idéntica a tracking ON
// SIN pose (azimut solo), y el yaw aplicado es solo el azimut.
// OJO (conducta preexistente, OrbeBinaural.cpp:296): con headTracking ON,
// ORBE usa el yawRad_ que le pasa applyPose (azimut + tracker×par); con
// OFF, ORBE usa yaw 0 — el azimut del control SOLO llega a ORBE cuando
// headTracking está ON. Así que ON+parallax 0 NO es igual a OFF (el OFF
// pierde el azimut en ORBE); la igualdad correcta es ON+par0 == ON sin
// pose. Ese quirk es de OrbeBinaural (fuera de ámbito), no del parallax.
TEST(FusedSpatial, ParallaxAmountZeroMutesTracker) {
    struct timespec ts;
    ASSERT_EQ(::clock_gettime(CLOCK_BOOTTIME, &ts), 0);
    const float nowMs = static_cast<float>(ts.tv_sec) * 1000.0f +
                        static_cast<float>(ts.tv_nsec) / 1.0e6f;
    const float rv[4] = {0.0f, 0.0f, 0.2588190451f, 0.9659258263f};  // yaw 30°
    const std::vector<double> in = makeTone();

    SpatialControls cOn = engagedControls();
    cOn.headTracking = true;
    cOn.upmixEnabled = false;
    cOn.parallaxAmount = 0.0f;
    FusedSpatial sZero;
    sZero.prepare(kSr);
    sZero.setControls(cOn);
    for (int i = 0; i < 10; ++i)
        sZero.feedRotationVector(rv, nowMs - 100.0f + 5.0f * static_cast<float>(i));
    std::vector<double> outZeroL = in, outZeroR = in;
    processBoth(sZero, outZeroL, outZeroR);

    // Tracking ON pero SIN pose: mismo azimut (30°), tracker en 0.
    FusedSpatial sNoPose;
    sNoPose.prepare(kSr);
    sNoPose.setControls(cOn);
    std::vector<double> outNoPoseL = in, outNoPoseR = in;
    processBoth(sNoPose, outNoPoseL, outNoPoseR);

    const NullResult n = nullTest(outZeroL.data(), outNoPoseL.data(), in.size());
    ASSERT_TRUE(n.valid);
    std::printf("[fused_spatial] parallax 0: vs ON-sin-pose maxDiff=%.6g, yaw tracker=%.2f deg\\n",
                n.maxAbsDiff, sZero.trackerYawDeg());
    EXPECT_TRUE(n.bitExact);
    EXPECT_EQ(n.maxAbsDiff, 0.0);

    // Telemetría: con parallax 0 el yaw aplicado es solo el azimut (30°).
    float tele[5] = {0, 0, 0, 0, 0};
    sZero.trackingTelemetry(tele);
    EXPECT_NEAR(tele[2], 30.0, 5.0);
}

TEST(FusedSpatial, ParallaxAmountScalesAppliedYaw) {
    struct timespec ts;
    ASSERT_EQ(::clock_gettime(CLOCK_BOOTTIME, &ts), 0);
    const float nowMs = static_cast<float>(ts.tv_sec) * 1000.0f +
                        static_cast<float>(ts.tv_nsec) / 1.0e6f;
    const float rv[4] = {0.0f, 0.0f, 0.2588190451f, 0.9659258263f};  // yaw 30°
    const std::vector<double> in = makeTone();
    for (const float par : {1.0f, 0.5f, 0.0f}) {
        SpatialControls c = engagedControls();
        c.headTracking = true;
        c.upmixEnabled = false;
        c.parallaxAmount = par;
        FusedSpatial s;
        s.prepare(kSr);
        s.setControls(c);
        for (int i = 0; i < 10; ++i)
            s.feedRotationVector(rv, nowMs - 100.0f + 5.0f * static_cast<float>(i));
        std::vector<double> outL = in, outR = in;
        processBoth(s, outL, outR);
        float tele[5] = {0, 0, 0, 0, 0};
        s.trackingTelemetry(tele);
        const float expected = 30.0f + 30.0f * par;  // azimut + yaw × par
        std::printf("[fused_spatial] parallax %.1f: yaw aplicado=%.2f (esperado %.1f)\\n",
                    par, tele[2], expected);
        EXPECT_NEAR(tele[2], expected, 5.0);
    }
}

// v2.5.472 (Fase 2, ítem E): SO(3) audible — el pitch del tracker llega
// al render por la etapa HRTF (setHeadPose(yaw, pitch, roll) →
// synthesizeHrtf → LGN applyPinnaFir con phi=pitch: el notch de pinna
// se mueve con la elevación). Etapa HRTF: amount=0 (ORBE fuera),
// hrtfWet=1, upmix apagado. El roll NO es audible en el render actual:
// synthesizeHrtf lo descarta ((void)roll) y ORBE solo usa el yaw — el
// roll solo se verifica a nivel de tracker (test_head_tracker_so3).
TEST(FusedSpatial, PitchAudibleViaHrtfStage) {
    struct timespec ts;
    ASSERT_EQ(::clock_gettime(CLOCK_BOOTTIME, &ts), 0);
    const float nowMs = static_cast<float>(ts.tv_sec) * 1000.0f +
                        static_cast<float>(ts.tv_nsec) / 1.0e6f;
    // Pitch 45°: giro sobre Y (rv = (0, sin22.5°, 0, cos22.5°)).
    const float rvPitch[4] = {0.0f, 0.3826834324f, 0.0f, 0.9238795325f};
    const std::vector<double> in = makeTone();

    SpatialControls c = engagedControls();
    // ORBE fuera → etapa HRTF: con amount = 0 la etapa binaural es la
    // HRTF y el pitch del tracker se oye por ella. La puerta HRTF
    // (phone-loop) NO cierra hrtfWet aquí porque headTracking está
    // activo: con tracking, la pose ES la escena binaural.
    c.amount = 0.0f;       // ORBE fuera → etapa HRTF
    c.azimuthDeg = 0.0f;
    c.wfsSpread = 0.0f;
    c.upmixEnabled = false;
    c.headTracking = true;
    c.parallaxAmount = 1.0f;
    FusedSpatial sPitch;
    sPitch.prepare(kSr);
    sPitch.setControls(c);
    for (int i = 0; i < 10; ++i)
        sPitch.feedRotationVector(rvPitch, nowMs - 100.0f + 5.0f * static_cast<float>(i));
    std::vector<double> outPitchL = in, outPitchR = in;
    processBoth(sPitch, outPitchL, outPitchR);

    FusedSpatial sNoPose;
    sNoPose.prepare(kSr);
    sNoPose.setControls(c);
    std::vector<double> outNoPoseL = in, outNoPoseR = in;
    processBoth(sNoPose, outNoPoseL, outNoPoseR);

    const NullResult n = nullTest(outPitchL.data(), outNoPoseL.data(), in.size());
    ASSERT_TRUE(n.valid);
    std::printf("[fused_spatial] pitch 45deg HRTF: vs sin pose maxDiff=%.6g, pitch=%.2f deg\\n",
                n.maxAbsDiff, sPitch.trackerPitchDeg());
    EXPECT_NEAR(sPitch.trackerPitchDeg(), 45.0, 5.0);
    EXPECT_NEAR(sPitch.trackerYawDeg(), 0.0, 2.0);
    EXPECT_FALSE(n.bitExact);
    EXPECT_GT(n.maxAbsDiff, 1e-3);
}

// Pan Poincaré OFF por defecto (2026-11-23): con el flag apagado la etapa
// WFS debe producir EXACTAMENTE la salida de la cadena actual. Este test
// clava la salida WFS con muestras doradas capturadas ANTES del cableado
// (rama poincare-wire, pre-cambio): si el cableado toca el camino OFF,
// el test cae. La etapa es Wfs (amount 0, hrtfWet 0, upmix off,
// wfsSpread 1): ORBE/HRTF/HOA no intervienen.
TEST(FusedSpatial, WfsStageGoldenWithPoincareOff) {
    SpatialControls c;
    c.amount = 0.0f;
    c.azimuthDeg = 0.0f;
    c.distanceM = 1.0f;
    c.width = 1.0f;
    c.mode = OmegaOutputMode::HEADPHONES;
    c.wfsSpread = 1.0f;
    c.hrtfWet = 0.0f;
    c.headTracking = false;
    c.xtcEnabled = false;
    c.upmixEnabled = false;
    c.clamp();
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(c);
    ASSERT_FALSE(s.isNeutral());

    const std::vector<double> in = makeTone();  // 1 kHz, 1 s, -3 dBFS
    std::vector<double> outL = in, outR = in;
    processBoth(s, outL, outR);
    ASSERT_TRUE(isFiniteBuffer(outL.data(), outL.size()));
    ASSERT_TRUE(isFiniteBuffer(outR.data(), outR.size()));

    // Muestras doradas (L/R) en índices fijos, capturadas pre-cambio
    // (%.17g). RMS total como segunda clava. Tolerancia 1e-7, NO igualdad
    // exacta: los dorados se capturaron con g++-14 y la CI compila con
    // clang++-18, que difiere por FMA/vectorización en ~1e-8 (medido en CI:
    // máx 8.3e-9). Un cambio real del cableado movería las muestras órdenes
    // de magnitud más; 1e-7 lo sigue cazando y el ruido de compilador no.
    static constexpr int kIdx[] = {100, 1000, 5000, 12000, 24000, 36000, 47999};
    static constexpr double kGoldenL[] = {0.35355339059326818, -0.0048235804904559441, -0.076335525334705667, -0.13175913760067637, -0.11161374585411231, -0.1116137425019425, -0.11843232632974263};
    static constexpr double kGoldenR[] = {0.35355339059326818, -0.0048238491737545245, -0.07633518203642646, -0.13175887588624424, -0.11161375142763194, -0.11161374807459326, -0.11843235231921902};
    static constexpr double kGoldenRmsL = 0.099134853935782405;
    static constexpr double kGoldenRmsR = 0.099134820869615753;
    static constexpr double kGoldenTol = 1e-7;
    double rmsL = 0.0, rmsR = 0.0;
    for (size_t i = 0; i < outL.size(); ++i) {
        rmsL += outL[i] * outL[i];
        rmsR += outR[i] * outR[i];
    }
    rmsL = std::sqrt(rmsL / static_cast<double>(outL.size()));
    rmsR = std::sqrt(rmsR / static_cast<double>(outR.size()));
    bool ok = true;
    for (size_t i = 0; i < 7; ++i) {
        if (std::fabs(outL[static_cast<size_t>(kIdx[i])] - kGoldenL[i]) > kGoldenTol ||
            std::fabs(outR[static_cast<size_t>(kIdx[i])] - kGoldenR[i]) > kGoldenTol)
            ok = false;
    }
    if (std::fabs(rmsL - kGoldenRmsL) > kGoldenTol || std::fabs(rmsR - kGoldenRmsR) > kGoldenTol) ok = false;
    if (!ok) {
        std::printf("[fused_spatial] GOLDEN L: {");
        for (size_t i = 0; i < 7; ++i)
            std::printf("%.17g%s", outL[static_cast<size_t>(kIdx[i])],
                        i + 1 < 7 ? ", " : "");
        std::printf("}\n[fused_spatial] GOLDEN R: {");
        for (size_t i = 0; i < 7; ++i)
            std::printf("%.17g%s", outR[static_cast<size_t>(kIdx[i])],
                        i + 1 < 7 ? ", " : "");
        std::printf("}\n[fused_spatial] GOLDEN RMS L=%.17g R=%.17g\n", rmsL, rmsR);
    }
    for (size_t i = 0; i < 7; ++i) {
        EXPECT_NEAR(outL[static_cast<size_t>(kIdx[i])], kGoldenL[i], kGoldenTol)
            << "muestra dorada L idx=" << kIdx[i];
        EXPECT_NEAR(outR[static_cast<size_t>(kIdx[i])], kGoldenR[i], kGoldenTol)
            << "muestra dorada R idx=" << kIdx[i];
    }
    EXPECT_NEAR(rmsL, kGoldenRmsL, kGoldenTol) << "RMS dorado L";
    EXPECT_NEAR(rmsR, kGoldenRmsR, kGoldenTol) << "RMS dorado R";
}

// Puerta HRTF (phone-loop, 2026-11-26): con ORBE apagado (amount = 0) la
// HRTF no tiene escena binaural que renderizar. El slider dejaba
// hrtfWet = 1.0, así que el operador no era neutro y la cadena seguía
// corriendo la HRTF sintética de reserva (notch de −44 dB en R a
// 11.5 kHz). SpatialControls::clamp() fuerza hrtfWet = 0 cuando
// amount = 0: el operador es neutro y el bypass es bit-exacto.
TEST(FusedSpatial, HrtfGateWhenOrbeOff) {
    // (a) El clamp cierra la puerta: hrtfWet 1.0 -> 0.0 y neutro.
    SpatialControls c;
    c.amount = 0.0f;
    c.hrtfWet = 1.0f;  // como lo deja el slider al apagar ORBE
    c.mode = OmegaOutputMode::HEADPHONES;
    c.clamp();
    EXPECT_FLOAT_EQ(c.hrtfWet, 0.0f);
    EXPECT_TRUE(c.isNeutral());

    // (b) Tono de 11.5 kHz (frecuencia del notch de reserva) por el
    // operador: salida bit-idéntica a la entrada en L y R.
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(c);
    ASSERT_TRUE(s.isNeutral());
    EXPECT_FLOAT_EQ(s.controls().hrtfWet, 0.0f);
    const std::vector<double> inL = makeTone(kToneAmp, 11500.0);
    const std::vector<double> inR = makeTone(kToneAmp, 11500.0);
    std::vector<double> outL = inL, outR = inR;
    processBoth(s, outL, outR);
    const NullResult nl = nullTest(inL.data(), outL.data(), inL.size());
    const NullResult nr = nullTest(inR.data(), outR.data(), inR.size());
    ASSERT_TRUE(nl.valid && nr.valid);
    EXPECT_TRUE(nl.bitExact) << "residuo L: " << nl.residualDb << " dB";
    EXPECT_TRUE(nr.bitExact) << "residuo R: " << nr.residualDb << " dB";

    // (c) Regresión: con amount = 1.0 y hrtfWet = 1.0 el comportamiento
    // no cambia: la HRTF sigue activa y la salida difiere de la entrada.
    SpatialControls e;
    e.amount = 1.0f;
    e.azimuthDeg = 30.0f;
    e.distanceM = 1.0f;
    e.mode = OmegaOutputMode::HEADPHONES;  // resto: defaults (hrtfWet 1)
    e.clamp();
    EXPECT_FLOAT_EQ(e.hrtfWet, 1.0f);
    FusedSpatial sE;
    sE.prepare(kSr);
    sE.setControls(e);
    ASSERT_FALSE(sE.isNeutral());
    const std::vector<double> in1k = makeTone();
    std::vector<double> outEL = in1k, outER = in1k;
    processBoth(sE, outEL, outER);
    const NullResult ne = nullTest(in1k.data(), outEL.data(), in1k.size());
    ASSERT_TRUE(ne.valid);
    EXPECT_FALSE(ne.bitExact);
    EXPECT_GT(ne.maxAbsDiff, 1e-6);
    EXPECT_TRUE(isFiniteBuffer(outEL.data(), outEL.size()));
    EXPECT_TRUE(isFiniteBuffer(outER.data(), outER.size()));
}

}  // namespace
