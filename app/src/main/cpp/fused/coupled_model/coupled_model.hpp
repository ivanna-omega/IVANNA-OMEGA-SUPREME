// coupled_model.hpp — FASE 3 (modelo acoplado no lineal, IVANNA OMEGA SUPREME).
//
// Traducción a código C++17 determinista (IEEE 754 binary64, sin I/O, sin
// aleatoriedad, sin estado global mutable) de FORMULACION.md (F1) y
// PREDICCIONES_FALSABLES.md (F2). Todo el archivo es header-only para que
// la herramienta offline y los tests lo usen sin tocar la FusedChain.
//
// REGLAS DE ESTE ARCHIVO (prohibición de humo, F1 §0):
//  * Cada símbolo lleva ecuación, unidades SI y dominio de validez en el
//    comentario que lo define. Lo que no tiene ecuación no se nombra.
//  * Los parámetros del Genezi NO identificados van marcados UNIDENTIFIED
//    con su rango supuesto (F2 §0) y NUNCA se presentan como medidos.
//  * El modelo completo es herramienta OFFLINE (F2 §3.4: 7.8-12.8 GFLOPS
//    ingenuo vs 2.67 ms/hop). En tiempo real solo van las correcciones
//    identificadas de la sección RT al final de este archivo.
//
// CORRECCIONES APLICADAS (auditoría F2 §3, §6):
//  [C1] Integrador exponencial exacto por banda ERB (F2 §3.1, opción (b),
//       recomendada) en vez de RK4 a 48 kHz para la cóclea: RK4 pierde
//       31 % de amplitud por paso a 16 kHz y su error de fase EXACTO es
//       +14.4°/ciclo a 16 kHz y +164.6°/ciclo a 20 kHz (ver [D7]). El
//       bloque modal (f < 139 Hz, y <= 0.018) y el transductor SÍ son
//       exactos con RK4.
//  [D7] FASE RK4 EXACTA vs APROXIMACIÓN DE F2 (hallazgo Fase 3): la tabla
//       de F2 §3.1 usaba la aproximación Δφ ≈ y⁵/120 rad/paso, válida
//       solo para y ≲ 1 (f ≲ 8 kHz). El valor EXACTO es arg(R(iy)) con
//       R(z) = 1+z+z²/2+z³/6+z⁴/24 en rama continua: 4 kHz +0.2°/ciclo,
//       8 kHz +2.3°/ciclo, 16 kHz +14.4°/ciclo (ADELANTO; la aproximación
//       decía +57.7° con el signo cambiado) y 20 kHz +164.6°/ciclo (la
//       aproximación decía 140.9°). La conclusión cualitativa NO cambia
//       (RK4 inservible > 4 kHz), pero los números exactos son estos y
//       son los que verifica el test.
//  [C2] Aliasing (F2 §3.3): la herramienta offline corre a f_s,off =
//       192 kHz por defecto (4x). A 48 kHz los términos no lineales solo
//       son válidos con banda limitada a f < 8 kHz (3er orden) / < 12 kHz
//       (2º orden). El término cúbico coclear a 48 kHz pliega el 3er
//       armónico de 16 kHz a DC: el test lo demuestra.
//  [C3] Coste tiempo real (F2 §3.4): el acoplo no lineal modal ingenuo
//       NO va al hilo de audio. Solo las correcciones RT (§RT).
//  [D1] Gamma_k corregido (F2 §3.2.1): Gamma_k [Pa·m⁻³] x Udot_s
//       [m³·s⁻²] = Pa·s⁻². La forma de F1 (3.4), Gamma_k x S_d v, daba
//       Pa donde qdd exige Pa·s⁻². Forma canónica:
//         Gamma_k = rho0 c0² phi_k(r_s) / V,   phi adimensional,
//         int_Omega phi_k² dV = V  (phi_k = 2^{n/2} prod cos, n = nº de
//         índices no nulos). qdd_k + 2 zeta_k w_k qdot_k + w_k² q_k
//         = Gamma_k Udot_s + (beta/(rho0 c0²)) d²/dt² Pi_k[p²].
//  [D2] Westervelt con factor 1/2 (F2 §3.2.2): p2/p1 =
//       beta k z P_rms / (2 rho0 c0²)  =>  -56.6 dB a 110 dB SPL / 1 kHz
//       / 3 m (convención F2: P = presión RMS derivada del SPL).
//       DIVERGENCIA REGISTRADA (hallazgo Fase 3, ver westerveltRatioDb
//       y el test): la forma estándar de Beyer / Hamilton-Blackstock usa
//       P = amplitud de pico y da -53.6 dB, es decir 3.01 dB (= 20log10
//       sqrt(2)) por encima. La serie de Fubini lo confirma
//       numéricamente. La diferencia es EXACTAMENTE la tolerancia de R-B
//       (±3 dB): la medición en hardware debe resolverla; el protocolo
//       registra ambos candidatos (-56.6 F2 / -53.6 Beyer).
//  [D3] Energía modal RE-DERIVADA (F2 §3.2.1 exigía re-derivar (5.2)):
//       E_ac,k = V/(2 rho0 c0²) (q_k² + w_k² Q_k²), Q_k = int_0^t q_k dt,
//       [J]. La forma de F1, qdot²/(2 Gamma w²) + q²/(2 Gamma), es
//       dimensionalmente consistente con el Gamma corregido pero NO es
//       la energía física: depende de phi_k(r_s) (posición de la
//       fuente). Con la forma re-derivada el balance es EXACTO:
//       dE_ac/dt (entrada) = p_s U_s  y  P_perd,modal,k =
//       (2 V zeta_k w_k / (rho0 c0²)) q_k²  [W]  (la disipación usa q²,
//       no qdot²; en promedio sinusoidal ambas coinciden).
//       Demostración en el comentario de modalEnergy().
//  [D4] N modal EXACTO: la sala 4.0x7.0x2.5 m tiene 35 modos con
//       f < 139.4 Hz (conteo rectangular exacto), no 37: el 37 de F2 es
//       la estimación asintótica de Weyl N(f), no el conteo exacto.
//  [D5] P-A.4 NO REPRODUCIDO (hallazgo Fase 3, ver resonanceShiftHz):
//       la suma modal exacta da Delta f_s (esquina-centro) = 0.017 Hz,
//       no 0.2-0.8 Hz. La estimación de F2 suponía una diferencia de
//       masa reactiva modal de (2-5)x10⁻⁴ kg; la suma exacta da
//       1.8x10⁻⁵ kg. La parte reactiva está dominada por el campo
//       directo / fuentes imagen (modos > 139 Hz, fuera de la base
//       truncada). Consecuencia: el criterio R-A sobre Delta f_s
//       (> 0.3 Hz) NO puede falsar el modelo modal como está escrito;
//       la pata operativa de P-A es Delta phi (máx 2.71° a 49.5 Hz en
//       esquina con los parámetros de referencia, 22/201 frecuencias
//       > 1°). Delta f_s se sigue midiendo en hardware, pero su
//       predicción modal es 0.02 Hz y un valor medido de 0.2-0.8 Hz
//       apuntaría a física fuera de la base modal (Fase 2-bis).
//  [D6] P-B2: el NIVEL -79.9 dB (ley de onda plana) y la TASA DE
//       DECAIMIENTO tau_2 = tau_1/2 son dos afirmaciones distintas.
//       La tasa tau_1/2 vale para la componente GENERADA (p2 ~ p1²,
//       y para Pi_2[p²](t) tras el corte). La cola OBSERVADA del modo
//       (0,0,2) a 137.2 Hz, al ser un modo resonante con el mismo
//       T60, repica con tau_1 a tiempos largos: el criterio R-B2 como
//       está escrito ("tau_2 = tau_1 => refutado") confunde la
//       componente generada con el repique modal. El protocolo mide la
//       cola TEMPRANA (ventanas de 25 ms, no 100 ms) para la firma
//       tau_1/2. El test verifica la tasa de Pi_2[p²], no el repique.
//
//  [R1] RIR (dato nuevo Esse 2026-11-26): ~200 RIR en
//       app/src/main/assets/ivanna_omega/rir/ (salas VARIADAS,
//       38.6-1443 m³, NINGUNA es la sala Genezi 4.0x7.0 m). Ver
//       validate_rir.py. CORRECCIÓN [R4]: los RIR son SINTÉTICOS
//       (generador), NO mediciones reales — verificado 2026-11-26
//       ~20:01 CST por el coordinador de élite Y re-verificado en
//       Fase 3 desde los WAV: corr(L,R) ≈ 0.999999 (dual-mono),
//       pico L = 0.9999 en todos (normalización del generador),
//       residuo EDC 0.045-0.085 dB (decaimiento exponencial perfecto),
//       9 archivos con RT60_s = 2.5 s exactos (tope del generador).
//       Consecuencia: la validación RIR valida la CADENA (estimador
//       Schroeder, frecuencias modales, consistencia Sabine) contra
//       una verdad sintética conocida — NO la física contra salas
//       reales. El factor 2.284 y el 81% de coincidencia modal son
//       propiedades DEL GENERADOR, no de la realidad. La validación
//       física sigue pendiente del barrido de Esse (9 posiciones).
//       Las únicas mediciones REALES de sala son el barrido Farina
//       2026-11-22 (1 posición a 3 m) y la THD del sistema Genezi:
//       100 Hz 0.65%/2.39%, 1 kHz 0.30%/0.39%, 2 kHz 0.38%/0.80%
//       (lo/hi); RT60 real sigue sin poder reportarse (la EDC se
//       aplana tras ~300 ms; T10 0.54/0.41 s, T20 0.70/0.61 s,
//       T30 1.07/0.83 s). Ver CODIGO_D10.md §7.
//  [R2] T60: DOS COLUMNAS, USAR LA MEDIDA (hallazgo validación RIR):
//       RT60_s (nominal) y RT60_T20_measured_s (T20x3 medido) difieren
//       por un factor SISTEMÁTICO 2.284 ± 0.022 en las 200 RIR: el
//       decaimiento SINTETIZADO es 2.28x más lento que el nominal
//       (pendiente única, no doble pendiente: T60(-5..-15) ≈
//       T60(-15..-35)). Es una inconsistencia DEL GENERADOR entre su
//       parámetro nominal y el decaimiento que sintetiza. Para
//       zeta_k = 6.9/(w·T60) usar el T60 MEDIDO (el que describe el
//       repique del RIR); el nominal no describe los RIR. En la sala
//       Genezi el T60 de control es 0.34 s (SALA VIVA) y NO es una
//       medida: el informe RT60 2026-11-23 no pudo derivar RT60 del
//       barrido (deconvolución inestable, 2.7-6.6 s absurdos), y la
//       revisión 2026-11-26 tampoco (EDC plana tras ~300 ms).
//  [R3] ALCANCE KEMAR / 2.1+ (aclaración Esse 2026-11-26): la geometría
//       3D es un ESTÁNDAR PRECISO Y UNIVERSAL (cabeza tipo KEMAR), no
//       escaneo personalizado. El modelo vale para 2.1 en adelante
//       (estéreo + subwoofer y multicanal, no solo audífonos): el
//       estado X es único y cada receptor es una lectura p(r) del
//       mismo estado; cada transductor lleva su propio r_s en el
//       acoplo. Objetivo: hiperrealismo holográfico (presencia como
//       abrir un portal). Ver pressureAt(Vec3) y kKemarEarOffsetM.
//       Ley ITD candidata para cabeza estándar (redescubierta por
//       PySR desde 710 posiciones KEMAR reales, coordinador de élite
//       2026-11-26 ~20:01 CST): ITD(θ) = -1409·θ + 918.6·sinθ +
//       144.8·θ³ [µs], θ en rad; RMSE 5.52 µs, R² = 0.99987; radio
//       efectivo 9.33 cm vs KEMAR 8.75 cm. CANDIDATA, no validada por
//       Fase 3: verificar su convención de signo y su acuerdo con el
//       modelo ORBE antes de usarla como ley. Ver kemarItdUs().
//  [R4] Ver [R1]: RIR SINTÉTICOS. (Nota numerada aquí para citarla
//       desde el protocolo y el contraste: la validación RIR es de
//       cadena, no de física; la física se valida con el barrido
//       real de 9 posiciones.)
//  [R5] DERIVA DE RELOJ DAC↔ADC (hueco real, crítica externa aceptada
//       2026-11-26 ~20:03 CST): el DAC JieLi USB y el ADC del G85
//       derivan 10-50 ppm entre sí; sin compensarla, la fase del
//       bucle cerrado gira 360° cada pocos segundos (a 50 ppm y
//       100 Hz: 1.8°/s; un barrido de 20 s acumula 36°, 10x la señal
//       P-A de 1.5-2.8°). CONSECUENCIA PARA EL PROTOCOLO: ninguna
//       comparación de fase (P-A, P-C) es falsable sin estimar y
//       compensar la deriva ANTES del contraste: GCC-PHAT entre el
//       estímulo conocido y el micrófono + resincronización Farrow
//       de 5º orden a <0.01 muestras, o referencia de lazo (loopback
//       eléctrico del DAC junto al micrófono). P-B y P-D (amplitud)
//       son insensibles a primer orden. Los otros dos huecos de la
//       misma crítica quedan para Fase 2-bis: (1) solucionador
//       simbólico-numérico de EDOs no lineales acopladas
//       (Diffeq-Julia) como referencia cruzada del integrador de
//       192 kHz; (2) reconstrucción continua de campo
//       (Kirchhoff-Helmholtz/WFS/XTC) para el portal en 2.1+:
//       pressureAt() es lectura modal punto a punto, no
//       reconstrucción de campo. Ver CODIGO_D10.md §8.
//
// DOMINIO DE VALIDEZ (F2 §4, honesto):
//  * Campo modal: f < f_Sch = 139.4 Hz (35 modos). Por encima, el modelo
//    NO predice campo/fase/nivel (58 millones de modos a 20 kHz).
//  * P-B (Westervelt 1-4 kHz) y P-D (histéresis) SÍ proceden: no usan la
//    base modal.
//  * SPL: régimen débil estricto 60-100 dB; 100-110 dB extendido con
//    advertencia (O(P³) ~ -90 dB relativo, medible frente al piso D10).
//    Por encima de 110 dB SPL no hay predicción válida.
//  * Aire en reposo, sin flujo medio, sin ondas de choque, sin
//    turbulencia del puerto bass-reflex ni térmica del bobinado.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace ivanna::fused::coupled {

