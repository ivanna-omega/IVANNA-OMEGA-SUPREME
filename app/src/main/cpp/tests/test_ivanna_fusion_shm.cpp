// test_ivanna_fusion_shm.cpp — Contrato SHM IvannaFusionState v2 + etapa
// HAL (Fase 2, ítem E / F5, v2.5.472). CÓDIGO SOLAMENTE: nada se instala
// en el teléfono; el SHM de prueba vive en /tmp y se borra al salir.
//
// Qué fija:
//   1. El contrato v2 hace round-trip (incluidos los campos nuevos:
//      dsp_us_avg, xrun_count, daemon_heartbeat_ns, pose compacta,
//      parallax_amount, asrc_bypass) con magic "IVAN" y versión 2.
//      (La consolidación §3.4 cita magic 0x49564653, pero el contrato
//      vivo usa 0x4956414E: no se cambia el magic.)
//   2. El seqlock rechaza la lectura a mitad de escritura (sequence impar).
//   3. La etapa HAL abre solo con open+mmap (sin O_CREAT): sin SHM,
//      passthrough sin crash.
//   4. La etapa publica el bloque músculo (frames_written, sample_rate,
//      dsp_us_avg, xrun_count, muscle_health) y pasa limpio con ASRC en
//      BYPASS 0033.
//   5. El gate engage: solo con cerebro vivo + heartbeat fresco (<100 ms)
//      + veredicto LOADED + asrc_bypass == 0. Cualquiera que falle es
//      passthrough.
//   6. Kill switch del músculo (pal_asrc.off, CON la c) fuerza passthrough.
//   7. Safe-mode tras 3 lecturas malas del SHM, latched (no se recupera
//      en caliente aunque el SHM vuelva).
//   8. Los xruns se cuentan con noteXrun() y se publican.
#include <gtest/gtest.h>

#include "daemon/hal/ivanna_hal_stage.hpp"
#include "include/ivanna_fusion_state.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

