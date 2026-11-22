// test_telemetry.cpp — Telemetría del control cerrado (plan del salto,
// Luna, sección 3 punto 14).
//
// Verifica el MECANISMO (registro, orden FIFO, fallos inmediatos,
// marcado de no-finitos, política de buffer lleno, productores
// concurrentes), no valores medidos: no se fabrica ninguna métrica.
// La telemetría solo registra; ningún test aquí decide nada.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../fused/closed_loop/telemetry.hpp"

using ivanna::closed_loop::FailureRecord;
using ivanna::closed_loop::Telemetry;
using ivanna::closed_loop::TelemetryFrame;

namespace {

TelemetryFrame makeFrame(uint64_t sampleTime, uint64_t generation = 1) {
  TelemetryFrame f{};
  f.sampleTime = sampleTime;
  f.generation = generation;
  f.confidence = 0.70f;  // calibración estimada actual (no medida)
  for (int i = 0; i < 7; ++i) {
    f.chosenAction[i] = 0.1f * static_cast<float>(i + 1);
    f.appliedAction[i] = 0.05f * static_cast<float>(i + 1);
  }
  f.energyBeforeDb = -14.0f;
  f.energyAfterDb = -14.2f;
  f.truePeakDbTp = -1.5f;
  f.thdEstimateDb = -94.0f;
  f.latencyMs = 2.667f;
  f.xruns = 0;
  f.twinDivergence = 0.01f;
  f.valid = true;
  return f;
}

void expectFrameEqual(const TelemetryFrame& got, const TelemetryFrame& want) {
  EXPECT_EQ(got.sampleTime, want.sampleTime);
  EXPECT_EQ(got.generation, want.generation);
  EXPECT_FLOAT_EQ(got.confidence, want.confidence);
  for (int i = 0; i < 7; ++i) {
    EXPECT_FLOAT_EQ(got.chosenAction[i], want.chosenAction[i]);
    EXPECT_FLOAT_EQ(got.appliedAction[i], want.appliedAction[i]);
  }
  EXPECT_FLOAT_EQ(got.energyBeforeDb, want.energyBeforeDb);
  EXPECT_FLOAT_EQ(got.energyAfterDb, want.energyAfterDb);
  EXPECT_FLOAT_EQ(got.truePeakDbTp, want.truePeakDbTp);
  EXPECT_FLOAT_EQ(got.thdEstimateDb, want.thdEstimateDb);
  EXPECT_FLOAT_EQ(got.latencyMs, want.latencyMs);
  EXPECT_EQ(got.xruns, want.xruns);
  EXPECT_FLOAT_EQ(got.twinDivergence, want.twinDivergence);
  EXPECT_EQ(got.valid, want.valid);
}

std::vector<TelemetryFrame> drainAll(Telemetry& t) {
  std::vector<TelemetryFrame> out;
  std::vector<TelemetryFrame> chunk(64);
  while (true) {
    const std::size_t n = t.drain(chunk.data(), chunk.size());
    if (n == 0) break;
    out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<long>(n));
  }
  return out;
}

}  // namespace

// (1) record() + drain() devuelven el frame intacto.
TEST(Telemetry, RecordDrainRoundtrip) {
  Telemetry t;
  EXPECT_EQ(t.pending(), 0u);
  EXPECT_EQ(t.drain(nullptr, 0), 0u);
  std::vector<TelemetryFrame> out(1);
  EXPECT_EQ(t.drain(out.data(), out.size()), 0u);  // vacío

  const TelemetryFrame want = makeFrame(48000, 7);
  t.record(want);
  EXPECT_EQ(t.pending(), 1u);
  EXPECT_EQ(t.dropped(), 0u);

  ASSERT_EQ(t.drain(out.data(), out.size()), 1u);
  expectFrameEqual(out[0], want);
  EXPECT_EQ(t.pending(), 0u);
}

// (2) Varios frames en orden FIFO.
TEST(Telemetry, FifoOrder) {
  Telemetry t;
  std::vector<TelemetryFrame> want;
  for (uint64_t i = 0; i < 5; ++i) {
    want.push_back(makeFrame(1000 * (i + 1), i + 1));
    t.record(want.back());
  }
  EXPECT_EQ(t.pending(), 5u);

  const std::vector<TelemetryFrame> got = drainAll(t);
  ASSERT_EQ(got.size(), 5u);
  for (std::size_t i = 0; i < want.size(); ++i) {
    expectFrameEqual(got[i], want[i]);
  }
  EXPECT_EQ(t.pending(), 0u);
}

// drain() respeta el máximo pedido y deja el resto pendiente.
TEST(Telemetry, DrainRespectsMax) {
  Telemetry t;
  for (uint64_t i = 0; i < 5; ++i) t.record(makeFrame(i));
  std::vector<TelemetryFrame> out(2);
  ASSERT_EQ(t.drain(out.data(), out.size()), 2u);
  EXPECT_EQ(out[0].sampleTime, 0u);
  EXPECT_EQ(out[1].sampleTime, 1u);
  EXPECT_EQ(t.pending(), 3u);
  const std::vector<TelemetryFrame> rest = drainAll(t);
  ASSERT_EQ(rest.size(), 3u);
  EXPECT_EQ(rest[0].sampleTime, 2u);
  EXPECT_EQ(rest[2].sampleTime, 4u);
}