// ── Constantes de referencia (F2 §0; todas SI) ──────────────────────────
inline constexpr double kRho0 = 1.2;        // [kg·m⁻³] densidad del aire
inline constexpr double kC0 = 343.0;        // [m·s⁻¹] velocidad del sonido
inline constexpr double kBetaAir = 1.2;     // [-] parámetro de no linealidad
                                            //     del aire (Westervelt)
inline constexpr double kRoomLx = 4.0;      // [m] sala (x)
inline constexpr double kRoomLy = 7.0;      // [m] sala (y)
inline constexpr double kRoomLz = 2.5;      // [m] sala (z)
inline constexpr double kRoomVolume = 70.0; // [m³] V = 4*7*2.5
inline constexpr double kT60 = 0.34;        // [s] RT60 medido (SALA VIVA)
inline constexpr double kSchroederHz = 139.4; // [Hz] 2000*sqrt(T60/V)
inline constexpr double kFsAudio = 48000.0;   // [Hz] muestreo de audio
inline constexpr double kFsOffline = 192000.0; // [Hz] muestreo offline (4x,
    // resuelve el aliasing no lineal hasta 20 kHz, F2 §3.3)
inline constexpr double kHopMs = 2.6666666666666665; // [ms] hop de 128
                                                     // muestras a 48 kHz
inline constexpr double kRefPressurePa = 20e-6; // [Pa] 0 dB SPL

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0; // [m]
};

