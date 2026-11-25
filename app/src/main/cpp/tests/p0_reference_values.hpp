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
inline constexpr double kSegOutRmsDbL[5] = {-23.367044, -15.763421, -23.374772, -38.769752, -18.620518};
inline constexpr double kSegOutRmsDbR[5] = {-19.766893, -16.325378, -20.230567, -35.714549, -14.994367};
inline constexpr double kSegOutPeakDbL[5] = {-19.195030, -12.146492, -9.347278, -9.514617, -9.489977};
inline constexpr double kSegOutPeakDbR[5] = {-15.877741, -11.173961, -7.753040, -9.555603, -5.639211};
inline constexpr double kNetGainDbL = -7.429452;
inline constexpr double kNetGainDbR = -5.882737;
// Latencia (muestras).
inline constexpr double kLatencyXcorrL = 1277.778757;
inline constexpr double kLatencyXcorrR = 1283.808562;
inline constexpr double kLatencyImpulseL = 1279.000000;
inline constexpr double kLatencyImpulseR = 1279.000000;
// True peak (dBTP).
inline constexpr double kTruePeakDbTp = -0.174101;
inline constexpr double kTruePeakHotDbTp = -0.174101;
// THD+N (dB) e IMD SMPTE (dB).
inline constexpr double kThdnDbL = -29.578397;
inline constexpr double kThdnDbR = -32.150543;
inline constexpr double kImdDbL = -17.145253;
inline constexpr double kImdDbR = -13.523127;
// Silencio residual, último segundo (dBFS).
inline constexpr double kSilenceRmsDbL = -199.150768;
inline constexpr double kSilenceRmsDbR = -199.088434;
inline constexpr double kSilencePeakDbL = -195.976928;
inline constexpr double kSilencePeakDbR = -195.923022;
// CPU: LÍMITES de tiempo de CPU del HILO (µs/hop), no valores exactos.
// p50 base medida = 1000.177000 µs (hilo), p99 = 2332.793000 µs, máx = 2727.305000 µs; pared p50/p99/máx = 1005.145000/2387.916000/10354.215000 µs, xruns de pared medidos = 5 (la pared mide la carga del host, no se congela).
inline constexpr double kCpuP50MaxUs = 4000.708000;
inline constexpr double kCpuP99MaxUs = 6998.379000;
// Máx: 2x el presupuesto (5333 µs), no el presupuesto pelado:
// el máx es un estadístico extremo y el motor tiene picos de hilo
// de hasta 4393 µs (medido en la congelación final) que NO son
// xruns de pared; un máx a 2666.67 sería flaky. p50/p99 son los
// límites que realmente vigilan el coste del motor.
inline constexpr double kCpuMaxMaxUs = 5333.33;
inline constexpr int kXrunsMax = 0;
}  // namespace p0
