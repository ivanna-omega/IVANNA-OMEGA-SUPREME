// test_head_tracker_so3.cpp — SO(3) completo + parallax por traslación
// (Fase 2, ítem E, v2.5.472). El dueño levantó la pausa del tracker SO(3)
// ("Espera" 2026-11-11 supersedida).
//
// Qué fija:
//   1. El cuaternión extrae yaw+pitch+roll (ZYX, misma convención que
//      yawDegrees()): un giro sobre Y es pitch puro, sobre X es roll puro.
//   2. HeadTrackerFusion publica los 3 ángulos (antes solo el yaw; pitch/
//      roll existían en FusedPose pero nunca se llenaban).
//   3. Fusión gyro+RV: el giroscopio integra la actitud rápida y el RV la
//      corrige con un polo lento (tau ~2 s); un giro sostenido NO se
//      absorbe como sesgo (el sesgo solo se estima en reposo) y el RV
//      tira de vuelta la deriva del integrador.
//   4. Parallax por traslación (NearFieldHeadModel): con oyente en
//      (0,0,0) la geometría es idéntica; con el oyente movido, el azimut
//      y la distancia efectivos se recalculan y el ITD/ILD cambian en el
//      sentido físico (fuente más cerca → ITD mayor).
//   5. El paquete CBU (14/14 tests) NO está en el repo: no hay ningún
//      archivo CBU que usar como referencia; la referencia es este test +
//      HeadTrackerFusion (filtro complementario + predicción).
#include <gtest/gtest.h>

#include "spatial/HeadTrackerFusion.hpp"
#include "spatial/NearFieldHeadModel.hpp"

#include <cmath>

using ivanna::spatial::HeadTrackerFusion;
using ivanna::spatial::Quaternion;

namespace {

void rotationVectorAxis(float ax, float ay, float az, float degrees, float out[4]) {
    const float half = degrees * static_cast<float>(M_PI) / 360.0f;
    const float s = std::sin(half);
    out[0] = ax * s;
    out[1] = ay * s;
    out[2] = az * s;
    out[3] = std::cos(half);
}

}  // namespace

TEST(HeadTrackerSo3, QuaternionExtractsYawPitchRoll) {
    Quaternion qYaw, qPitch, qRoll;
    float rv[4];
    rotationVectorAxis(0.f, 0.f, 1.f, 30.f, rv);
    qYaw.fromRotationVector(rv);
    rotationVectorAxis(0.f, 1.f, 0.f, 30.f, rv);
    qPitch.fromRotationVector(rv);
    rotationVectorAxis(1.f, 0.f, 0.f, 30.f, rv);
    qRoll.fromRotationVector(rv);

    EXPECT_NEAR(qYaw.yawDegrees(), 30.0, 0.05);
    EXPECT_NEAR(qYaw.pitchDegrees(), 0.0, 0.05);
    EXPECT_NEAR(qYaw.rollDegrees(), 0.0, 0.05);

    EXPECT_NEAR(qPitch.pitchDegrees(), 30.0, 0.05);
    EXPECT_NEAR(qPitch.yawDegrees(), 0.0, 0.05);
    EXPECT_NEAR(qPitch.rollDegrees(), 0.0, 0.05);

    EXPECT_NEAR(qRoll.rollDegrees(), 30.0, 0.05);
    EXPECT_NEAR(qRoll.yawDegrees(), 0.0, 0.05);
    EXPECT_NEAR(qRoll.pitchDegrees(), 0.0, 0.05);
}