// ── Parámetros del transductor (Genezi PZ1D) ────────────────────────────
// UNIDENTIFIED (F2 §0, §6.5): valores supuestos explícitos con su rango.
// Reemplazar por identificación en Fase 3 antes de cerrar ninguna
// predicción como número exacto. Los tests de cociente (P-A, P-C) solo
// dependen de (S_d, r_m, m, f_s); K_t/K_e/R_e/L solo fijan niveles
// absolutos del simulador dinámico, no los cocientes.
struct TransducerParams {
    double sd = 0.020;    // [m²] área efectiva del diafragma (rango
                          // 0.013-0.030; woofer Ø≈16 cm)
    double mMs = 0.030;   // [kg] masa móvil (rango 0.020-0.040)
    double rM = 1.0;      // [N·s·m⁻¹] amortiguamiento mecánico
                          // (rango 0.8-1.5)
    double fsHz = 55.0;   // [Hz] frecuencia de resonancia en aire libre
    double re = 6.0;      // [Ω] resistencia de bobina
    double le = 1.0e-3;   // [H] inductancia de bobina
    double kt0 = 8.0;     // [N·A⁻¹] constante de fuerza (sin histéresis)
    double ke0 = 8.0;     // [V·s·m⁻¹] constante de Back-EMF
    // Jiles-Atherton (F1 §6.1). UNIDENTIFIED: sin lazo B-H medido no hay
    // valores; los defaults son de ferrita típica de motor de altavoz
    // y SOLO alimentan P-D como orden de magnitud.
    double ms = 4.0e5;    // [A·m⁻¹] magnetización de saturación
    double jaA = 1.0e3;   // [A·m⁻¹] parámetro de forma anhisterética
    double jaK = 1.5e3;   // [A·m⁻¹] parámetro de pinning
    double jaAlpha = 1.0e-4; // [-] acoplamiento inter-dominio
    double jaC = 0.2;     // [-] reversibilidad
    double chiM = 0.05;   // [-] sensibilidad de K_t a M (rango 0.03-0.08)
    double chiE = 0.05;   // [-] sensibilidad de K_e a M
    double turns = 100.0; // [-] vueltas de bobina (para H = N i / l_m)
    double lm = 0.05;     // [m] longitud magnética
    double xMax = 8.0e-3; // [m] excursión máxima admisible
    double iMax = 8.0;    // [A] corriente máxima admisible
    double km() const {   // [N·m⁻¹] rigidez: k = (2π f_s)² m
        const double w = 2.0 * 3.14159265358979323846 * fsHz;
        return w * w * mMs;
    }
};

// ── Geometría de sala (generalizada para los RIR, [R1]) ────────────────
// La formulación F1 fijaba la sala Genezi 4.0x7.0x2.5 m. El código
// generaliza: RoomGeometry parametriza cualquier sala rectangular, y los
// ~200 RIR (salas 38.6-1443 m³) se validan con sus propias dimensiones
// (validate_rir.py). La sala Genezi sigue siendo el default.
// Ejes: x = ancho, y = profundidad, z = alto (igual que la metadata RIR:
// room_width_m -> x, room_depth_m -> y, room_height_m -> z).
struct RoomGeometry {
    double lx = kRoomLx;  // [m] ancho (x)
    double ly = kRoomLy;  // [m] profundidad (y)
    double lz = kRoomLz;  // [m] alto (z)
    double t60 = kT60;    // [s] RT60 MEDIDO (ver [R2]: usar el decaimiento
                          // medido, no el nominal de la metadata)
    double volume() const { return lx * ly * lz; }  // [m³]
    double surfaceArea() const {  // [m²] S = 2(lx·ly + lx·lz + ly·lz)
        return 2.0 * (lx * ly + lx * lz + ly * lz);
    }
    double schroederHz() const {  // [Hz] 2000·sqrt(T60/V)
        return 2000.0 * std::sqrt(t60 / volume());
    }
};
inline constexpr RoomGeometry kGeneziRoom{};  // 4.0x7.0x2.5 m, T60 0.34 s

// ── Modos rectangulares (base ortonormal, phi adimensional) ─────────────
// phi_k(r) = 2^{n/2} cos(nx π x/lx) cos(ny π y/ly) cos(nz π z/lz),
// n = nº de índices no nulos. Entonces int_Omega phi_k² dV = V y
// phi_k² en antinodo = 2^n (2 para un índice no nulo, 4 para dos, 8
// para tres). Frecuencia: f_k = (c0/2) sqrt((nx/lx)²+(ny/ly)²+(nz/lz)²).
// Amortiguamiento modal desde T60: zeta_k = 6.9 / (w_k T60)  [F2 §0:
// de exp(-zeta w T60) = 10⁻³]; tau_1 (amplitud) = T60/6.9 = 49.3 ms.
struct RoomMode {
    int nx = 0, ny = 0, nz = 0;  // [-] índices modales
    double freqHz = 0.0;         // [Hz]
    double omega = 0.0;          // [rad·s⁻¹]
    double zeta = 0.0;           // [-]
    double lx = kRoomLx, ly = kRoomLy, lz = kRoomLz;  // [m] sala
    double volume() const { return lx * ly * lz; }    // [m³]
    // phi_k(r_s) [adimensional].
    double phiAt(const Vec3& r) const {
        const double pi = 3.14159265358979323846;
        const int n = (nx > 0 ? 1 : 0) + (ny > 0 ? 1 : 0) + (nz > 0 ? 1 : 0);
        return std::pow(2.0, 0.5 * n) * std::cos(pi * nx * r.x / lx) *
               std::cos(pi * ny * r.y / ly) *
               std::cos(pi * nz * r.z / lz);
    }
    // Gamma_k [Pa·m⁻³] = rho0 c0² phi_k(r_s) / V  (forma canónica [D1]).
    double gammaAt(const Vec3& rs) const {
        return kRho0 * kC0 * kC0 * phiAt(rs) / volume();
    }
};

inline std::vector<RoomMode> roomModesFor(const RoomGeometry& g,
                                          double fMaxHz) {
    std::vector<RoomMode> modes;
    const double pi = 3.14159265358979323846;
    // Cota por dimensión: nx <= 2·fMax·lx/c0 + 1 (un modo más allá no
    // puede estar por debajo de fMax). Sin esto, salas grandes con
    // f_Sch alta (hasta 303 Hz en los RIR) perderían modos.
    const int nxMax = static_cast<int>(2.0 * fMaxHz * g.lx / kC0) + 1;
    const int nyMax = static_cast<int>(2.0 * fMaxHz * g.ly / kC0) + 1;
    const int nzMax = static_cast<int>(2.0 * fMaxHz * g.lz / kC0) + 1;
    for (int nx = 0; nx <= nxMax; ++nx)
        for (int ny = 0; ny <= nyMax; ++ny)
            for (int nz = 0; nz <= nzMax; ++nz) {
                if (nx == 0 && ny == 0 && nz == 0) continue;  // modo uniforme
                const double f = 0.5 * kC0 *
                    std::sqrt((nx / g.lx) * (nx / g.lx) +
                              (ny / g.ly) * (ny / g.ly) +
                              (nz / g.lz) * (nz / g.lz));
                if (f >= fMaxHz) continue;
                RoomMode m;
                m.nx = nx; m.ny = ny; m.nz = nz;
                m.freqHz = f;
                m.omega = 2.0 * pi * f;
                m.zeta = 6.9 / (m.omega * g.t60);
                m.lx = g.lx; m.ly = g.ly; m.lz = g.lz;
                modes.push_back(m);
            }
    std::sort(modes.begin(), modes.end(),
              [](const RoomMode& a, const RoomMode& b) {
                  return a.freqHz < b.freqHz;
              });
    return modes;
}

// Sala Genezi por defecto (fMax = f_Sch = 139.4 Hz, T60 = 0.34 s).
inline std::vector<RoomMode> roomModes(double fMaxHz = kSchroederHz,
                                       double t60 = kT60) {
    RoomGeometry g;
    g.t60 = t60;
    return roomModesFor(g, fMaxHz);
}

// Impedancia de radiación modal (P-A.1, F2 §1):
//   Z_rad(w) = Σ_k Gamma_k phi_k(r_s) jw / (w_k² - w² + 2j zeta_k w_k w)
// [Pa·s·m⁻³]. En resonancia modal (w = w_k, antinodo phi² = 2):
//   |Z_rad|_res = rho0 c0² phi² T60 / (13.8 V) ≈ 99.4 Pa·s·m⁻³ (un solo
//   modo; la suma completa es mayor: a 68.6 Hz en esquina, |Z| = 134.7).
inline std::complex<double> radiationImpedance(
    const std::vector<RoomMode>& modes, double omega, const Vec3& rs) {
    std::complex<double> z(0.0, 0.0);
    const std::complex<double> jw(0.0, omega);
    for (const RoomMode& m : modes) {
        const double g = m.gammaAt(rs);
        const double ph = m.phiAt(rs);
        const std::complex<double> den(m.omega * m.omega - omega * omega,
                                       2.0 * m.zeta * m.omega * omega);
        z += g * ph * jw / den;
    }
    return z;
}

// Impedancia mecánica del transductor (sin sala):
//   Z_m(w) = r_m + j(w m - k_m/w)   [N·s·m⁻¹].
inline std::complex<double> mechanicalImpedance(const TransducerParams& p,
                                                double omega) {
    return {p.rM, omega * p.mMs - p.km() / omega};
}

