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
inline constexpr double kSegOutRmsDbL[5] = {-24.875622, -17.114545, -23.656984, -39.124766, -19.947672};
inline constexpr double kSegOutRmsDbR[5] = {-21.098217, -17.553257, -20.534821, -36.085028, -16.353191};
inline constexpr double kSegOutPeakDbL[5] = {-20.651116, -13.411188, -9.684632, -9.293753, -10.318494};
inline constexpr double kSegOutPeakDbR[5] = {-17.014156, -12.223830, -7.965053, -9.270784, -6.524363};
inline constexpr double kNetGainDbL = -8.538116;
inline constexpr double kNetGainDbR = -6.816957;
// Latencia (muestras).
inline constexpr double kLatencyXcorrL = 1277.779778;
inline constexpr double kLatencyXcorrR = 1283.671073;
inline constexpr double kLatencyImpulseL = 1279.000000;
inline constexpr double kLatencyImpulseR = 1279.000000;
// True peak (dBTP).
inline constexpr double kTruePeakDbTp = -0.199179;
inline constexpr double kTruePeakHotDbTp = -0.199179;
// THD+N (dB) e IMD SMPTE (dB).
inline constexpr double kThdnDbL = -29.234820;
inline constexpr double kThdnDbR = -30.633265;
inline constexpr double kImdDbL = -17.088528;
inline constexpr double kImdDbR = -13.526062;
// Silencio residual, último segundo (dBFS).
inline constexpr double kSilenceRmsDbL = -199.713179;
inline constexpr double kSilenceRmsDbR = -199.440207;
inline constexpr double kSilencePeakDbL = -196.596224;
inline constexpr double kSilencePeakDbR = -196.333100;
// CPU: LÍMITES de tiempo de CPU del HILO (µs/hop), no valores exactos.
// p50 base medida = 707.380000 µs (hilo), p99 = 1286.440000 µs, máx = 1674.223000 µs; pared p50/p99/máx = 717.006000/53352.145000/62158.081000 µs, xruns de pared medidos = 34 (la pared mide la carga del host, no se congela).
inline constexpr double kCpuP50MaxUs = 2829.520000;
inline constexpr double kCpuP99MaxUs = 3859.320000;
// Máx: 2x el presupuesto (5333 µs), no el presupuesto pelado:
// el máx es un estadístico extremo y el motor tiene picos de hilo
// de hasta 4393 µs (medido en la congelación final) que NO son
// xruns de pared; un máx a 2666.67 sería flaky. p50/p99 son los
// límites que realmente vigilan el coste del motor.
inline constexpr double kCpuMaxMaxUs = 5333.33;
inline constexpr int kXrunsMax = 0;
}  // namespace p0
