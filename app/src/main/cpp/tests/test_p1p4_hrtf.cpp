// test_p1p4_hrtf.cpp — REFINAMIENTO TOTAL MAGISTRAL, P1+P4 (2026-11-26).
//
// HRTF (verificación en código + telemetría preparada):
//   1) Qué camino corre: Data.IR medido vs fallback PCA — tasa de
//      parseo real sobre los 239 SOFA del módulo
//      (magisk_module/system/etc/ivanna_omega/sofa: 39 en sofa/ + 200
//      en sofa/ari/). La afirmación heredada era "239/239 legible";
//      aquí se mide cuántos parsean como MEDIDOS (Data.IR leído de
//      verdad) y cuántos caen al fallback PCA (load()==true en ambos
//      casos por diseño fail-closed: "legible" ≠ "medido").
//   2) Artefacto de fase del vecino más cercano sin interpolación
//      (hrtfAt, SofaHRTFLoader.cpp): la respuesta es constante a
//      trozos — el mismo HRIR para todo un intervalo de azimut y un
//      salto brusco al cruzar el punto medio entre dos posiciones
//      medidas. Se mide el salto (barrido de azimut 0→30° en pasos
//      de 1° sobre MIT KEMAR, 710 posiciones).
//   3) Telemetría: SofaHRTFLoader::loadCounters() distingue en vivo
//      medido / reconstruido / fallido (ver el header). Este test
//      verifica que los contadores cuadran con lo cargado.
//
// La medición EN VIVO en el teléfono (qué camino suena con la sesión
// real) necesita acceso de solo lectura al dispositivo y NO se hace
// aquí: queda anotada en tests/p1p4_golden_rule.md §PENDIENTE.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../SofaHRTFLoader.hpp"

namespace fs = std::filesystem;

namespace {

// Localiza un dir subiendo desde el CWD (ctest corre desde el build).
std::string findDir(const std::string& rel) {
    fs::path cur = fs::current_path();
    for (int i = 0; i < 10 && !cur.empty(); ++i) {
        const fs::path cand = cur / rel;
        if (fs::is_directory(cand)) return cand.string();
        if (cur == cur.parent_path()) break;
        cur = cur.parent_path();
    }
    return {};
}

double rmsOf(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p += static_cast<double>(v) * v;
    return std::sqrt(p / static_cast<double>(std::max<std::size_t>(x.size(), 1)));
}

double rmsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1.0;
    double p = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        p += d * d;
    }
    return std::sqrt(p / static_cast<double>(std::max<std::size_t>(a.size(), 1)));
}

}  // namespace