TEST(HeadTrackerSo3, FusionPublishesAllThreeAxes) {
    HeadTrackerFusion f;
    f.prepare(48000.0f);
    float rv[4];
    // Pitch 30° (giro sobre Y).
    rotationVectorAxis(0.f, 1.f, 0.f, 30.f, rv);
    f.updateRotationVector(rv, 1000.0f);
    const auto pPitch = f.latest();
    EXPECT_NEAR(pPitch.pitchDeg, 30.0, 0.5);
    EXPECT_NEAR(pPitch.yawDeg, 0.0, 0.5);
    EXPECT_NEAR(pPitch.rollDeg, 0.0, 0.5);
    // Roll 30° (giro sobre X).
    rotationVectorAxis(1.f, 0.f, 0.f, 30.f, rv);
    f.updateRotationVector(rv, 1010.0f);
    const auto pRoll = f.latest();
    EXPECT_NEAR(pRoll.rollDeg, 30.0, 0.5);
    EXPECT_NEAR(pRoll.yawDeg, 0.0, 0.5);
    EXPECT_NEAR(pRoll.pitchDeg, 0.0, 0.5);
    // Yaw 30° (giro sobre Z).
    rotationVectorAxis(0.f, 0.f, 1.f, 30.f, rv);
    f.updateRotationVector(rv, 1020.0f);
    const auto pYaw = f.latest();
    EXPECT_NEAR(pYaw.yawDeg, 30.0, 0.5);
    // offsetForSpatial3D expone los 3 ángulos al render.
    float az = 0.f, lat = 0.f, pitch = 0.f, roll = 0.f;
    f.offsetForSpatial3D(az, lat, pitch, roll);
    EXPECT_NEAR(az, 30.0, 0.5);
    EXPECT_NEAR(pitch, 0.0, 0.5);
    EXPECT_NEAR(roll, 0.0, 0.5);
    float yaw = 0.f, p = 0.f, r = 0.f;
    f.poseEulerDeg(yaw, p, r);
    EXPECT_NEAR(yaw, 30.0, 0.5);
}

TEST(HeadTrackerSo3, GyroIntegratesSustainedTurnWithoutBiasAbsorption) {
    HeadTrackerFusion f;
    f.prepare(48000.0f);
    // Giroscopio a 30°/s sobre Z durante 2 s (pasos de 10 ms). La primera
    // muestra solo fija el reloj; las 200 siguientes integran 2.0 s.
    // 30°/s es movimiento real (>10°/s): el sesgo NO debe absorberlo.
    const float w = 30.0f * static_cast<float>(M_PI) / 180.0f;
    float gyro[3] = {0.f, 0.f, w};
    f.updateGyro(gyro, 1000.0f);
    for (int i = 1; i <= 200; ++i)
        f.updateGyro(gyro, 1000.0f + 10.0f * static_cast<float>(i));
    const auto pose = f.latest();
    std::printf("[so3] gyro 30deg/s x 2s: yaw=%.2f (esperado ~60)\\n", pose.yawDeg);
    EXPECT_NEAR(pose.yawDeg, 60.0, 6.0);
    EXPECT_NEAR(pose.pitchDeg, 0.0, 2.0);
    EXPECT_NEAR(pose.rollDeg, 0.0, 2.0);
}

TEST(HeadTrackerSo3, RotationVectorPullsGyroDriftBack) {
    HeadTrackerFusion f;
    f.prepare(48000.0f);
    // 1. Integrar con gyro hasta ~60° de yaw.
    const float w = 30.0f * static_cast<float>(M_PI) / 180.0f;
    float gyro[3] = {0.f, 0.f, w};
    f.updateGyro(gyro, 1000.0f);
    for (int i = 1; i <= 200; ++i)
        f.updateGyro(gyro, 1000.0f + 10.0f * static_cast<float>(i));
    EXPECT_NEAR(f.latest().yawDeg, 60.0, 6.0);
    // 2. RV fijo en 30° de yaw, 200 muestras a 10 ms: el polo lento
    // (tau 2 s) tira del integrador hacia el RV sin saltos.
    float rv[4];
    rotationVectorAxis(0.f, 0.f, 1.f, 30.f, rv);
    for (int i = 1; i <= 200; ++i)
        f.updateRotationVector(rv, 3000.0f + 10.0f * static_cast<float>(i));
    const float yawAfter = f.latest().yawDeg;
    std::printf("[so3] RV 30deg x 200: yaw=%.2f (de ~60 hacia 30)\\n", yawAfter);
    EXPECT_LT(yawAfter, 50.0f);  // convergió hacia el RV
    EXPECT_GT(yawAfter, 30.0f);  // sin saltos: el polo es lento
}