// (3) recordFailure() queda disponible de inmediato: no espera al
// drenado de los frames de 10 Hz, y usa un canal independiente.
TEST(Telemetry, FailureAvailableImmediately) {
  Telemetry t;
  for (uint64_t i = 0; i < 3; ++i) t.record(makeFrame(i));
  EXPECT_EQ(t.pending(), 3u);

  t.recordFailure("xruns spike");
  EXPECT_EQ(t.pendingFailures(), 1u);

  std::vector<FailureRecord> failures(4);
  ASSERT_EQ(t.drainFailures(failures.data(), failures.size()), 1u);
  EXPECT_STREQ(failures[0].reason, "xruns spike");
  EXPECT_EQ(failures[0].ordinal, 0u);
  EXPECT_EQ(t.pendingFailures(), 0u);
  // Los frames siguen pendientes: el fallo no los drenó ni los tocó.
  EXPECT_EQ(t.pending(), 3u);

  // Segundo fallo: ordinal monótono; motivo nulo -> "unknown".
  t.recordFailure(nullptr);
  ASSERT_EQ(t.drainFailures(failures.data(), failures.size()), 1u);
  EXPECT_STREQ(failures[0].reason, "unknown");
  EXPECT_EQ(failures[0].ordinal, 1u);
}

// Un motivo más largo que el buffer se trunca sin perder el NUL.
TEST(Telemetry, FailureReasonTruncated) {
  Telemetry t;
  const std::string longReason(200, 'x');
  t.recordFailure(longReason.c_str());
  std::vector<FailureRecord> failures(1);
  ASSERT_EQ(t.drainFailures(failures.data(), failures.size()), 1u);
  EXPECT_EQ(std::strlen(failures[0].reason), 63u);
  EXPECT_EQ(failures[0].reason[63], '\0');
}

// (4) Frame con NaN (o Inf) se registra como inválido, no se descarta.
TEST(Telemetry, NonFiniteFrameMarkedInvalid) {
  Telemetry t;
  TelemetryFrame nanThd = makeFrame(1);
  nanThd.thdEstimateDb = std::nan("");
  t.record(nanThd);

  TelemetryFrame nanAction = makeFrame(2);
  nanAction.chosenAction[3] = std::nan("");
  t.record(nanAction);

  TelemetryFrame infLatency = makeFrame(3);
  infLatency.latencyMs = HUGE_VALF;
  t.record(infLatency);

  TelemetryFrame validFrame = makeFrame(4);
  t.record(validFrame);

  EXPECT_EQ(t.pending(), 4u);  // nada descartado en silencio
  const std::vector<TelemetryFrame> got = drainAll(t);
  ASSERT_EQ(got.size(), 4u);
  EXPECT_FALSE(got[0].valid);
  EXPECT_TRUE(std::isnan(got[0].thdEstimateDb));  // el valor queda visible
  EXPECT_EQ(got[0].sampleTime, 1u);
  EXPECT_FALSE(got[1].valid);
  EXPECT_FALSE(got[2].valid);
  EXPECT_TRUE(got[3].valid);  // el frame sano sigue válido
}

// Un frame que ya venía con valid=false se conserva como inválido.
TEST(Telemetry, InvalidInputStaysInvalid) {
  Telemetry t;
  TelemetryFrame f = makeFrame(9);
  f.valid = false;
  t.record(f);
  const std::vector<TelemetryFrame> got = drainAll(t);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_FALSE(got[0].valid);
}

// (5) Buffer lleno: política documentada = sobrescribe el MÁS VIEJO y
// conserva el más reciente. Al llenar capacidad+10, los 10 primeros se
// pierden (contados en dropped()) y el último frame sigue presente.
TEST(Telemetry, FullBufferKeepsNewest) {
  Telemetry t;
  const std::size_t capacity = Telemetry::kCapacity;
  const std::size_t total = capacity + 10;
  for (std::size_t i = 0; i < total; ++i) {
    t.record(makeFrame(i));
  }
  EXPECT_EQ(t.pending(), capacity);
  // La pérdida se contabiliza cuando el consumidor detecta el salto al
  // drenar (contaje exacto, sin carrera con el productor).
  EXPECT_EQ(t.dropped(), 0u);

  const std::vector<TelemetryFrame> got = drainAll(t);
  EXPECT_EQ(t.dropped(), 10u);
  ASSERT_EQ(got.size(), capacity);
  EXPECT_EQ(got.front().sampleTime, 10u);   // los 0..9 se sobrescribieron
  EXPECT_EQ(got.back().sampleTime, total - 1);  // el más reciente intacto
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(got[i].sampleTime, 10u + i);  // FIFO sin huecos ni duplicados
  }
  EXPECT_EQ(t.pending(), 0u);
}

// (6) record() desde dos hilos no rompe la estructura: sin crash ni
// corrupción; todos los frames llegan intactos y sin duplicados.
TEST(Telemetry, ConcurrentProducers) {
  Telemetry t;
  constexpr int kPerThread = 100;
  auto produce = [&t](uint64_t base) {
    for (int i = 0; i < kPerThread; ++i) {
      t.record(makeFrame(base + static_cast<uint64_t>(i)));
    }
  };
  std::thread a(produce, 1000);
  std::thread b(produce, 2000);
  a.join();
  b.join();

  EXPECT_EQ(t.pending(), 2u * kPerThread);
  EXPECT_EQ(t.dropped(), 0u);
  const std::vector<TelemetryFrame> got = drainAll(t);
  ASSERT_EQ(got.size(), 2u * kPerThread);
  std::set<uint64_t> seen;
  for (const TelemetryFrame& f : got) {
    EXPECT_TRUE(f.valid);
    EXPECT_TRUE(f.confidence == 0.70f);
    EXPECT_TRUE(seen.insert(f.sampleTime).second) << "duplicado: " << f.sampleTime;
  }
  for (int i = 0; i < kPerThread; ++i) {
    EXPECT_TRUE(seen.count(1000u + static_cast<uint64_t>(i)) > 0);
    EXPECT_TRUE(seen.count(2000u + static_cast<uint64_t>(i)) > 0);
  }
}