// ── 1) Tasa de parseo real de los 239 SOFA del módulo ────────────────────
TEST(P1P4Hrtf, ParseRate239ModuleSofa) {
    const std::string dir =
        findDir("magisk_module/system/etc/ivanna_omega/sofa");
    if (dir.empty())
        GTEST_SKIP() << "sin dir de SOFA del módulo (repo no a la vista)";
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".sofa")
            files.push_back(e.path());
    }
    std::printf("[P1P4-HRTF] archivos .sofa en el módulo: %zu\\n", files.size());
    ASSERT_EQ(files.size(), 239u) << "el inventario del módulo ya no es 239";
    Ivanna::SofaHRTFLoader::resetLoadCountersForTest();
    std::size_t measured = 0, reconstructed = 0, failed = 0;
    std::vector<std::string> reconstructedNames;
    std::vector<std::string> measuredNames;
    for (const fs::path& p : files) {
        Ivanna::SofaHRTFLoader l;
        const bool ok = l.load(p.string());
        if (!ok || l.lastStatus() == Ivanna::SofaHRTFLoader::Status::FILE_NOT_FOUND ||
            l.lastStatus() == Ivanna::SofaHRTFLoader::Status::CORRUPT ||
            l.lastStatus() == Ivanna::SofaHRTFLoader::Status::NOT_HDF5) {
            ++failed;
            std::printf("[P1P4-HRTF] FALLO: %s (status %d)\\n", p.filename().string().c_str(),
                        static_cast<int>(l.lastStatus()));
        } else if (l.isMeasured()) {
            ++measured;
            measuredNames.push_back(p.filename().string());
        } else {
            ++reconstructed;
            reconstructedNames.push_back(p.filename().string());
        }
    }
    const auto cnt = Ivanna::SofaHRTFLoader::loadCounters();
    std::printf("[P1P4-HRTF] 239 SOFA del módulo: medidos=%zu (%.1f%%) | "
                "fallback PCA=%zu | fallidos=%zu\\n",
                measured, 100.0 * measured / 239.0, reconstructed, failed);
    for (const std::string& n : reconstructedNames)
        std::printf("[P1P4-HRTF] fallback PCA: %s\\n", n.c_str());
    std::printf("[P1P4-HRTF] contadores loadCounters(): medidos=%zu "
                "reconstruidos=%zu fallidos=%zu\\n",
                cnt.measured, cnt.reconstructed, cnt.failed);
    // Los contadores deben cuadrar con lo medido (telemetría verificada).
    EXPECT_EQ(cnt.measured, measured);
    EXPECT_EQ(cnt.reconstructed, reconstructed);
    EXPECT_EQ(cnt.failed, failed);
    // load() fail-closed: todo archivo existente devuelve true (con
    // fallback PCA si no parsea). Un fallo real es defecto.
    EXPECT_EQ(failed, 0u) << "archivos SOFA que ni cargan con fallback";
    // ANTES del lector v0 (evol-hrtf): medidos=0/239, PCA=239. DESPUÉS:
    // medidos=12/239. Los 12 son HRTF estrictas (Data.IR con N posiciones
    // y SourcePosition con N filas, R=2): los 3 de sala firma (MIT normal,
    // TU-Berlin, demo FreeField) + MIT large + Pulse + 7 CIPIC. Los 227
    // restantes caen al PCA POR DISEÑO, no por falta de lector: 223 son
    // hpir_* (SimpleHeadphoneIR: 10 IR de audífono en UNA posición,
    // SourcePosition de 1 fila — no es HRTF direccional, el requisito
    // sp.rows == ir.rows los excluye a propósito) y 4 son otra convención
    // SOFA sin Data.IR (GeneralSOS, GeneralTF_E, SimpleFreeFieldSOS,
    // UMA_AnnotatedReceiverAudio). Afirmar 239/239 medidos sería falso.
    const std::vector<std::string> expectedMeasured = {
        "MIT_KEMAR_large_pinna.sofa", "MIT_KEMAR_normal_pinna.sofa",
        "Pulse.sofa", "TU-Berlin_QU_KEMAR_anechoic_radius_0.5m.sofa",
        "demo_FreeFieldHRTF_1_IR.sofa", "subject_003.sofa",
        "subject_008.sofa", "subject_009.sofa", "subject_010.sofa",
        "subject_011.sofa", "subject_012.sofa", "subject_165.sofa",
    };
    std::sort(measuredNames.begin(), measuredNames.end());
    EXPECT_EQ(measured, 12u) << "antes del lector v0: 0/239; después: 12/239";
    EXPECT_EQ(measuredNames, expectedMeasured)
        << "el conjunto de SOFA medidos cambió (revisar: ¿nuevo HRTF o regresión?)";
    EXPECT_EQ(reconstructed, 227u);
    for (const std::string& n : measuredNames)
        std::printf("[P1P4-HRTF] MEDIDO: %s\\n", n.c_str());
    // La tasa de MEDIDOS es el dato nuevo: se reporta, no se impone un
    // umbral inventado. Lo que sí se afirma: si algún archivo cae al
    // fallback, su HRTF NO es su medición (es PCA por id de ruta).
    std::printf("[P1P4-HRTF] conclusión: '239/239 legible' es %s; "
                "'239/239 medidos' es %s (medidos %zu/239)\\n",
                failed == 0 ? "CIERTO (load()==true en los 239)"
                            : "FALSO",
                measured == 239 ? "CIERTO" : "FALSO", measured);
}