uint64_t nowNs() {
    struct timespec ts;
    EXPECT_EQ(::clock_gettime(CLOCK_MONOTONIC, &ts), 0);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// SHM de prueba en /tmp: archivo + mmap compartido (mismo patrón que el
// daemon: open + ftruncate + mmap). RAII: borra el archivo al salir.
struct TestShm {
    std::string path;
    int fd = -1;
    IvannaFusionState* mem = nullptr;

    TestShm() {
        char tmpl[] = "/tmp/ivanna_fusion_test_XXXXXX";
        fd = ::mkstemp(tmpl);
        EXPECT_GE(fd, 0);
        if (fd < 0) return;
        path = tmpl;
        EXPECT_EQ(::ftruncate(fd, static_cast<off_t>(sizeof(IvannaFusionState))), 0);
        void* p = ::mmap(nullptr, sizeof(IvannaFusionState), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        EXPECT_NE(p, MAP_FAILED);
        if (p == MAP_FAILED) return;
        mem = static_cast<IvannaFusionState*>(p);
        IvannaFusionState init{};
        init.magic = IVANNA_FUSION_MAGIC;
        init.version = IVANNA_FUSION_VERSION;
        init.verdict = IVANNA_VERDICT_BYPASS;
        init.ratio = 1.0f;
        init.asrc_bypass = 1;
        init.parallax_amount = 1.0f;
        ivanna_fusion_write(mem, &init);
    }
    ~TestShm() {
        if (mem) ::munmap(mem, sizeof(IvannaFusionState));
        if (fd >= 0) ::close(fd);
        if (!path.empty()) ::unlink(path.c_str());
    }
    void writeBrain(uint32_t verdict, uint64_t heartbeatNs, uint32_t bypass) {
        IvannaFusionState cur{};
        EXPECT_TRUE(ivanna_fusion_read(mem, &cur));
        IvannaFusionState w = cur;
        w.verdict = verdict;
        w.brain_health = 1;
        w.daemon_heartbeat_ns = heartbeatNs;
        w.asrc_bypass = bypass;
        w.ratio = 1.0f;
        ivanna_fusion_write(mem, &w);
    }
};

std::vector<float> makeTone(int frames, float amp = 0.5f) {
    std::vector<float> v(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
        v[static_cast<size_t>(i)] = amp * std::sin(2.0 * 3.14159265 * 1000.0 * i / 48000.0);
    return v;
}

}  // namespace

TEST(IvannaFusionShm, ContractV2RoundTrip) {
    IvannaFusionState w{};
    w.magic = IVANNA_FUSION_MAGIC;
    w.version = IVANNA_FUSION_VERSION;
    w.session_id = 329;
    w.ratio = 1.000123f;
    w.verdict = IVANNA_VERDICT_LOADED;
    w.dsp_us_avg = 123.5f;
    w.xrun_count = 7;
    w.daemon_heartbeat_ns = 123456789ULL;
    w.head_yaw_rad = 0.5f;
    w.head_pitch_rad = 0.2f;
    w.head_roll_rad = -0.1f;
    w.parallax_amount = 0.5f;
    w.asrc_bypass = 1;
    w.frames_written = 48000;
    w.stream_rate_hz = 48000;

    IvannaFusionState mem{};
    ivanna_fusion_write(&mem, &w);
    IvannaFusionState r{};
    ASSERT_TRUE(ivanna_fusion_read(&mem, &r));
    EXPECT_EQ(r.magic, IVANNA_FUSION_MAGIC);
    EXPECT_EQ(r.version, 2u);
    EXPECT_EQ(r.session_id, 329);
    EXPECT_FLOAT_EQ(r.ratio, 1.000123f);
    EXPECT_FLOAT_EQ(r.dsp_us_avg, 123.5f);
    EXPECT_EQ(r.xrun_count, 7u);
    EXPECT_EQ(r.daemon_heartbeat_ns, 123456789ULL);
    EXPECT_FLOAT_EQ(r.head_yaw_rad, 0.5f);
    EXPECT_FLOAT_EQ(r.head_pitch_rad, 0.2f);
    EXPECT_FLOAT_EQ(r.head_roll_rad, -0.1f);
    EXPECT_FLOAT_EQ(r.parallax_amount, 0.5f);
    EXPECT_EQ(r.asrc_bypass, 1u);
    EXPECT_EQ(r.frames_written, 48000u);
    EXPECT_EQ(r.stream_rate_hz, 48000u);
}

TEST(IvannaFusionShm, SeqlockRejectsMidWriteRead) {
    IvannaFusionState mem{};
    mem.magic = IVANNA_FUSION_MAGIC;
    mem.version = IVANNA_FUSION_VERSION;
    // Escritor a mitad: sequence impar.
    __atomic_store_n(&mem.sequence, 1u, __ATOMIC_RELEASE);
    IvannaFusionState out{};
    EXPECT_FALSE(ivanna_fusion_read(&mem, &out));
    uint32_t seq = 0;
    EXPECT_FALSE(ivanna_fusion_read_seq(&mem, &out, &seq));
    // Escritor termina: sequence par, lectura válida con su sequence.
    __atomic_store_n(&mem.sequence, 2u, __ATOMIC_RELEASE);
    EXPECT_TRUE(ivanna_fusion_read_seq(&mem, &out, &seq));
    EXPECT_EQ(seq, 2u);
}

TEST(IvannaHalStage, OpenMissingPathFailsClosedToPassthrough) {
    ivanna::hal::IvannaHalStage stage;
    EXPECT_FALSE(stage.open("/tmp/no_existe_ivanna_fusion_state"));
    EXPECT_FALSE(stage.isShmOpen());
    auto l = makeTone(128);
    auto r = makeTone(128);
    const auto l0 = l, r0 = r;
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_TRUE(stage.isPassthrough());
    EXPECT_EQ(l, l0);  // bit-exacto: sin SHM no se toca el audio
    EXPECT_EQ(r, r0);
}

TEST(IvannaHalStage, NoShmProcessDoesNotCrash) {
    ivanna::hal::IvannaHalStage stage;  // sin open(): passthrough puro
    auto l = makeTone(128);
    auto r = makeTone(128);
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_TRUE(stage.isPassthrough());
    EXPECT_FALSE(stage.isSafeMode());
}

TEST(IvannaHalStage, PublishesMuscleBlockAndPassesThroughOnBypass) {
    TestShm shm;
    ASSERT_NE(shm.mem, nullptr);
    ivanna::hal::IvannaHalStage stage;
    ASSERT_TRUE(stage.open(shm.path.c_str(), "/tmp/no_existe_kill_switch"));
    auto l = makeTone(128);
    auto r = makeTone(128);
    const auto l0 = l, r0 = r;
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_EQ(l, l0);  // ASRC BYPASS 0033: bit-exacto
    EXPECT_EQ(r, r0);
    EXPECT_TRUE(stage.isPassthrough());
    EXPECT_GT(stage.framesWritten(), 0u);
    IvannaFusionState cur{};
    ASSERT_TRUE(ivanna_fusion_read(shm.mem, &cur));
    EXPECT_EQ(cur.muscle_health, 1u);
    EXPECT_EQ(cur.stream_rate_hz, 48000u);
    EXPECT_EQ(cur.stream_channels, 2u);
    EXPECT_GT(cur.frames_written, 0u);
    EXPECT_GT(cur.frames_passthrough, 0u);
    EXPECT_GE(cur.dsp_us_avg, 0.0f);
    EXPECT_EQ(cur.asrc_bypass, 1u);  // el cerebro lo publicó en bypass
}

TEST(IvannaHalStage, EngageGateNeedsFreshBrainLoadedAndAsrcEnabled) {
    TestShm shm;
    ASSERT_NE(shm.mem, nullptr);
    ivanna::hal::IvannaHalStage stage;
    ASSERT_TRUE(stage.open(shm.path.c_str(), "/tmp/no_existe_kill_switch"));
    auto l = makeTone(128);
    auto r = makeTone(128);
    // Cerebro vivo + heartbeat fresco + LOADED + ASRC habilitado: engage.
    shm.writeBrain(IVANNA_VERDICT_LOADED, nowNs(), 0);
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_FALSE(stage.isPassthrough());
    // ASRC en bypass: passthrough aunque todo lo demás esté bien.
    shm.writeBrain(IVANNA_VERDICT_LOADED, nowNs(), 1);
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_TRUE(stage.isPassthrough());
    // Heartbeat expirado (>100 ms): passthrough.
    shm.writeBrain(IVANNA_VERDICT_LOADED, nowNs() - 200ULL * 1000000ULL, 0);
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_TRUE(stage.isPassthrough());
    // Veredicto BYPASS: passthrough.
    shm.writeBrain(IVANNA_VERDICT_BYPASS, nowNs(), 0);
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_TRUE(stage.isPassthrough());
}

TEST(IvannaHalStage, KillSwitchForcesPassthrough) {
    TestShm shm;
    ASSERT_NE(shm.mem, nullptr);
    // Kill switch PRESENTE (CON la c, el del músculo).
    char ktmpl[] = "/tmp/ivanna_kill_test_XXXXXX";
    const int kfd = ::mkstemp(ktmpl);
    ASSERT_GE(kfd, 0);
    ::close(kfd);
    shm.writeBrain(IVANNA_VERDICT_LOADED, nowNs(), 0);
    ivanna::hal::IvannaHalStage stage;
    ASSERT_TRUE(stage.open(shm.path.c_str(), ktmpl));
    auto l = makeTone(128);
    auto r = makeTone(128);
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_TRUE(stage.isPassthrough());
    ::unlink(ktmpl);
    // Sin kill switch, otra instancia sí engancha.
    ivanna::hal::IvannaHalStage stage2;
    ASSERT_TRUE(stage2.open(shm.path.c_str(), "/tmp/no_existe_kill_switch"));
    stage2.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_FALSE(stage2.isPassthrough());
}

TEST(IvannaHalStage, SafeModeAfterThreeBadReadsIsLatched) {
    TestShm shm;
    ASSERT_NE(shm.mem, nullptr);
    ivanna::hal::IvannaHalStage stage;
    ASSERT_TRUE(stage.open(shm.path.c_str(), "/tmp/no_existe_kill_switch"));
    auto l = makeTone(128);
    auto r = makeTone(128);
    // Corromper el magic: 3 bloques malos → safe-mode.
    IvannaFusionState cur{};
    ASSERT_TRUE(ivanna_fusion_read(shm.mem, &cur));
    IvannaFusionState bad = cur;
    bad.magic = 0xDEADBEEFu;
    ivanna_fusion_write(shm.mem, &bad);
    for (int i = 0; i < 3; ++i)
        stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_TRUE(stage.isSafeMode());
    EXPECT_TRUE(stage.isPassthrough());
    // Restaurar el SHM: el safe-mode NO se recupera en caliente.
    IvannaFusionState good = bad;
    good.magic = IVANNA_FUSION_MAGIC;
    good.version = IVANNA_FUSION_VERSION;
    ivanna_fusion_write(shm.mem, &good);
    stage.processBlock(l.data(), r.data(), 128, 48000);
    EXPECT_TRUE(stage.isSafeMode());
    EXPECT_TRUE(stage.isPassthrough());
}

TEST(IvannaHalStage, XrunsAreCountedAndPublished) {
    TestShm shm;
    ASSERT_NE(shm.mem, nullptr);
    ivanna::hal::IvannaHalStage stage;
    ASSERT_TRUE(stage.open(shm.path.c_str(), "/tmp/no_existe_kill_switch"));
    auto l = makeTone(128);
    auto r = makeTone(128);
    stage.noteXrun();
    stage.noteXrun();
    EXPECT_EQ(stage.xrunCount(), 2u);
    stage.processBlock(l.data(), r.data(), 128, 48000);
    IvannaFusionState cur{};
    ASSERT_TRUE(ivanna_fusion_read(shm.mem, &cur));
    EXPECT_EQ(cur.xrun_count, 2u);
}
