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
inline constexpr double kSegOutRmsDbL[5] = {-21.445191, -15.278961, -21.367798, -36.881193, -15.367631};
inline constexpr double kSegOutRmsDbR[5] = {-20.908996, -16.554227, -21.381646, -36.847527, -14.819202};
inline constexpr double kSegOutPeakDbL[5] = {-17.332113, -11.660429, -7.279750, -9.520592, -6.174304};
inline constexpr double kSegOutPeakDbR[5] = {-16.862166, -11.451261, -8.824636, -9.546619, -5.559448};
inline constexpr double kNetGainDbL = -6.064533;
inline constexpr double kNetGainDbR = -6.273575;
// Latencia (muestras).
inline constexpr double kLatencyXcorrL = 1277.780088;
inline constexpr double kLatencyXcorrR = 1283.815435;
inline constexpr double kLatencyImpulseL = 1279.000000;
inline constexpr double kLatencyImpulseR = 1279.000000;
// True peak (dBTP).
inline constexpr double kTruePeakDbTp = -0.134600;
inline constexpr double kTruePeakHotDbTp = -0.134600;
// THD+N (dB) e IMD SMPTE (dB).
inline constexpr double kThdnDbL = -30.324404;
inline constexpr double kThdnDbR = -30.761292;
inline constexpr double kImdDbL = -15.969072;
inline constexpr double kImdDbR = -13.695352;
// Silencio residual, último segundo (dBFS).
inline constexpr double kSilenceRmsDbL = -196.948084;
inline constexpr double kSilenceRmsDbR = -199.956124;
inline constexpr double kSilencePeakDbL = -193.780272;
inline constexpr double kSilencePeakDbR = -196.779813;
// CPU: LÍMITES de tiempo de CPU del HILO (µs/hop), no valores exactos.
// p50 base medida = 771.857000 µs (hilo), p99 = 1418.056000 µs, máx = 2190.404000 µs; pared p50/p99/máx = 772.939000/1527.391000/2190.925000 µs, xruns de pared medidos = 0 (la pared mide la carga del host, no se congela).
inline constexpr double kCpuP50MaxUs = 3087.428000;
inline constexpr double kCpuP99MaxUs = 4254.168000;
// Máx: 2x el presupuesto (5333 µs), no el presupuesto pelado:
// el máx es un estadístico extremo y el motor tiene picos de hilo
// de hasta 4393 µs (medido en la congelación final) que NO son
// xruns de pared; un máx a 2666.67 sería flaky. p50/p99 son los
// límites que realmente vigilan el coste del motor.
inline constexpr double kCpuMaxMaxUs = 5333.33;
inline constexpr int kXrunsMax = 0;
}  // namespace p0