// P-A (F1 (7.2)-(7.3)): v_c/v_0 = Z_m / (Z_m + S_d² Z_rad); el
// desplazamiento de fase es arg(v_c) - arg(v_0) = arg(Z_m/(Z_m+S_d²Z)).
// Clásico (desacoplado): Delta phi = 0.00°. Acoplado (referencia
// S_d = 0.020 m², r_m = 1.0): máx 2.71° a 49.5 Hz en esquina.
inline double deltaPhiDeg(const TransducerParams& p,
                          const std::vector<RoomMode>& modes, double omega,
                          const Vec3& rs) {
    const std::complex<double> zm = mechanicalImpedance(p, omega);
    const std::complex<double> zr = radiationImpedance(modes, omega, rs);
    const std::complex<double> ratio = zm / (zm + p.sd * p.sd * zr);
    return std::arg(ratio) * 180.0 / 3.14159265358979323846;
}

// P-C (F2 (P-C.1)): |v_c/v_0| en resonancia modal. Clásico: 1.000.
// Acoplado (referencia): mín 0.9838 en resonancias modales en esquina.
inline double velocityRatio(const TransducerParams& p,
                            const std::vector<RoomMode>& modes, double omega,
                            const Vec3& rs) {
    const std::complex<double> zm = mechanicalImpedance(p, omega);
    const std::complex<double> zr = radiationImpedance(modes, omega, rs);
    return std::abs(zm / (zm + p.sd * p.sd * zr));
}

// P-A.4 / D3 (F1 (7.5)): desplazamiento de f_s por masa acústica
// reactiva. m_rad = S_d² Im(Z_rad)/w  [kg]; Delta f_s/f_s = -m_rad/(2 m).
// HALLAZGO [D5]: la suma modal exacta (f < 139 Hz) da entre esquina
// (0.3,0.3,1.2) y centro (2.0,3.5,1.2) Delta f_s = 0.017 Hz, NO 0.2-0.8
// Hz. La masa reactiva modal diferencial es 1.8e-5 kg, no (2-5)e-4 kg.
inline double resonanceShiftHz(const TransducerParams& p,
                               const std::vector<RoomMode>& modes,
                               const Vec3& rsA, const Vec3& rsB) {
    const double w = 2.0 * 3.14159265358979323846 * p.fsHz;
    const double imA = radiationImpedance(modes, w, rsA).imag();
    const double imB = radiationImpedance(modes, w, rsB).imag();
    const double dm = p.sd * p.sd * (imA - imB) / w;  // [kg]
    return -0.5 * (dm / p.mMs) * p.fsHz;               // [Hz]
}

// ── Westervelt: 2º armónico en propagación (F2 (P-B.1)) ────────────────
// Onda plana progresiva débil, p1 = P cos(wt - kz):
//   p2/p1 = beta k z P_rms / (2 rho0 c0²)   (convención F2 [D2]).
// splDbToPressurePa convierte dB SPL a presión RMS: P_rms = 20µPa·10^(L/20).
inline double splDbToPressurePa(double splDb) {
    return kRefPressurePa * std::pow(10.0, splDb / 20.0);
}
inline double westerveltRatioDb(double beta, double freqHz, double zM,
                                double splDb) {
    const double pRms = splDbToPressurePa(splDb);       // [Pa] RMS
    const double k = 2.0 * 3.14159265358979323846 * freqHz / kC0;  // [rad·m⁻¹]
    const double ratio = beta * k * zM * pRms / (2.0 * kRho0 * kC0 * kC0);
    return 20.0 * std::log10(ratio);
}
// Forma estándar de Beyer / Hamilton-Blackstock (P = amplitud de PICO):
// p2/p1 = beta k z P_pico / (2 rho0 c0²), P_pico = sqrt(2) P_rms.
// Da EXACTAMENTE +3.01 dB sobre la convención F2. Ver [D2].
inline double westerveltRatioDbBeyerPeak(double beta, double freqHz,
                                         double zM, double splDb) {
    return westerveltRatioDb(beta, freqHz, zM, splDb) +
           20.0 * std::log10(std::sqrt(2.0));
}
// Serie de Fubini (solución exacta de la ecuación de Burgers sin
// disipación, sigma = beta k z P_pico / (rho0 c0²) = parámetro de
// choque): p/p0 = Σ_n (2/(n sigma)) J_n(n sigma) sin(n(wt-kz)).
// p2/p1 = J_2(2σ) / (2 J_1(σ)). Para σ << 1 tiende a σ/2 (Beyer pico).
inline double fubiniSecondHarmonicRatio(double sigma) {
    const double j1 = std::cyl_bessel_j(1, sigma);
    const double j2 = std::cyl_bessel_j(2, 2.0 * sigma);
    return j2 / (2.0 * j1);
}
// Constantes de tiempo modales (F2 §0): tau_1 = T60/6.9 (amplitud);
// la cola del 2º armónico generado decae al doble de velocidad:
// tau_2 = tau_1/2 (solo la componente generada, ver [D6]).
inline double modalTau1(double t60 = kT60) { return t60 / 6.9; }  // [s]
inline double modalTau2(double t60 = kT60) { return t60 / 13.8; } // [s]

// ── Tensor de acoplo triple C_ijk (proyección de p² sobre la base) ──────
// Pi_k[p²] = Σ_ij C_ijk q_i q_j,  C_ijk = int phi_i phi_j phi_k dV / V.
// Para cosenos rectangulares la integral 1-D es exacta por
// product-to-sum: int_0^L cos(p πx/L) cos(q πx/L) cos(r πx/L) dx
//   = (L/4) · #{p+q+r, p+q-r, p-q+r, -p+q+r iguales a 0}.
// Regla de selección: la mayoría de los 35³ = 42875 tripletes se anulan;
// quedan 1614 no nulos (3.8 %). Coste offline ~1614 mults/evaluación,
// muy por debajo de la estimación ingenua O(K³) de F2 §3.4.
inline double cosTripleIntegral(int p, int q, int r, double length) {
    int count = 0;
    if (p + q + r == 0) ++count;
    if (p + q - r == 0) ++count;
    if (p - q + r == 0) ++count;
    if (-p + q + r == 0) ++count;
    return 0.25 * length * count;
}
struct TripleCoupling {
    std::size_t i = 0, j = 0, k = 0;  // índices modales
    double c = 0.0;                   // [adimensional] C_ijk
};
inline std::vector<TripleCoupling> tripleCouplings(
    const std::vector<RoomMode>& modes) {
    std::vector<TripleCoupling> out;
    if (modes.empty()) return out;
    const double lx = modes[0].lx, ly = modes[0].ly, lz = modes[0].lz;
    const double vol = modes[0].volume();
    const std::size_t n = modes.size();
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
            for (std::size_t k = 0; k < n; ++k) {
                const double ix =
                    cosTripleIntegral(modes[i].nx, modes[j].nx, modes[k].nx, lx);
                if (ix == 0.0) continue;
                const double iy =
                    cosTripleIntegral(modes[i].ny, modes[j].ny, modes[k].ny, ly);
                if (iy == 0.0) continue;
                const double iz =
                    cosTripleIntegral(modes[i].nz, modes[j].nz, modes[k].nz, lz);
                if (iz == 0.0) continue;
                const int ni = (modes[i].nx > 0 ? 1 : 0) +
                               (modes[i].ny > 0 ? 1 : 0) +
                               (modes[i].nz > 0 ? 1 : 0);
                const int nj = (modes[j].nx > 0 ? 1 : 0) +
                               (modes[j].ny > 0 ? 1 : 0) +
                               (modes[j].nz > 0 ? 1 : 0);
                const int nk = (modes[k].nx > 0 ? 1 : 0) +
                               (modes[k].ny > 0 ? 1 : 0) +
                               (modes[k].nz > 0 ? 1 : 0);
                const double c = std::pow(2.0, 0.5 * (ni + nj + nk)) *
                                 ix * iy * iz / vol;
                if (c != 0.0) out.push_back({i, j, k, c});
            }
    return out;
}

// ── Jiles-Atherton (F1 §6.1) ────────────────────────────────────────────
// M_an = M_s [coth((H+aM)/a) - a/(H+aM)]  (Langevin), H = N i / l_m [A·m⁻¹].
// dM_irr/dH = (M_an - M_irr) / (k δ - alpha (M_an - M_irr)), δ = ±1 signo
// de dH/dt; dM/dH = c dM_an/dH + (1-c) dM_irr/dH. K_t(M), K_e(M) por (6.4).
// GUARDA DE RIGIDEZ (F2 §3.1): si |dM/dH| hace |lambda| T_s > 2.78 el
// RK4 explícito diverge (no degrada: explota). El integrador acota
// |dM/dH| <= dMdHMax y levanta la bandera stiffnessClamped: si se
// levanta, la identificación debe revisar los parámetros J-A.
struct JilesAtherton {
    TransducerParams p;
    double m = 0.0;    // [A·m⁻¹] magnetización total M
    double mIrr = 0.0; // [A·m⁻¹] componente irreversible
    double dMdHMax = 1.0e4;  // [-] cota de rigidez (a identificar)
    bool stiffnessClamped = false;  // se levanta si la cota actúa

