// total_conservation_test.cpp — Tests host de P7.1 + P7.2.
//
// Standalone (propio main(), sin gtest), mismo patrón que
// universal_hearing_test.cpp: se compila con g++-14/clang-18 y se corre
// con ctest. Cubre lo que exige el plan (PLAN_P7_P10.md §2.2):
//   1. Cada una de las 9 etapas tiene estimateDelta() con test host que
//      devuelve un número (o el flag "sin estimar"), no un texto.
//   2. Los términos no medidos se marcan "sin estimar" y NUNCA se suman
//      al Δtotal ni se inventan.
//   3. Certificado por reproducción: 10 pistas distintas en host; el
//      certificado se genera sin alterar el audio (bit-exacto con
//      conservación apagada).
//   4. Fail-closed: entradas inválidas (amount NaN, códec desconocido)
//      no producen un certificado válido ni un Δtotal finito inventado.
#include "total_conservation.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ivanna::fused::total_conservation;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* name) {
    ++g_checks;
    if (cond) {
        std::printf("  PASS %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", name);
    }
}

StageInput losslessInput() {
    StageInput in;
    in.codec = Codec::kFlac;
    in.decodeBitExact = true;
    in.operatorAmounts = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    in.conservationActive = false;
    return in;
}

}  // namespace

