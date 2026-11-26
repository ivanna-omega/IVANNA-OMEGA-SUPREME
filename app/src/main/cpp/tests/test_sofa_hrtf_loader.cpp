/**
 * test_sofa_hrtf_loader.cpp — SofaHRTFLoader: qué hace de verdad y qué NO hace
 *
 * load() solo valida la FIRMA HDF5 (8 bytes) y RECONSTRUYE un HRIR desde la base maestra
 * SOFA-PCA; no lee las mediciones del archivo. Estas pruebas fijan ese comportamiento para que
 * ningún consumidor vuelva a presentarlo como "medido":
 *   1. archivo inexistente / demasiado pequeño / sin firma HDF5 -> error explícito y sin HRIR.
 *   2. firma válida -> true con Status::VALID_NOT_PARSED e isReconstructedFromMasterBasis().
 *   3. el HRIR es finito, no nulo y distinto por oído (no mono ni silencio).
 *   4. el contenido del archivo NO influye; la RUTA sí (elige el sujeto entrenado).
 */
#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "SofaHRTFLoader.hpp"
#include "spatial/SofaSafRirMasterKnowledge.hpp"

namespace fs = std::filesystem;
using Ivanna::SofaHRTFLoader;

namespace {
class SofaLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("ivanna_sofa_test_" + std::to_string(::getpid()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    // Escribe un archivo; si hdf5 es true empieza con la firma HDF5 completa.
    std::string write(const std::string& sub, const std::string& name, size_t size,
                      bool hdf5, uint8_t fill) {
        const fs::path d = dir_ / sub;
        fs::create_directories(d);
        const fs::path p = d / name;
        std::vector<uint8_t> buf(size, fill);
        if (hdf5 && size >= 8) {
            const uint8_t sig[8] = {0x89, 'H', 'D', 'F', 0x0D, 0x0A, 0x1A, 0x0A};
            for (int i = 0; i < 8; ++i) buf[i] = sig[i];
        }
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
        return p.string();
    }
    fs::path dir_;
};
} // namespace

TEST_F(SofaLoaderTest, MissingFileFailsExplicitlyWithoutHrir) {
    SofaHRTFLoader l;
    EXPECT_FALSE(l.load((dir_ / "no_existe.sofa").string()));
    EXPECT_EQ(l.lastStatus(), SofaHRTFLoader::Status::FILE_NOT_FOUND);
    EXPECT_FALSE(l.isReconstructedFromMasterBasis());
    EXPECT_TRUE(l.hrtf().left.empty());
    EXPECT_TRUE(l.hrtf().right.empty());
}

TEST_F(SofaLoaderTest, TruncatedFileIsCorruptNotSilentlyAccepted) {
    SofaHRTFLoader l;
    EXPECT_FALSE(l.load(write("a", "trunc.sofa", 100, /*hdf5=*/true, 0x11)));
    EXPECT_EQ(l.lastStatus(), SofaHRTFLoader::Status::CORRUPT);
    EXPECT_TRUE(l.hrtf().left.empty());
}

TEST_F(SofaLoaderTest, FileWithoutHdf5SignatureIsRejected) {
    SofaHRTFLoader l;
    EXPECT_FALSE(l.load(write("a", "fake.sofa", 4096, /*hdf5=*/false, 'A')));
    EXPECT_EQ(l.lastStatus(), SofaHRTFLoader::Status::NOT_HDF5);
    EXPECT_FALSE(l.isReconstructedFromMasterBasis());
    EXPECT_TRUE(l.hrtf().left.empty());
}

TEST_F(SofaLoaderTest, SignatureOnlyAcceptsAsReconstructedNeverAsMeasured) {
    SofaHRTFLoader l;
    ASSERT_TRUE(l.load(write("a", "subject.sofa", 2048, /*hdf5=*/true, 0x00)));
    // v2.5.472 (lector D): el archivo sintético tiene el byte de versión
    // del superbloque en 0 (v0) -> UNSUPPORTED, no VALID_NOT_PARSED. En
    // ambos casos el HRIR es RECONSTRUIDO desde la base PCA, nunca medido.
    EXPECT_EQ(l.lastStatus(), SofaHRTFLoader::Status::UNSUPPORTED);
    EXPECT_TRUE(l.isReconstructedFromMasterBasis());   // contrato: NO es una medición del archivo
    EXPECT_FALSE(l.isMeasured());
}

// La base PCA es MONOAURAL (evol-hrtf 2026-11-26, medido): p0 tiene
// E_R≈0 y cada PC lleva ~99.6% de su energía en L. El par frontal
// reconstruido es SIMÉTRICO (izquierdo == derecho): el frontal medido
// (MIT KEMAR a 0°) es L≡R por construcción, y las diferencias
// interaurales las crean la Ley de Curvatura (ITD) y el ILD en el
// render, no la reconstrucción. Antes este test exigía L!=R, lo que
// codificaba el defecto (el residuo R, 0.4% de energía, amplificado
// 23 dB por la normalización, daba ITD −1083 µs a 0°).
TEST_F(SofaLoaderTest, ReconstructedHrirIsFiniteNonZeroAndSymmetricFrontal) {
    SofaHRTFLoader l;
    ASSERT_TRUE(l.load(write("a", "subject.sofa", 2048, true, 0x00)));
    const auto& h = l.hrtf();
    ASSERT_EQ((int)h.left.size(), ivanna::master::kMasterHrirLen);
    ASSERT_EQ((int)h.right.size(), ivanna::master::kMasterHrirLen);
    EXPECT_FLOAT_EQ(h.sampleRate, 48000.0f);
    double eL = 0.0, eR = 0.0;
    for (int i = 0; i < ivanna::master::kMasterHrirLen; ++i) {
        ASSERT_TRUE(std::isfinite(h.left[i]) && std::isfinite(h.right[i])) << "tap " << i;
        eL += (double)h.left[i] * h.left[i];
        eR += (double)h.right[i] * h.right[i];
        EXPECT_FLOAT_EQ(h.left[i], h.right[i])
            << "el par frontal reconstruido es simétrico (tap " << i << ")";
    }
    EXPECT_GT(eL, 1.0e-12);        // no es el filtro nulo
    EXPECT_GT(eR, 1.0e-12);
}

TEST_F(SofaLoaderTest, ContentDoesNotMatterButPathSelectsTheSubject) {
    SofaHRTFLoader a, b, c, d;
    // Mismo nombre sin id de sujeto, contenido totalmente distinto -> HRIR idéntico.
    ASSERT_TRUE(a.load(write("x", "medida.sofa", 2048, true, 0x00)));
    ASSERT_TRUE(b.load(write("y", "medida.sofa", 5000, true, 0xFF)));
    EXPECT_EQ(a.hrtf().left, b.hrtf().left);
    EXPECT_EQ(a.hrtf().right, b.hrtf().right);
    // La RUTA con el id de un sujeto entrenado cambia el resultado.
    ASSERT_TRUE(c.load(write("z", "cipic_003.sofa", 2048, true, 0x00)));
    ASSERT_TRUE(d.load(write("w", "kemar.sofa", 2048, true, 0x00)));
    EXPECT_NE(c.hrtf().left, d.hrtf().left);
}

// ── Selección de sujeto: coincidencia MÁS LARGA, no la primera ──────────────────────────────
// La tabla de anclas lista "kemar" antes que "kemar_large" y "tu_berlin_kemar", y la ruta se
// busca por subcadena: con "primera coincidencia" esos dos sujetos recibían el ancla de KEMAR
// estándar y la elección del usuario no tenía efecto.
TEST_F(SofaLoaderTest, LongestSubjectIdWinsOverShorterSubstring) {
    SofaHRTFLoader kemar, large, berlin;
    ASSERT_TRUE(kemar.load(write("a", "kemar.sofa", 2048, true, 0x00)));
    ASSERT_TRUE(large.load(write("b", "kemar_large.sofa", 2048, true, 0x00)));
    ASSERT_TRUE(berlin.load(write("c", "tu_berlin_kemar.sofa", 2048, true, 0x00)));
    EXPECT_NE(kemar.hrtf().left, large.hrtf().left)
        << "kemar_large no debe resolverse al ancla de kemar";
    EXPECT_NE(kemar.hrtf().left, berlin.hrtf().left)
        << "tu_berlin_kemar no debe resolverse al ancla de kemar";
    EXPECT_NE(large.hrtf().left, berlin.hrtf().left);
}

// ── loadCached: un acierto de caché no puede devolver éxito con el HRIR vacío ───────────────
// La caché (ruta + ok) es GLOBAL, pero hrtf() es de cada instancia. Antes, una segunda instancia
// que pedía la misma ruta recibía true sin haber cargado nada y leía un HRIR vacío.
TEST_F(SofaLoaderTest, LoadCachedHitOnAnotherInstanceStillPopulatesItsOwnHrir) {
    const std::string p = write("d", "cipic_008.sofa", 2048, true, 0x00);
    SofaHRTFLoader first, second;
    ASSERT_TRUE(first.loadCached(p));
    ASSERT_FALSE(first.hrtf().left.empty());
    ASSERT_TRUE(second.loadCached(p));
    EXPECT_FALSE(second.hrtf().left.empty())
        << "loadCached devolvió true pero esta instancia no tiene HRIR";
    EXPECT_EQ(second.hrtf().left.size(), second.hrtf().right.size());
    EXPECT_EQ(first.hrtf().left, second.hrtf().left);
    EXPECT_TRUE(second.isReconstructedFromMasterBasis());
}

TEST_F(SofaLoaderTest, LoadCachedSameInstanceSecondCallIsStillValid) {
    const std::string p = write("e", "kemar.sofa", 2048, true, 0x00);
    SofaHRTFLoader l;
    ASSERT_TRUE(l.loadCached(p));
    const auto left = l.hrtf().left;
    ASSERT_TRUE(l.loadCached(p));
    EXPECT_EQ(l.hrtf().left, left);
}

// Concurrencia: las estáticas de la caché se compartían sin exclusión mutua (carrera de datos
// sobre un std::string). Con ASan/TSan o sin ellos, esto no debe corromper ni caerse.
TEST_F(SofaLoaderTest, LoadCachedFromManyThreadsIsSafe) {
    const std::string p1 = write("f", "kemar.sofa", 2048, true, 0x00);
    const std::string p2 = write("g", "cipic_165.sofa", 2048, true, 0x00);
    std::vector<std::thread> ts;
    std::atomic<int> bad{0};
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&, t] {
            SofaHRTFLoader l;
            for (int i = 0; i < 200; ++i) {
                if (!l.loadCached((i + t) % 2 ? p1 : p2) || l.hrtf().left.empty()) ++bad;
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(bad.load(), 0);
}

// ── Ruta MEDIDA (v2.5.472 Fase 2 ítem D): Data.IR se lee DE VERDAD ──────────
// Los SOFA del catálogo se parsean con el lector HDF5 mínimo: el estado es
// PARSED_MEASURED, isMeasured() es true y hrtfAt() selecciona por azimut /
// elevación desde las mediciones del archivo (no desde la base PCA). Estos
// tests usan los archivos REALES de app/src/main/assets/ivanna_omega/sofa/;
// si el dir no se encuentra (entorno sin assets) se saltan, no fallan.
namespace {

// Localiza el dir de SOFA: env IVANNA_SOFA_ASSET_DIR, o caminando hacia
// arriba desde el CWD buscando app/src/main/assets/ivanna_omega/sofa
// (ctest corre desde build-tests: 7 niveles hasta la raíz del repo).
std::string sofaAssetDir() {
    if (const char* e = std::getenv("IVANNA_SOFA_ASSET_DIR")) {
        if (fs::is_directory(e)) return e;
    }
    fs::path cur = fs::current_path();
    for (int i = 0; i < 10 && !cur.empty(); ++i) {
        const fs::path cand = cur / "app" / "src" / "main" / "assets" / "ivanna_omega" / "sofa";
        if (fs::is_directory(cand)) return cand.string();
        if (cur == cur.parent_path()) break;
        cur = cur.parent_path();
    }
    return {};
}

struct CatalogSofa {
    const char* file;
    size_t positions;
    size_t taps;
    float sampleRateHz;
    float radiusM;
};

constexpr CatalogSofa kCatalog[] = {
    {"TU-Berlin_QU_KEMAR_anechoic_radius_0.5m.sofa", 360, 2048, 44100.0f, 0.5f},
    {"MIT_KEMAR_normal_pinna.sofa", 710, 512, 44100.0f, 1.4f},
    {"demo_FreeFieldHRTF_1_IR.sofa", 2354, 128, 48000.0f, 3.25f},
};

} // namespace

TEST(SofaMeasuredTest, CatalogFilesParseAsMeasured) {
    const std::string dir = sofaAssetDir();
    if (dir.empty()) GTEST_SKIP() << "sin dir de SOFA (IVANNA_SOFA_ASSET_DIR ni repo a la vista)";
    for (const CatalogSofa& c : kCatalog) {
        const std::string path = (fs::path(dir) / c.file).string();
        ASSERT_TRUE(fs::exists(path)) << "falta el SOFA del catálogo: " << path;
        SofaHRTFLoader l;
        ASSERT_TRUE(l.load(path)) << c.file;
        EXPECT_EQ(l.lastStatus(), SofaHRTFLoader::Status::PARSED_MEASURED) << c.file;
        EXPECT_TRUE(l.isMeasured()) << c.file;
        EXPECT_FALSE(l.isReconstructedFromMasterBasis()) << c.file;
        EXPECT_EQ(l.numPositions(), c.positions) << c.file;
        EXPECT_EQ(l.tapsPerEar(), c.taps) << c.file;
        EXPECT_FLOAT_EQ(l.sampleRateHz(), c.sampleRateHz) << c.file;
        EXPECT_EQ(l.path(), path);
    }
}

TEST(SofaMeasuredTest, HrtfAtVariesWithAzimuthAndWraps) {
    const std::string dir = sofaAssetDir();
    if (dir.empty()) GTEST_SKIP() << "sin dir de SOFA";
    SofaHRTFLoader l;
    ASSERT_TRUE(l.load((fs::path(dir) / "MIT_KEMAR_normal_pinna.sofa").string()));
    ASSERT_TRUE(l.isMeasured());
    Ivanna::HRTFIR front, side, back, wrapped;
    ASSERT_TRUE(l.hrtfAt(0.0f, 0.0f, front));
    ASSERT_TRUE(l.hrtfAt(90.0f, 0.0f, side));
    ASSERT_TRUE(l.hrtfAt(180.0f, 0.0f, back));
    ASSERT_TRUE(l.hrtfAt(360.0f, 0.0f, wrapped));
    EXPECT_EQ(front.left.size(), l.tapsPerEar());
    EXPECT_EQ(front.right.size(), l.tapsPerEar());
    EXPECT_FLOAT_EQ(front.sampleRate, 44100.0f);
    // Azimut 0 y 90 son mediciones DISTINTAS (no el mismo par PCA).
    EXPECT_NE(front.left, side.left) << "az 0 y az 90 devuelven el mismo HRIR";
    EXPECT_NE(front.left, back.left) << "az 0 y az 180 devuelven el mismo HRIR";
    EXPECT_NE(side.left, back.left) << "az 90 y az 180 devuelven el mismo HRIR";
    // 360 envuelve a 0: misma posición más cercana.
    EXPECT_EQ(front.left, wrapped.left);
    EXPECT_EQ(front.right, wrapped.right);
    // Frontal (az 0): la cabeza KEMAR es simétrica -> L y R son IDÉNTICOS
    // (bit-exactos). La diferencia interaural real está a az 90 (ILD).
    EXPECT_EQ(front.left, front.right);
    EXPECT_NE(side.left, side.right) << "az 90: L y R deben diferir (ILD)";
    double eL = 0.0, eR = 0.0;
    for (size_t i = 0; i < front.left.size(); ++i) {
        ASSERT_TRUE(std::isfinite(front.left[i]) && std::isfinite(front.right[i]));
        eL += (double)front.left[i] * front.left[i];
        eR += (double)front.right[i] * front.right[i];
    }
    EXPECT_GT(eL, 1.0e-6);
    EXPECT_GT(eR, 1.0e-6);
}

TEST(SofaMeasuredTest, HrtfAtRejectsNonFinitePoseWithoutTouchingOut) {
    const std::string dir = sofaAssetDir();
    if (dir.empty()) GTEST_SKIP() << "sin dir de SOFA";
    SofaHRTFLoader l;
    ASSERT_TRUE(l.load((fs::path(dir) / "MIT_KEMAR_normal_pinna.sofa").string()));
    Ivanna::HRTFIR out;
    out.left = {1.0f, 2.0f};
    out.right = {3.0f, 4.0f};
    EXPECT_FALSE(l.hrtfAt(std::nan(""), 0.0f, out));
    EXPECT_FALSE(l.hrtfAt(0.0f, std::nan(""), out));
    EXPECT_EQ(out.left, (std::vector<float>{1.0f, 2.0f}));
    EXPECT_EQ(out.right, (std::vector<float>{3.0f, 4.0f}));
    // Sin dataset medido, hrtfAt falla (loader reconstruido: solo firma HDF5).
    const fs::path fake = fs::temp_directory_path() / ("ivanna_sofa_fake_" + std::to_string(::getpid()) + ".sofa");
    {
        std::vector<uint8_t> buf(2048, 0x00);
        const uint8_t sig[8] = {0x89, 'H', 'D', 'F', 0x0D, 0x0A, 0x1A, 0x0A};
        for (int i = 0; i < 8; ++i) buf[i] = sig[i];
        std::ofstream f(fake, std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
    }
    SofaHRTFLoader pca;
    ASSERT_TRUE(pca.load(fake.string()));
    ASSERT_FALSE(pca.isMeasured());
    Ivanna::HRTFIR none;
    EXPECT_FALSE(pca.hrtfAt(0.0f, 0.0f, none));
    EXPECT_TRUE(none.left.empty());
    fs::remove(fake);
}

TEST(SofaMeasuredTest, SourcePositionsMatchCatalogRadius) {
    const std::string dir = sofaAssetDir();
    if (dir.empty()) GTEST_SKIP() << "sin dir de SOFA";
    for (const CatalogSofa& c : kCatalog) {
        SofaHRTFLoader l;
        ASSERT_TRUE(l.load((fs::path(dir) / c.file).string()));
        float az = 0.0f, el = 0.0f, r = 0.0f;
        ASSERT_TRUE(l.sourcePosition(0, az, el, r)) << c.file;
        EXPECT_TRUE(std::isfinite(az) && std::isfinite(el));
        EXPECT_FLOAT_EQ(r, c.radiusM) << c.file;
        // Todas las posiciones del catálogo están al mismo radio.
        for (size_t i = 0; i < l.numPositions(); ++i) {
            ASSERT_TRUE(l.sourcePosition(i, az, el, r)) << c.file << " pos " << i;
            EXPECT_FLOAT_EQ(r, c.radiusM) << c.file << " pos " << i;
        }
        EXPECT_FALSE(l.sourcePosition(l.numPositions(), az, el, r));
    }
}

TEST(SofaMeasuredTest, LoadCachedMeasuredStaysMeasured) {
    const std::string dir = sofaAssetDir();
    if (dir.empty()) GTEST_SKIP() << "sin dir de SOFA";
    const std::string p = (fs::path(dir) / "MIT_KEMAR_normal_pinna.sofa").string();
    SofaHRTFLoader l;
    ASSERT_TRUE(l.loadCached(p));
    EXPECT_TRUE(l.isMeasured());
    const auto left = l.hrtf().left;
    ASSERT_FALSE(left.empty());
    ASSERT_TRUE(l.loadCached(p));
    EXPECT_TRUE(l.isMeasured());
    EXPECT_EQ(l.hrtf().left, left);
    EXPECT_EQ(l.numPositions(), 710u);
}