    static double langevin(double x) {  // coth(x) - 1/x, estable en x=0
        if (std::abs(x) < 1e-6) return x / 3.0 - x * x * x / 45.0;
        return 1.0 / std::tanh(x) - 1.0 / x;
    }
    double man(double h) const {  // [A·m⁻¹] magnetización anhisterética
        const double he = h + p.jaAlpha * m;
        return p.ms * langevin(he / p.jaA);
    }
    // Avanza M un paso de corriente i0 -> i1 (H0 -> H1). Devuelve M.
    double step(double i0, double i1) {
        const double h0 = p.turns * i0 / p.lm;
        const double h1 = p.turns * i1 / p.lm;
        const double dh = h1 - h0;
        if (dh == 0.0) return m;
        const double delta = dh > 0.0 ? 1.0 : -1.0;
        const double mAn = man(h1);
        // dM_an/dH por diferencias centradas (paso 1e-3·a).
        const double e = 1e-3 * p.jaA;
        const double dManDh =
            (p.ms * langevin((h1 + e + p.jaAlpha * m) / p.jaA) -
             p.ms * langevin((h1 - e + p.jaAlpha * m) / p.jaA)) / (2.0 * e);
        double denom = p.jaK * delta - p.jaAlpha * (mAn - mIrr);
        double dMIrrDh = (mAn - mIrr) / denom;
        double dMDh = p.jaC * dManDh + (1.0 - p.jaC) * dMIrrDh;
        if (std::abs(dMDh) > dMdHMax) {
            dMDh = std::copysign(dMdHMax, dMDh);
            stiffnessClamped = true;
        }
        // (1-c) de la parte irreversible se integra en M_irr por separado.
        mIrr += dMIrrDh * dh;
        m += dMDh * dh;
        return m;
    }
    double kt() const {  // [N·A⁻¹] K_t(M) = K_t0 (1 + chi_M M/M_s)
        return p.kt0 * (1.0 + p.chiM * m / p.ms);
    }
    double ke() const {  // [V·s·m⁻¹]
        return p.ke0 * (1.0 + p.chiE * m / p.ms);
    }
};

// ── Integrador exponencial exacto del oscilador amortiguado [C1] ────────
// y'' + 2 zeta w y' + w² y = f (f constante en el paso, zero-order hold).
// Solución cerrada por paso: EXACTA en fase y amplitud para la parte
// lineal, para cualquier w·h (sin el límite y <= 0.5 del RK4). Es la
// corrección (b) de F2 §3.1, recomendada sobre el subpaso x4.
// La no linealidad cúbica (Duffing) y la adaptación se tratan por
// splitting de Strang (medio paso no lineal - paso lineal exacto -
// medio paso no lineal) en CochlearBand.
// Los coeficientes del mapa lineal 2x2 se PRECOMPUTAN en el constructor
// (w, zeta, h fijos): por muestra el paso son ~6 multiplicaciones, sin
// exp/cos/sin en el camino caliente. Sin esto, el bloque RT-1 pagaría
// 30 trascendentes por modo y muestra.
struct ExpOscillator {
    double omega = 0.0;  // [rad·s⁻¹]
    double zeta = 0.0;   // [-]
    double h = 0.0;      // [s] paso (fijo por instancia)
    // Mapa [y'; v'] = M [y; v] + g·f (f constante en el paso).
    double m11 = 1.0, m12 = 0.0, m21 = 0.0, m22 = 1.0, g1 = 0.0, g2 = 0.0;
    ExpOscillator() = default;
    ExpOscillator(double w, double z, double stepH) { init(w, z, stepH); }
    void init(double w, double z, double stepH) {
        omega = w;
        zeta = z;
        h = stepH;
        const double wd = w * std::sqrt(std::max(0.0, 1.0 - z * z));
        const double e = std::exp(-z * w * h);
        const double cw = std::cos(wd * h), sw = std::sin(wd * h);
        // y' = e[(cw + z w sw/wd) y + (sw/wd) v] + yEq (1 - e[...]);
        // con yEq = f/w²: g1 = (1 - m11 - ...)/w²... se deriva directo:
        // y' = m11 y + m12 v + g1 f ; v' = m21 y + m22 v + g2 f.
        m11 = e * (cw + z * w * sw / wd);
        m12 = e * (sw / wd);
        // Derivación: v' = d/dt y' con y(t) = e^{-zwt}(a cw + b sw) + yEq,
        // a = y0 - yEq, b = (v0 + zw a)/wd. Coeficiente de y0 en v':
        // e[(-zw + wd·zw/wd) cw + (-(zw²+wd²)/wd) sw] = -e w² sw/wd.
        m21 = e * (-w * w * sw / wd);
        m22 = e * (cw - z * w * sw / wd);
        g1 = (1.0 - m11) / (w * w);
        g2 = -m21 / (w * w);
    }
    // Un paso exacto: (y, v) -> (y', v') con fuerza constante f [m·s⁻²].
    // Nota: g2 se deriva de m21: con f constante, el equilibrio es
    // yEq = f/w², vEq = 0; v' = m21 (y - yEq) + m22 v.
    void step(double& y, double& v, double f) const {
        const double y0 = y, v0 = v;
        y = m11 * y0 + m12 * v0 + g1 * f;
        v = m21 * y0 + m22 * v0 + g2 * f;
    }
    // Variante con fuerza explícita por paso (compatibilidad: calcula el
    // mapa al vuelo si h cambió; no usar en camino caliente).
    void step(double& y, double& v, double f, double hStep) const {
        if (hStep != h) {
            ExpOscillator tmp(omega, zeta, hStep);
            tmp.step(y, v, f);
            return;
        }
        step(y, v, f);
    }
};

// ── Banda coclear ERB (F1 (3.7)-(3.9)) ───────────────────────────────────
// y'' + 2 zeta_b w_b y' + w_b² y + alpha_b y³ = beta_b p_b(t) - a_b y'
// a_b' = (g_b |y'| - a_b)/tau_a ; n_b' = (max(0, g_b|y'| - a_b) - n_b)/tau_n.
// Unidades: y [m], alpha_b [m⁻²·s⁻²], beta_b [m·Pa⁻¹·s⁻²], a_b/n_b [s⁻¹],
// g_b [s⁻¹], tau [s]. El paso lineal es EXACTO ([C1]); el cúbico y la
// adaptación van por Strang/Euler (lentos frente a w_b, sin rigidez).
// f_s de trabajo del bloque coclear: 192 kHz (offline) para que el 3er
// armónico de 20 kHz (60 kHz) quede por debajo de Nyquist (96 kHz) [C2].
struct CochlearBand {
    double omega = 0.0;   // [rad·s⁻¹] centro de banda ERB
    double zeta = 0.05;   // [-]
    double alpha = 0.0;   // [m⁻²·s⁻²] rigidez cúbica (Duffing); 0 = lineal
    double beta = 1.0;    // [m·Pa⁻¹·s⁻²] acoplo presión -> basilar
    double g = 1.0;       // [s⁻¹] ganancia de adaptación
    double tauA = 0.05;   // [s] constante de adaptación
    double tauN = 0.01;   // [s] constante de tasa neural
    double y = 0.0, v = 0.0;  // [m], [m·s⁻¹] estado basilar
    double a = 0.0;       // [s⁻¹] adaptación
    double n = 0.0;       // [s⁻¹] tasa de disparo efectiva
    void prepare(double h) { osc_.init(omega, zeta, h); }
    void step(double pPa, double h) {
        if (osc_.h != h) prepare(h);
        // Medio paso no lineal (Strang): v -= h/2 (alpha y³ + a v).
        v -= 0.5 * h * (alpha * y * y * y + a * v);
        // Paso lineal exacto con fuerza constante beta·p.
        osc_.step(y, v, beta * pPa);
        // Medio paso no lineal.
        v -= 0.5 * h * (alpha * y * y * y + a * v);
        // Adaptación (Euler explícito; tau >> h, sin rigidez).
        a += h * (g * std::abs(v) - a) / tauA;
        n += h * (std::max(0.0, g * std::abs(v) - a) - n) / tauN;
    }