// ── 1b) El SOFA v0 del módulo se decodifica EXACTO (no solo "medido") ─────
// Referencia: h5py 3.16.0 sobre los archivos del módulo (verificado
// 2026-11-26, /tmp/hrtf_ref.json). MIT KEMAR frontal (az 0, el 0, idx
// 260): L y R IDÉNTICOS — pico 0.441070556640625 en tap 53, energía
// 0.9960648082196712, primera muestra 6.103515625e-05. TU-Berlin frontal
// (idx 180): L pico 0.2878664433956146 en tap 65, energía
// 0.48345795945003833; R pico 0.3014014661312103 en tap 66, energía
// 0.4834578659088852. Si el lector v0 fallara en un byte (chunk, shuffle,
// deflate, offset), estos números no cuadrarían: isMeasured() solo no
// basta, hay que comparar el IR.
TEST(P1P4Hrtf, MeasuredV0SofaMatchesH5py) {
    const std::string dir =
        findDir("magisk_module/system/etc/ivanna_omega/sofa");
    if (dir.empty())
        GTEST_SKIP() << "sin dir de SOFA del módulo (repo no a la vista)";
    auto peakTap = [](const std::vector<float>& x) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < x.size(); ++i)
            if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
        return best;
    };
    auto energy = [](const std::vector<float>& x) {
        double e = 0.0;
        for (float v : x) e += static_cast<double>(v) * v;
        return e;
    };
    // MIT KEMAR normal (v0, 710 pos, 512 taps, 44.1 kHz).
    {
        Ivanna::SofaHRTFLoader l;
        ASSERT_TRUE(l.load((fs::path(dir) / "MIT_KEMAR_normal_pinna.sofa").string()));
        ASSERT_TRUE(l.isMeasured());
        ASSERT_EQ(l.numPositions(), 710u);
        ASSERT_EQ(l.tapsPerEar(), 512u);
        EXPECT_FLOAT_EQ(l.sampleRateHz(), 44100.0f);
        Ivanna::HRTFIR h;
        ASSERT_TRUE(l.hrtfAt(0.0f, 0.0f, h));
        ASSERT_EQ(h.left.size(), 512u);
        ASSERT_EQ(h.right.size(), 512u);
        EXPECT_EQ(peakTap(h.left), 53u);
        EXPECT_EQ(peakTap(h.right), 53u);
        EXPECT_NEAR(std::fabs(h.left[53]), 0.441070556640625, 1e-7);
        EXPECT_NEAR(energy(h.left), 0.9960648082196712, 1e-6);
        EXPECT_NEAR(energy(h.right), 0.9960648082196712, 1e-6);
        EXPECT_NEAR(h.left[0], 6.103515625e-05, 1e-9);
        // Frontal medido: L y R idénticos (ITD 0 e ILD 0 por construcción).
        double maxDiff = 0.0;
        for (std::size_t i = 0; i < 512; ++i)
            maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(h.left[i]) -
                                                  h.right[i]));
        EXPECT_EQ(maxDiff, 0.0) << "el frontal medido de MIT KEMAR es simétrico";
        std::printf("[P1P4-HRTF] MIT v0 frontal: pico %.9f @tap %zu, "
                    "energía %.9f, |L-R| máx %.1e\\n",
                    std::fabs(h.left[53]), peakTap(h.left), energy(h.left), maxDiff);
    }
    // TU-Berlin QU KEMAR (v0, 360 pos, 2048 taps, 44.1 kHz, 2 chunks).
    {
        Ivanna::SofaHRTFLoader l;
        ASSERT_TRUE(l.load((fs::path(dir) / "TU-Berlin_QU_KEMAR_anechoic_radius_0.5m.sofa").string()));
        ASSERT_TRUE(l.isMeasured());
        ASSERT_EQ(l.numPositions(), 360u);
        ASSERT_EQ(l.tapsPerEar(), 2048u);
        EXPECT_FLOAT_EQ(l.sampleRateHz(), 44100.0f);
        Ivanna::HRTFIR h;
        ASSERT_TRUE(l.hrtfAt(0.0f, 0.0f, h));
        ASSERT_EQ(h.left.size(), 2048u);
        EXPECT_EQ(peakTap(h.left), 65u);
        EXPECT_EQ(peakTap(h.right), 66u);
        EXPECT_NEAR(std::fabs(h.left[65]), 0.2878664433956146, 1e-7);
        EXPECT_NEAR(std::fabs(h.right[66]), 0.3014014661312103, 1e-7);
        EXPECT_NEAR(energy(h.left), 0.48345795945003833, 1e-6);
        EXPECT_NEAR(energy(h.right), 0.4834578659088852, 1e-6);
        std::printf("[P1P4-HRTF] TU-Berlin v0 frontal: picos L %.9f @65 / "
                    "R %.9f @66, energías %.9f / %.9f\\n",
                    std::fabs(h.left[65]), std::fabs(h.right[66]), energy(h.left),
                    energy(h.right));
    }
}

