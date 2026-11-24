#pragma once
// p0_reference_values.hpp — VALORES CONGELADOS P0 (REFERENCE_LOCKED).
// GENERADO por scripts/p0_freeze.sh — NUNCA editar a mano.
// Un cambio intencional del motor exige re-congelar y revisar el
// diff de valores como cambio de producto, no como ruido.
namespace p0 {
inline constexpr bool kFrozen = true;
inline constexpr char kFrozenFrom[] = "CONGELADO post-fix APEX+NEURO (scripts/p0_freeze.sh --final)";
inline constexpr char kReferenceId[] = "p0_reference_v1";
// Niveles: RMS y pico de salida por segmento [sine, imd, noise, clicks, hot] (dBFS).
inline constexpr double kSegOutRmsDbL[5] = {-23.367031, -15.769643, -23.410037, -38.769527, -18.620765};
inline constexpr double kSegOutRmsDbR[5] = {-19.766883, -16.420340, -20.270750, -35.714443, -14.993243};
inline constexpr double kSegOutPeakDbL[5] = {-19.194988, -12.243037, -9.594114, -9.514646, -9.488569};
inline constexpr double kSegOutPeakDbR[5] = {-15.877707, -11.503951, -7.870087, -9.555617, -5.639092};
inline constexpr double kNetGainDbL = -7.443626;
inline constexpr double kNetGainDbR = -5.927507;
// Latencia (muestras).
inline constexpr double kLatencyXcorrL = 1277.778643;
inline constexpr double kLatencyXcorrR = 1283.808919;
inline constexpr double kLatencyImpulseL = 1279.000000;
inline constexpr double kLatencyImpulseR = 1279.000000;
// True peak (dBTP).
inline constexpr double kTruePeakDbTp = -0.174006;
inline constexpr double kTruePeakHotDbTp = -0.174006;
// THD+N (dB) e IMD SMPTE (dB).
inline constexpr double kThdnDbL = -29.578406;
inline constexpr double kThdnDbR = -32.150576;
inline constexpr double kImdDbL = -17.143659;
inline constexpr double kImdDbR = -13.522287;
// Silencio residual, último segundo (dBFS).
inline constexpr double kSilenceRmsDbL = -199.147472;
inline constexpr double kSilenceRmsDbR = -199.116395;
inline constexpr double kSilencePeakDbL = -195.937548;
inline constexpr double kSilencePeakDbR = -195.897371;
// CPU: LÍMITES de tiempo de CPU del HILO (µs/hop), no valores exactos.
// p50 base medida = 770.845000 µs (hilo), p99 = 1535.723000 µs, máx = 3034.740000 µs; pared p50/p99/máx = 771.607000/1548.171000/3035.401000 µs, xruns de pared medidos = 1 (la pared mide la carga del host, no se congela).
inline constexpr double kCpuP50MaxUs = 3083.380000;
inline constexpr double kCpuP99MaxUs = 4607.169000;
// Máx: 2x el presupuesto (5333 µs), no el presupuesto pelado:
// el máx es un estadístico extremo y el motor tiene picos de hilo
// de hasta 4393 µs (medido en la congelación final) que NO son
// xruns de pared; un máx a 2666.67 sería flaky. p50/p99 son los
// límites que realmente vigilan el coste del motor.
inline constexpr double kCpuMaxMaxUs = 5333.33;
inline constexpr int kXrunsMax = 0;
}  // namespace p0