  private:
    ExpOscillator osc_;  // mapa lineal precomputado (paso exacto)
};

// ── Energía modal RE-DERIVADA [D3] ──────────────────────────────────────
// E_ac = Σ_k V/(2 rho0 c0²) (q_k² + w_k² Q_k²)  [J], Q_k = int q_k dt.
// Demostración del balance exacto (estados con Qdot = q):
//   A_k = qdot_k + w_k² Q_k  =>  dA_k/dt = qdd_k + w_k² q_k
//        = Gamma_k Udot - 2 zeta_k w_k qdot_k   (por la ODE modal).
//   Integrando desde condiciones cero: A_k = Gamma_k U_s - 2 zeta_k w_k q_k.
//   dE_k/dt = V/(rho0 c0²) q_k A_k
//           = phi_k(r_s) q_k U_s - (2 V zeta_k w_k/(rho0 c0²)) q_k².
//   Sumando: Σ phi_k q_k = p_s, luego Σ dE_k/dt
//           = p_s U_s - Σ_k (2 V zeta_k w_k/(rho0 c0²)) q_k².
//   Y la potencia mecánica que sale del transductor es v·F_rad =
//   v·S_d·p_s = U_s·p_s: CANCELA EXACTO. La disipación modal es
//   P_perd,k = (2 V zeta_k w_k/(rho0 c0²)) q_k² [W] (usa q², no qdot²;
//   en promedio sinusoidal coinciden: <q²> = <qdot²>/w²).
// La forma de F1 (5.2) con Gamma corregido es dimensionalmente correcta
// pero depende de phi_k(r_s): no es energía física y solo balancea en
// resonancia (w = w_k). NO se usa.
inline double modalEnergy(const std::vector<RoomMode>& modes,
                          const std::vector<double>& q,
                          const std::vector<double>& qq,
                          double volume = kRoomVolume) {
    double e = 0.0;
    for (std::size_t k = 0; k < modes.size(); ++k) {
        const double w = modes[k].omega;
        e += q[k] * q[k] + w * w * qq[k] * qq[k];
    }
    return volume / (2.0 * kRho0 * kC0 * kC0) * e;
}
inline double modalLossPower(const std::vector<RoomMode>& modes,
                             const std::vector<double>& q,
                             double volume = kRoomVolume) {
    double p = 0.0;
    for (std::size_t k = 0; k < modes.size(); ++k)
        p += 2.0 * modes[k].zeta * modes[k].omega * q[k] * q[k];
    return volume / (kRho0 * kC0 * kC0) * p;
}

// ── Identificación desde RIR (Sabine/Eyring) [R2] ───────────────────────
// Absorción media de Sabine (métrico): alpha_bar = 0.161·V/(S·T60) [-].
// Válida si alpha_bar << 1; con alpha_bar > 0.5 usar Eyring.
// Z_w (paredes) desde alpha (incidencia normal, Z real):
//   alpha = 1 - |(Z - rho0 c0)/(Z + rho0 c0)|²  =>
//   |Z_w| = rho0 c0 (1 + sqrt(1-alpha)) / (1 - sqrt(1-alpha)) [Pa·s·m⁻¹].
// Es una ESTIMACIÓN de orden de magnitud para identificar Z_w (F1 §12.1),
// no una medida de impedancia compleja por pared.
inline double sabineMeanAbsorption(double rt60, double volume,
                                   double surfaceArea) {
    return 0.161 * volume / (surfaceArea * rt60);  // [-]
}
inline double wallImpedanceFromAbsorption(double alpha) {
    const double s = std::sqrt(std::max(0.0, 1.0 - alpha));
    return kRho0 * kC0 * (1.0 + s) / (1.0 - s);  // [Pa·s·m⁻¹]
}

// ── Receptor estándar KEMAR y sistemas 2.1+ [R3] ────────────────────────
// La geometría 3D es un ESTÁNDAR PRECISO Y UNIVERSAL (cabeza tipo KEMAR),
// no un escaneo personalizado (aclaración de Esse 2026-11-26). El estado
// X del modelo es independiente del receptor: la presión en CUALQUIER
// punto es una lectura del mismo estado (F1 (2.2)-(2.3)). Para 2.1+ se
// evalúa p(r) en cada transductor/receptor (L, R, sub, multicanal) desde
// el mismo estado; el acoplo sala->transductor se evalúa por transductor
// con su propio r_s. Los oídos KEMAR están a ±earOffset del centro de la
// cabeza (radio de cabeza estándar 0.0875 m).
inline constexpr double kKemarEarOffsetM = 0.0875;  // [m] medio ancho KEMAR
inline constexpr double kKemarHeadRadiusM = 0.0875; // [m]

// ITD de Woodworth redescubierta desde KEMAR real [R3]: ITD(θ) =
// -1409·θ + 918.6·sin(θ) + 144.8·θ³ [µs], θ [rad] azimut (0 = frontal).
// Redescubierta por PySR desde 710 posiciones KEMAR reales
// (coordinador de élite 2026-11-26 ~20:03 CST): RMSE 5.52 µs,
// R² = 0.99987, radio efectivo 9.33 cm (vs KEMAR 8.75 cm). CANDIDATA:
// el signo sigue la convención del ajuste (negativa para θ>0);
// verificar contra el modelo ORBE antes de usarla como ley.
// Acuerdo con Woodworth teórico (r/c)(θ+sinθ) con r = 9.33 cm: <6%.
inline double kemarItdUs(double thetaRad) {
    const double t = thetaRad;
    return -1409.0 * t + 918.6 * std::sin(t) + 144.8 * t * t * t;  // [µs]
}
inline double woodworthItdUs(double thetaRad, double headRadiusM) {
    return 1e6 * (headRadiusM / kC0) * (thetaRad + std::sin(thetaRad));
}

// ── Simulador acoplado OFFLINE (RK4 a f_s,off) ──────────────────────────
// Estado X = [x, v, i, M, q_1, qdot_1, Q_1, ..., q_K, qdot_K, Q_K]
// (F1 (2.1), con Q_k añadido para la energía [D3]). Ecuaciones (3.1),
// (3.2) con F_rad = S_d p_s, (3.4) corregida [D1] con término Westervelt
// modal Pi_k[p²] vía el tensor C_ijk (perturbativo, O(beta); la
// segunda derivada temporal se evalúa EXACTA desde el estado con
//   d²(q_i q_j)/dt² = qdd_i q_j + 2 qdot_i qdot_j + q_i qdd_j
// usando qdd de la parte lineal — sin diferencias finitas ruidosas).
// El bloque modal (f < 139 Hz) y el transductor son exactos con RK4 a
// 48 kHz y más a 192 kHz (y <= 0.018 / 0.0045). La cóclea NO se integra
// aquí con RK4: va por ExpOscillator ([C1]), alimentada por p_l(t).
class CoupledSimulator {
  public:
    struct Config {
        TransducerParams transducer;
        RoomGeometry room{};     // sala (default Genezi 4.0x7.0x2.5 m)
        Vec3 rs{0.3, 0.3, 1.2};  // [m] fuente en esquina (G1, F2 §1 P-A)
        Vec3 rl{2.0, 3.5, 1.2};  // [m] oyente a 3.0 m en el eje
        double fs = kFsOffline;  // [Hz] 192 kHz por defecto [C2]
        bool westervelt = true;  // Pi_k[p²] activo
        bool jilesAtherton = false;  // M dinámica (P-D); off por defecto
        bool coupled = true;     // false = desacoplado (F_rad = 0):
                                 // control para R-G
    };

    explicit CoupledSimulator(Config cfg)
        : cfg_(cfg),
          modes_(roomModesFor(cfg_.room, cfg_.room.schroederHz())),
          couplings_(tripleCouplings(modes_)) {
        const std::size_t k = modes_.size();
        q_.assign(k, 0.0);
        qd_.assign(k, 0.0);
        qq_.assign(k, 0.0);
        gamma_.reserve(k);
        phiS_.reserve(k);
        phiL_.reserve(k);
        for (const RoomMode& m : modes_) {
            gamma_.push_back(m.gammaAt(cfg_.rs));
            phiS_.push_back(m.phiAt(cfg_.rs));
            phiL_.push_back(m.phiAt(cfg_.rl));
        }
    }