// ── 2) Vecino más cercano sin interpolación: escalera con saltos ─────────
TEST(P1P4Hrtf, NearestNeighborStepArtifact) {
    const std::string dir = findDir("app/src/main/assets/ivanna_omega/sofa");
    if (dir.empty()) GTEST_SKIP() << "sin dir de SOFA de assets";
    const std::string path = (fs::path(dir) / "MIT_KEMAR_normal_pinna.sofa").string();
    if (!fs::exists(path)) GTEST_SKIP() << "falta MIT_KEMAR_normal_pinna.sofa";
    Ivanna::SofaHRTFLoader l;
    ASSERT_TRUE(l.load(path));
    ASSERT_TRUE(l.isMeasured());
    ASSERT_EQ(l.numPositions(), 710u);
    // Barrido azimut 0→30° en pasos de 1°, elevación 0.
    std::vector<Ivanna::HRTFIR> h(31);
    for (int a = 0; a <= 30; ++a) ASSERT_TRUE(l.hrtfAt(static_cast<float>(a), 0.0f, h[a]));
    const double hRef = rmsOf(h[0].left);
    std::size_t zeroSteps = 0, jumps = 0;
    double maxJump = 0.0, sumJump = 0.0;
    int maxJumpAz = -1;
    for (int a = 0; a < 30; ++a) {
        const double dL = rmsDiff(h[a].left, h[a + 1].left);
        const double dR = rmsDiff(h[a].right, h[a + 1].right);
        const double d = std::max(dL, dR);
        if (d == 0.0) {
            ++zeroSteps;
        } else {
            ++jumps;
            sumJump += d;
            if (d > maxJump) {
                maxJump = d;
                maxJumpAz = a;
            }
            std::printf("[P1P4-HRTF] salto az %d°→%d°: dRMS=%.4f (%.1f%% del "
                        "RMS del HRIR)\\n",
                        a, a + 1, d, 100.0 * d / hRef);
        }
    }
    std::printf("[P1P4-HRTF] MIT KEMAR az 0→30° paso 1°: %zu pasos idénticos "
                "(mismo vecino), %zu saltos, salto máx %.4f (%.1f%% del HRIR) "
                "en %d°→%d°\\n",
                zeroSteps, jumps, maxJump, 100.0 * maxJump / hRef, maxJumpAz,
                maxJumpAz + 1);
    // La escalera existe: la mayoría de pasos de 1° no cambian nada y
    // unos pocos saltan de golpe (sin interpolación no hay transición).
    EXPECT_GT(zeroSteps, 0u) << "sin pasos idénticos no hay vecino más cercano";
    EXPECT_GT(jumps, 0u) << "sin saltos no hay cambio de vecino en 0→30°";
    EXPECT_GT(maxJump, 0.05 * hRef)
        << "el salto al cambiar de vecino debe ser >5% del RMS del HRIR";
}

// ── 3) Los contadores distinguen medido / reconstruido / fallido ─────────
TEST(P1P4Hrtf, LoadCountersDistinguishPaths) {
    const std::string dir = findDir("app/src/main/assets/ivanna_omega/sofa");
    if (dir.empty()) GTEST_SKIP() << "sin dir de SOFA de assets";
    Ivanna::SofaHRTFLoader::resetLoadCountersForTest();
    {
        Ivanna::SofaHRTFLoader l;
        ASSERT_TRUE(l.load((fs::path(dir) / "MIT_KEMAR_normal_pinna.sofa").string()));
        EXPECT_TRUE(l.isMeasured());
    }
    {
        // Archivo con solo la firma HDF5: no parsea como medido → PCA.
        const fs::path tmp = fs::temp_directory_path() / "p1p4_fake_hdf5.sofa";
        {
            std::vector<unsigned char> bytes(600, 0);
            bytes[0] = 0x89;
            bytes[1] = 'H';
            bytes[2] = 'D';
            bytes[3] = 'F';
            bytes[4] = 0x0D;
            bytes[5] = 0x0A;
            bytes[6] = 0x1A;
            bytes[7] = 0x0A;
            bytes[8] = 2;  // superbloque v2
            FILE* f = std::fopen(tmp.string().c_str(), "wb");
            ASSERT_NE(f, nullptr);
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
        Ivanna::SofaHRTFLoader l;
        ASSERT_TRUE(l.load(tmp.string())) << "el fallback PCA debe devolver true";
        EXPECT_TRUE(l.isReconstructedFromMasterBasis());
        EXPECT_FALSE(l.isMeasured());
        fs::remove(tmp);
    }
    {
        Ivanna::SofaHRTFLoader l;
        EXPECT_FALSE(l.load("/no/existe/este_archivo.sofa"));
    }
    const auto cnt = Ivanna::SofaHRTFLoader::loadCounters();
    std::printf("[P1P4-HRTF] contadores: medidos=%zu reconstruidos=%zu "
                "fallidos=%zu (esperado 1/1/1)\\n",
                cnt.measured, cnt.reconstructed, cnt.failed);
    EXPECT_EQ(cnt.measured, 1u);
    EXPECT_EQ(cnt.reconstructed, 1u);
    EXPECT_EQ(cnt.failed, 1u);
}