TEST(HeadTrackerSo3, TranslationHookDefaultsToZero) {
    HeadTrackerFusion f;
    f.prepare(48000.0f);
    EXPECT_FALSE(f.hasListenerTranslation());
    float t[3] = {9.f, 9.f, 9.f};
    f.listenerTranslation(t);
    EXPECT_FLOAT_EQ(t[0], 0.0f);
    EXPECT_FLOAT_EQ(t[1], 0.0f);
    EXPECT_FLOAT_EQ(t[2], 0.0f);
    f.setListenerTranslation(0.1f, -0.05f, 0.0f);
    EXPECT_TRUE(f.hasListenerTranslation());
    f.listenerTranslation(t);
    EXPECT_FLOAT_EQ(t[0], 0.1f);
    EXPECT_FLOAT_EQ(t[1], -0.05f);
    EXPECT_FLOAT_EQ(t[2], 0.0f);
    // No finitos se rechazan sin tocar el estado.
    f.setListenerTranslation(std::nanf(""), 0.f, 0.f);
    f.listenerTranslation(t);
    EXPECT_FLOAT_EQ(t[0], 0.1f);
}

TEST(NearFieldTranslation, ZeroListenerIsIdentity) {
    using namespace ivanna::nearfield;
    const ListenerVec zero{};
    // FIX 487: az/d en double — sourceRelativeToListener toma double&
    // desde la conversión a double (486); un float no se liga a double&.
    double az = 0.0, d = 0.0;
    sourceRelativeToListener(0.5f, 1.0f, zero, az, d);
    EXPECT_NEAR(az, 0.5, 1e-6);
    EXPECT_NEAR(d, 1.0, 1e-6);
    EXPECT_NEAR(itdSecondsTranslated(0.5f, 1.0f, zero), itdSeconds(0.5f, 1.0f), 1e-9);
    EXPECT_NEAR(ildDbTranslated(0.5f, 1.0f, 4000.f, zero), ildDb(0.5f, 1.0f, 4000.f), 1e-6);
}

TEST(NearFieldTranslation, ListenerShiftRecomputesAzimuthAndDistance) {
    using namespace ivanna::nearfield;
    // Fuente frontal a 1 m (az 0). Oyente +0.1 m en x (hacia el frente):
    // la fuente queda a la izquierda del oyente trasladado.
    const ListenerVec forward{0.1f, 0.f, 0.f};
    // FIX 487: az/d en double (ver arriba).
    double az = 0.0, d = 0.0;
    sourceRelativeToListener(0.0f, 1.0f, forward, az, d);
    EXPECT_NEAR(az, std::atan2(-0.1f, 1.0f), 1e-6);
    EXPECT_NEAR(d, std::sqrt(1.01f), 1e-6);
    // Fuente a 90° (derecha) a 1 m. Oyente +0.1 m en x (hacia la fuente):
    // distancia efectiva 0.9 m → ITD mayor (campo cercano: el ITD crece
    // al acercarse la fuente, ~+12 % a 0.15 m / 90°).
    const float itdFar = itdSeconds(1.5707963f, 1.0f);
    const float itdNear = itdSecondsTranslated(1.5707963f, 1.0f, forward);
    EXPECT_GT(itdFar, 0.0f);
    EXPECT_GT(itdNear, itdFar);
    // ILD también crece al acercarse (divergencia esférica 1/d).
    const float ildFar = ildDb(1.5707963f, 1.0f, 4000.f);
    const float ildNear = ildDbTranslated(1.5707963f, 1.0f, 4000.f, forward);
    EXPECT_GT(ildNear, ildFar);
}