    std::size_t numModes() const { return modes_.size(); }
    const std::vector<RoomMode>& modes() const { return modes_; }
    double time() const { return t_; }
    double x() const { return x_; }
    double v() const { return v_; }
    double current() const { return i_; }
    double magnetisation() const { return ja_.m; }
    bool stiffnessClamped() const { return ja_.stiffnessClamped; }
    double pressureAtSource() const { return pressureFromPhi(phiS_); }
    double pressureAtListener() const { return pressureFromPhi(phiL_); }
    // Presión en un punto ARBITRARIO [Pa] (F1 (2.2)): el estado es único;
    // cada receptor (oídos KEMAR L/R, sub, multicanal 2.1+) es una
    // lectura del mismo estado. [R3]
    double pressureAt(const Vec3& r) const {
        double p = 0.0;
        for (std::size_t k = 0; k < modes_.size(); ++k)
            p += q_[k] * modes_[k].phiAt(r);
        return p;
    }
    // Proyección Pi_k[p²] del modo k [Pa²] (para el test de P-B2).
    double piP2(std::size_t k) const {
        double s = 0.0;
        for (const TripleCoupling& c : couplings_)
            if (c.k == k) s += c.c * q_[c.i] * q_[c.j];
        return s;
    }

    // Energía total [J] = E_em + E_ac (E_coch = 0 aquí: la cóclea es un
    // observador aparte; E_hys = 0 con J-A off o lineal).
    double totalEnergy() const {
        const TransducerParams& p = cfg_.transducer;
        const double eEm = 0.5 * p.le * i_ * i_ + 0.5 * p.mMs * v_ * v_ +
                           0.5 * p.km() * x_ * x_;
        return eEm + modalEnergy(modes_, q_, qq_, cfg_.room.volume());
    }
    // Potencia de entrada [W] = V_DAC · i.
    double powerIn(double uIn) const { return uIn * i_; }
    // Potencia disipada [W] = R_e i² + r_m v² + Σ P_perd,modal,k.
    // (zeta_k ya incluye las pérdidas en paredes vía T60: no hay término
    // P_dOmega separado; documentado en el comentario de totalEnergy.)
    double powerLoss() const {
        const TransducerParams& p = cfg_.transducer;
        return p.re * i_ * i_ + p.rM * v_ * v_ +
               modalLossPower(modes_, q_, cfg_.room.volume());
    }

    // Un paso RK4 con entrada uIn [V] constante en el paso (ZOH).
    // Buffers scratch reutilizados (sin alocación por paso: el offline
    // corre 192 000 pasos/s simulado y el test no puede pagar un
    // vector por etapa). Además acumula la integral de potencia con la
    // CUADRATURA DEL PROPIO RK4 (pesos 1,2,2,1/6 sobre los 4 estadios):
    // el balance discreto solo es exacto a O(h^5) con la cuadratura del
    // integrador; la regla trapezoidal sobre valores de borde es de
    // 2º orden y falsea el criterio 1e-6 en ~4e-3 (medido: el error
    // dominante era el ZOH de u(t) frente a la potencia muestreada, no
    // la física). Corrección documentada en CODIGO_D10.md.
    void step(double uIn) {
        const double h = 1.0 / cfg_.fs;
        packInto(s0_);
        rhs(s0_, uIn, k1_);
        stageInto(s0_, k1_, 0.5 * h, st1_);
        rhs(st1_, uIn, k2_);
        stageInto(s0_, k2_, 0.5 * h, st2_);
        rhs(st2_, uIn, k3_);
        stageInto(s0_, k3_, h, st3_);
        rhs(st3_, uIn, k4_);
        // Integral de potencia del paso, cuadratura RK4 (estadios 1-4).
        // Se evalúa ANTES de actualizar s0_: s0_ aún es el estadio 1.
        const double p1n = powerNet(uIn, s0_);
        const double p2n = powerNet(uIn, st1_);
        const double p3n = powerNet(uIn, st2_);
        const double p4n = powerNet(uIn, st3_);
        intNet_ += h / 6.0 * (p1n + 2.0 * p2n + 2.0 * p3n + p4n);
        const double q1n = powerInState(uIn, s0_);
        const double q2n = powerInState(uIn, st1_);
        const double q3n = powerInState(uIn, st2_);
        const double q4n = powerInState(uIn, st3_);
        intIn_ += h / 6.0 * (q1n + 2.0 * q2n + 2.0 * q3n + q4n);
        for (std::size_t n = 0; n < s0_.v.size(); ++n)
            s0_.v[n] += h / 6.0 * (k1_.v[n] + 2.0 * k2_.v[n] + 2.0 * k3_.v[n] +
                                  k4_.v[n]);
        unpack(s0_);
        // J-A se integra en H (no en t): un paso por muestra con i0->i1.
        if (cfg_.jilesAtherton) ja_.step(iPrev_, i_);
        iPrev_ = i_;
        t_ += h;
    }
    void resetPowerIntegrals() { intNet_ = 0.0; intIn_ = 0.0; }
    double powerIntegralNet() const { return intNet_; }  // [J] Σh(Pin-Pperd)
    double powerIntegralIn() const { return intIn_; }    // [J] Σh Pin

    // Corre con una señal uIn[n] y devuelve, por hop, el residuo de
    // balance discreto relativo: |ΔE - ∫(P_in - P_perd)| / |∫P_in|,
    // con la integral en cuadratura RK4 (ver step()). Criterio F2
    // §2.2.4 (mejorado): promediado por hop, solo exigible en
    // configuración lineal (sin Westervelt ni J-A ni cóclea).
    double maxHopBalanceError(const std::vector<double>& uIn,
                              std::size_t hopSamples) const {
        return maxHopBalanceErrorImpl(uIn, hopSamples);
    }

  private:
    struct State {
        std::vector<double> v;
    };
    Config cfg_;
    std::vector<RoomMode> modes_;
    std::vector<TripleCoupling> couplings_;
    std::vector<double> gamma_, phiS_, phiL_;
    double x_ = 0.0, v_ = 0.0, i_ = 0.0, iPrev_ = 0.0, t_ = 0.0;
    std::vector<double> q_, qd_, qq_;
    JilesAtherton ja_{cfg_.transducer};
    // Scratch (mutable por diseño: rhs/step son lógicamente const en
    // cuanto al estado físico, pero reutilizan buffers).
    mutable State s0_, k1_, k2_, k3_, k4_, st1_, st2_, st3_;
    mutable std::vector<double> qScr_, qdScr_, qddScr_, nlScr_;
    double intNet_ = 0.0, intIn_ = 0.0;  // [J] integrales del paso