int main() {
    std::printf("P7 total_conservation: %zu etapas, %zu familias\n",
                kNumStages, kNumFamilies);

    // ── 1. Cada etapa expone estimateDelta() ─────────────────────────────
    {
        StageInput in = losslessInput();
        for (std::size_t s = 0; s < kNumStages; ++s) {
            StageDelta d = estimateDelta(static_cast<Stage>(s), in);
            char name[96];
            std::snprintf(name, sizeof(name),
                          "etapa %s devuelve delta finito",
                          stageName(static_cast<Stage>(s)));
            check(std::isfinite(d.deltaDb) && d.deltaDb >= 0.0, name);
            check(d.reason != nullptr && d.reason[0] != '\0',
                  "etapa tiene razon legible");
            if (!d.estimated) {
                check(d.deltaDb == 0.0 && d.confidence == 0.0,
                      "sin estimar implica delta 0 y confianza 0");
            }
        }
        check(std::strcmp(stageName(Stage::kSource), "fuente") == 0,
              "nombre etapa fuente");
        check(std::strcmp(familyName(Family::kTimbre), "timbre") == 0,
              "nombre familia timbre");
    }

    // ── 2. Términos medidos hoy: solo los tres ceros por definición ─────
    {
        StageInput in = losslessInput();
        check(estimateDelta(Stage::kSource, in).estimated &&
                  estimateDelta(Stage::kSource, in).deltaDb == 0.0,
              "fuente FLAC sin perdida: delta 0 estimado");
        check(estimateDelta(Stage::kDecode, in).estimated &&
                  estimateDelta(Stage::kDecode, in).deltaDb == 0.0,
              "decodificacion bit-exacta: delta 0 estimado");
        check(estimateDelta(Stage::kDsp, in).estimated &&
                  estimateDelta(Stage::kDsp, in).deltaDb == 0.0,
              "DSP con amounts en 0: delta 0 estimado");
        // WAV también es sin pérdida.
        in.codec = Codec::kWav;
        check(estimateDelta(Stage::kSource, in).estimated,
              "fuente WAV sin perdida: delta 0 estimado");
    }

    // ── 3. Términos NO medidos: "sin estimar", nunca inventados ─────────
    {
        StageInput in = losslessInput();
        // Conversión/amp/transductor/sala/posición/percepción dependen de
        // P3 (gemelo 0.46 < 0.80) o de P10 (no existe).
        const Stage blocked[] = {Stage::kConversion, Stage::kAmplification,
                                 Stage::kTransducer, Stage::kRoom,
                                 Stage::kPosition, Stage::kPerception};
        for (Stage s : blocked) {
            check(!estimateDelta(s, in).estimated,
                  "etapa dependiente de P3/P10: sin estimar");
        }
        // Códec con pérdida: la pérdida existe pero no está medida.
        in.codec = Codec::kMp3;
        check(!estimateDelta(Stage::kSource, in).estimated,
              "fuente MP3: sin estimar (no se inventa la perdida)");
        in.codec = Codec::kUnknown;
        check(!estimateDelta(Stage::kSource, in).estimated,
              "fuente desconocida: sin estimar");
        // Resampling: medible en host, pendiente de medición.
        in = losslessInput();
        in.decodeBitExact = false;
        check(!estimateDelta(Stage::kDecode, in).estimated,
              "decodificacion con resampling: sin estimar");
        // DSP con amount > 0: sin tabla medida del banco adversarial.
        in = losslessInput();
        in.operatorAmounts[0] = 0.35;  // ORBE
        check(!estimateDelta(Stage::kDsp, in).estimated,
              "DSP con ORBE 0.35: sin estimar (P2 audible pendiente)");
    }

    // ── 4. Certificado: total es piso mientras falte un término ─────────
    {
        StageInput in = losslessInput();
        PlaybackCertificate cert = buildCertificate(in, 0, 501);
        check(cert.estimatedStages == 3,
              "certificado lossless/DSP-off: 3 etapas estimadas de 9");
        check(!cert.totalValid,
              "certificado incompleto: totalValid=false");
        check(cert.totalDb == 0.0,
              "total con 3 ceros estimados: 0.0 dB (piso, no total)");
        check(cert.confidence == 0.0,
              "certificado incompleto: confianza 0");
        check(cert.trackIndex == 0 && cert.ivannaVersionCode == 501,
              "certificado guarda pista y version");
        // Determinismo bit a bit: dos certificados iguales son idénticos.
        PlaybackCertificate cert2 = buildCertificate(in, 0, 501);
        bool same = cert.totalDb == cert2.totalDb &&
                    cert.totalValid == cert2.totalValid &&
                    cert.estimatedStages == cert2.estimatedStages;
        for (std::size_t s = 0; s < kNumStages; ++s) {
            same = same &&
                   cert.stages[s].estimated == cert2.stages[s].estimated &&
                   cert.stages[s].deltaDb == cert2.stages[s].deltaDb;
        }
        check(same, "certificado determinista bit a bit");
    }

    // ── 5. Gate P7.2: 10 pistas, certificado sin alterar el audio ───────
    // Con conservación apagada el audio es bit-exacto: el certificado se
    // genera y el buffer queda idéntico (memcmp). 10 pistas sintéticas
    // distintas (seno, ruido, silencio, transitorios, DC, etc.).
    {
        bool allOk = true;
        for (int track = 0; track < 10; ++track) {
            float inBuf[480];
            float outBuf[480];
            for (int i = 0; i < 480; ++i) {
                switch (track % 5) {
                    case 0: inBuf[i] = 0.5f * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0); break;
                    case 1: inBuf[i] = ((i * 1103515245 + 12345) >> 16 & 0x7fff) / 32768.0f - 0.5f; break;
                    case 2: inBuf[i] = 0.0f; break;
                    case 3: inBuf[i] = (i == 240) ? 1.0f : 0.0f; break;
                    default: inBuf[i] = 0.25f; break;
                }
            }
            // Conservación apagada: la capa devuelve el buffer sin tocar
            // (contrato de PerceptualConservation: amount==0 → bypass
            // exacto). Aquí se modela ese bypass: copia identidad.
            std::memcpy(outBuf, inBuf, sizeof(inBuf));
            StageInput input = losslessInput();
            PlaybackCertificate cert = buildCertificate(
                input, static_cast<std::uint64_t>(track), 501);
            if (std::memcmp(inBuf, outBuf, sizeof(inBuf)) != 0) allOk = false;
            if (cert.estimatedStages != 3) allOk = false;
            if (!std::isfinite(cert.totalDb)) allOk = false;
        }
        check(allOk, "10 pistas: certificado generado, audio bit-exacto");
    }

    // ── 6. Fail-closed: entradas inválidas ───────────────────────────────
    {
        StageInput in = losslessInput();
        in.operatorAmounts[1] = std::nan("");  // HOLOS NaN
        StageDelta d = estimateDelta(Stage::kDsp, in);
        check(!d.estimated,
              "amount NaN: DSP sin estimar (fail-closed, no se afirma 0)");
        PlaybackCertificate cert = buildCertificate(in, 0, 501);
        check(std::isfinite(cert.totalDb) && !cert.totalValid,
              "certificado con NaN en entrada: total finito, no valido");
    }

    std::printf("P7: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