    double pressureFromPhi(const std::vector<double>& phi) const {
        double p = 0.0;
        for (std::size_t k = 0; k < modes_.size(); ++k) p += q_[k] * phi[k];
        return p;
    }
    void packInto(State& s) const {
        s.v.resize(3 + 3 * modes_.size());
        s.v[0] = x_;
        s.v[1] = v_;
        s.v[2] = i_;
        for (std::size_t k = 0; k < modes_.size(); ++k) {
            s.v[3 + 3 * k] = q_[k];
            s.v[3 + 3 * k + 1] = qd_[k];
            s.v[3 + 3 * k + 2] = qq_[k];
        }
    }
    State pack() const {
        State s;
        packInto(s);
        return s;
    }
    void unpack(const State& s) {
        x_ = s.v[0];
        v_ = s.v[1];
        i_ = s.v[2];
        for (std::size_t k = 0; k < modes_.size(); ++k) {
            q_[k] = s.v[3 + 3 * k];
            qd_[k] = s.v[3 + 3 * k + 1];
            qq_[k] = s.v[3 + 3 * k + 2];
        }
    }
    void stageInto(const State& base, const State& k, double a,
                   State& out) const {
        out.v.resize(base.v.size());
        for (std::size_t n = 0; n < base.v.size(); ++n)
            out.v[n] = base.v[n] + a * k.v[n];
    }
    // Potencia de entrada [W] y neta [W] en un estado arbitrario.
    double powerInState(double uIn, const State& s) const {
        return uIn * s.v[2];
    }
    double powerLossState(const State& s) const {
        const TransducerParams& p = cfg_.transducer;
        double loss = p.re * s.v[2] * s.v[2] + p.rM * s.v[1] * s.v[1];
        for (std::size_t k = 0; k < modes_.size(); ++k) {
            const double q = s.v[3 + 3 * k];
            loss += 2.0 * cfg_.room.volume() * modes_[k].zeta *
                    modes_[k].omega / (kRho0 * kC0 * kC0) * q * q;
        }
        return loss;
    }
    double powerNet(double uIn, const State& s) const {
        return powerInState(uIn, s) - powerLossState(s);
    }
    // Lado derecho f(X, u) de (3.1)-(3.4) [D1]. Escribe en d (scratch).
    void rhs(const State& s, double uIn, State& d) const {
        const TransducerParams& p = cfg_.transducer;
        const std::size_t k = modes_.size();
        const double x = s.v[0], v = s.v[1], i = s.v[2];
        qScr_.resize(k);
        qdScr_.resize(k);
        for (std::size_t m = 0; m < k; ++m) {
            qScr_[m] = s.v[3 + 3 * m];
            qdScr_[m] = s.v[3 + 3 * m + 1];
        }
        // Presión en la fuente y fuerza de radiación (3.3).
        double pS = 0.0;
        for (std::size_t m = 0; m < k; ++m) pS += qScr_[m] * phiS_[m];
        const double fRad = cfg_.coupled ? p.sd * pS : 0.0;
        const double kt = cfg_.jilesAtherton ? ja_.kt() : p.kt0;
        const double ke = cfg_.jilesAtherton ? ja_.ke() : p.ke0;
        // (3.1): L di/dt = V - R i - K_e v. (3.2): m dv/dt = K_t i - r v
        // - k x - F_rad. (F_hys = 0 en esta implementación: la histéresis
        // entra por K_t(M)/K_e(M); la fuerza histerética mecánica explícita
        // es trabajo de Fase 2-bis, registrado, no inventado.)
        d.v.resize(s.v.size());
        d.v[0] = v;
        d.v[1] = (kt * i - p.rM * v - p.km() * x - fRad) / p.mMs;
        d.v[2] = (uIn - p.re * i - ke * v) / p.le;
        // Parte lineal modal: qdd_lin = -2 zeta w qd - w² q + Gamma Udot,
        // Udot = S_d dv/dt.
        const double uDot = p.sd * d.v[1];
        qddScr_.assign(k, 0.0);
        for (std::size_t m = 0; m < k; ++m) {
            const RoomMode& md = modes_[m];
            qddScr_[m] = -2.0 * md.zeta * md.omega * qdScr_[m] -
                         md.omega * md.omega * qScr_[m] + gamma_[m] * uDot;
        }
        // Término Westervelt modal (perturbativo, O(beta)):
        //   nl_k = (beta/(rho0 c0²)) d²/dt² Pi_k[p²],
        //   d²(q_i q_j)/dt² = qdd_i q_j + 2 qdot_i qdot_j + q_i qdd_j.
        nlScr_.assign(k, 0.0);
        if (cfg_.westervelt) {
            const double coef = kBetaAir / (kRho0 * kC0 * kC0);  // [Pa⁻¹]
            for (const TripleCoupling& c : couplings_)
                nlScr_[c.k] += coef * c.c *
                               (qddScr_[c.i] * qScr_[c.j] +
                                2.0 * qdScr_[c.i] * qdScr_[c.j] +
                                qScr_[c.i] * qddScr_[c.j]);
        }
        for (std::size_t m = 0; m < k; ++m) {
            d.v[3 + 3 * m] = qdScr_[m];              // dq/dt
            d.v[3 + 3 * m + 1] = qddScr_[m] + nlScr_[m];  // dqdot/dt
            d.v[3 + 3 * m + 2] = qScr_[m];            // dQ/dt = q
        }
    }
    double maxHopBalanceErrorImpl(const std::vector<double>& uIn,
                                  std::size_t hopSamples) const {
        // Copia mutable: el balance se mide sobre una corrida fresca.
        CoupledSimulator sim = *this;
        double worst = 0.0;
        double ePrev = sim.totalEnergy();
        std::size_t n = 0;
        sim.resetPowerIntegrals();
        for (double u : uIn) {
            sim.step(u);
            if (++n % hopSamples == 0) {
                const double eNow = sim.totalEnergy();
                const double denom =
                    std::max(std::abs(sim.powerIntegralIn()), 1e-30);
                worst = std::max(worst,
                                 std::abs((eNow - ePrev) -
                                          sim.powerIntegralNet()) /
                                     denom);
                ePrev = eNow;
                sim.resetPowerIntegrals();
            }
        }
        return worst;
    }
};

// ── §RT — CORRECCIONES IDENTIFICADAS PARA TIEMPO REAL ───────────────────
// El modelo completo (RK4 offline) NO va al hilo de audio (F2 §3.4:
// 7.8-12.8 GFLOPS ingenuo, 5.2-8.5 ms/hop vs presupuesto 2.67 ms/hop,
// útil < 1.33 ms/hop). Lo que va en tiempo real son solo estas tres
// correcciones identificadas: tablas + 2-3 multiplicaciones por muestra.
// NO tocan las muestras de audio: alimentan el observador del
// transductor (D10 / gemelo) y la telemetría. Su efecto audible es
// CERO por diseño hasta que Fase 3 identifique los parámetros y Esse
// ordene lo contrario; son instrumentación, no efecto.

// RT-1. Realimentación modal truncada (F2 §3.4, fila "tensor truncado"):
// K_rt <= 10 modos dominantes (los de mayor |Gamma_k phi_k(r_s)|, i.e.
// mayor acoplo fuente->modo), cada uno un resonador exacto
// (ExpOscillator, sin error de fase) a 48 kHz. Salida: F_rad,rt [N] =
// S_d Σ q_k phi_k(r_s). Coste medido en el test (hop 128 @ 48 kHz).
struct ModalFeedbackRt {
    struct ModeRt {
        ExpOscillator osc;  // [rad·s⁻¹, -]
        double gamma = 0.0; // [Pa·m⁻³] Gamma_k
        double phiS = 0.0;  // [-] phi_k(r_s)
        double q = 0.0, qd = 0.0;  // [Pa], [Pa·s⁻¹]
    };
    std::vector<ModeRt> modes;
    double sd = 0.020;  // [m²]

    ModalFeedbackRt() = default;
    ModalFeedbackRt(const std::vector<RoomMode>& allModes, const Vec3& rs,
                    const TransducerParams& p, std::size_t kRt = 10) {
        sd = p.sd;
        std::vector<std::pair<double, const RoomMode*>> ranked;
        for (const RoomMode& m : allModes)
            ranked.emplace_back(std::abs(m.gammaAt(rs) * m.phiAt(rs)), &m);
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        const std::size_t n = std::min(kRt, ranked.size());
        modes.reserve(n);
        const double h = 1.0 / kFsAudio;
        for (std::size_t i = 0; i < n; ++i) {
            ModeRt mr;
            mr.osc.init(ranked[i].second->omega, ranked[i].second->zeta, h);
            mr.gamma = ranked[i].second->gammaAt(rs);
            mr.phiS = ranked[i].second->phiAt(rs);
            modes.push_back(mr);
        }
    }
    // Un paso a 48 kHz: Udot [m³·s⁻²] -> F_rad [N]. La fuerza modal
    // Gamma_k·Udot entra como aceleración del oscilador (f = Gamma·Udot
    // con y = q, unidades Pa·s⁻²); el paso es exacto para la parte
    // lineal, así que no hay deriva de fase a 48 kHz (f < 139 Hz).
    double step(double uDot) {
        double pS = 0.0;
        for (ModeRt& m : modes) {
            m.osc.step(m.q, m.qd, m.gamma * uDot);
            pS += m.q * m.phiS;
        }
        return sd * pS;
    }
};

// RT-2. Estimador Westervelt: predice L_2f [dB SPL] desde el fundamental
// medido (p1_rms [Pa], f [Hz], z [m]) por la ley (P-B.1). 2-3 flops.
struct WesterveltEstimatorRt {
    double predictL2fDb(double p1RmsPa, double freqHz, double zM,
                        double beta = kBetaAir) const {
        const double k =
            2.0 * 3.14159265358979323846 * freqHz / kC0;  // [rad·m⁻¹]
        const double ratio = beta * k * zM * p1RmsPa / (2.0 * kRho0 * kC0 * kC0);
        const double l1 = 20.0 * std::log10(p1RmsPa / kRefPressurePa);
        return l1 + 20.0 * std::log10(ratio);
    }
};

// RT-3. Trim J-A efectivo: K_t,eff = K_t0 (1 + chi_M M/M_s) actualizado
// a la tasa lenta del sensor (tau_Z 1-5 s, F1 (3.13)), no por muestra.
// Por muestra: 1 multiplicación. M se estima por el observador lento.
struct JaTrimRt {
    double chiM = 0.05;    // [-] UNIDENTIFIED (rango 0.03-0.08)
    double mOverMs = 0.0;  // [-] M/M_s estimado por el observador lento
    double ktFactor() const { return 1.0 + chiM * mOverMs; }  // [-]
};

}  // namespace ivanna::fused::coupled
