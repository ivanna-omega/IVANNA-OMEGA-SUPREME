#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Ivanna {

struct HRTFIR {
    std::vector<float> left;
    std::vector<float> right;
    float sampleRate = 48000.0f;
};

class SofaHRTFLoader {
public:
    // FIX: antes load() solo devolvía bool — el caller no podía distinguir
    // "archivo no encontrado", "firma HDF5 incorrecta" y "SOFA válido pero
    // no parseado en runtime". Ahora lastStatus() da el diagnóstico exacto.
    enum class Status {
        NONE,               // No se ha llamado a load() aún
        FILE_NOT_FOUND,     // El path no existe o no hay permisos de lectura
        CORRUPT,            // Tamaño insuficiente o error de I/O en cabecera
        NOT_HDF5,           // El archivo existe pero no tiene firma HDF5 válida
        VALID_NOT_PARSED,   // Firma HDF5 OK pero el contenido NO se pudo parsear
                            // (estructura inesperada, chunk corrupto, filtro no
                            // soportado...). HRIR reconstruido desde la base
                            // maestra SOFA-PCA (ver load()).
        UNSUPPORTED,        // Firma HDF5 OK pero formato fuera del alcance del
                            // lector mínimo (superbloque HDF5 v1/v3, layout
                            // contiguo, datatype no-float...). HRIR reconstruido
                            // desde la base maestra SOFA-PCA (fail-closed).
                            // v0 SÍ se lee desde evol-hrtf (SOFA del módulo):
                            // solo v1/v3 quedan fuera de alcance.
        PARSED_MEASURED,    // Data.IR + SourcePosition + Data.SamplingRate
                            // leídos DE VERDAD del archivo (lector HDF5 mínimo,
                            // v2.5.472 Fase 2 ítem D). hrtf() es el par frontal
                            // medido y hrtfAt() selecciona por azimut/elevación.
    };

    bool   load(const std::string& path);
    bool   loadCached(const std::string& path);
    Status lastStatus() const noexcept { return lastStatus_; }

    // true cuando el HRIR de hrtf() fue RECONSTRUIDO desde la base maestra SOFA-PCA y no leído
    // del archivo. load()==true solo prueba que el archivo empieza con la firma HDF5; el "sujeto"
    // se elige buscando su id en la RUTA, no en el contenido. Quien registre o muestre esta HRTF
    // no debe presentarla como una medición del archivo.
    bool isReconstructedFromMasterBasis() const noexcept {
        return lastStatus_ == Status::VALID_NOT_PARSED ||
               lastStatus_ == Status::UNSUPPORTED;
    }

    // true solo cuando Data.IR se leyó de verdad del archivo (Status::PARSED_MEASURED).
    bool isMeasured() const noexcept { return lastStatus_ == Status::PARSED_MEASURED; }

    // Par frontal medido (azimut 0, elevación 0) cuando isMeasured(); par reconstruido
    // SOFA-PCA (128 taps, 48 kHz) en caso contrario. NUNCA un par inventado como medido.
    const HRTFIR& hrtf() const { return m_hrtf; }

    // ── Datos medidos (solo válidos cuando isMeasured()) ─────────────────────
    // HRIR medido de la posición más cercana a (azimuthDeg, elevationDeg), distancia
    // angular sobre la esfera (mismo criterio que HrirPoseSelect::nearestHrirIndex).
    // out queda con los taps nativos del archivo a su sample rate nativo (sin
    // remuestrear: el llamador — HrtfManager — remuestrea a la SR de la sesión).
    // false si no hay dataset medido o la pose no es finita (out intacto).
    bool hrtfAt(float azimuthDeg, float elevationDeg, HRTFIR& out) const;

    // Posición de medición i: azimut (grados), elevación (grados), radio (m).
    // false si i fuera de rango o no hay dataset medido.
    bool sourcePosition(size_t i, float& azimuthDeg, float& elevationDeg,
                        float& radiusM) const noexcept;

    size_t numPositions() const noexcept { return m_numPos; }
    size_t tapsPerEar() const noexcept { return m_taps; }
    float  sampleRateHz() const noexcept { return m_sampleRateHz; }
    const std::string& path() const noexcept { return m_path; }

    // ── Telemetría P1+P4 (2026-11-26): qué camino se usó, en vivo ───────
    // Contadores GLOBALES (todas las instancias) de load(): cuántas
    // cargas leyeron Data.IR de verdad (measured), cuántas cayeron al
    // fallback PCA (reconstructed) y cuántas fallaron (failed: archivo
    // ausente, corrupto o sin firma HDF5). loadCached() en caliente NO
    // incrementa (no hay carga real). Con esto la app / el daemon
    // pueden reportar en vivo si la HRTF que suena es MEDIDA o
    // RECONSTRUIDA — antes solo lastStatus() por instancia lo decía y
    // nadie lo agregaba. Los LOGI/LOGE de load() ya distinguen el
    // camino por carga en logcat; estos contadores son el agregado.
    struct LoadCounters {
        std::size_t measured = 0;
        std::size_t reconstructed = 0;
        std::size_t failed = 0;
    };
    static LoadCounters loadCounters() noexcept;
    static void resetLoadCountersForTest() noexcept;

private:
    // Intento de parseo real (lector HDF5 mínimo). true = PARSED_MEASURED y
    // m_ir/m_srcPos/m_taps/m_sampleRateHz poblados; false = no se pudo leer
    // (reason distingue UNSUPPORTED de VALID_NOT_PARSED). NO toca m_hrtf.
    bool parseMeasured(const std::vector<unsigned char>& file, bool& unsupported);
    // Reconstrucción SOFA-PCA (comportamiento histórico, fail-closed).
    void reconstructFromPca(const std::string& path);

    HRTFIR m_hrtf;
    Status lastStatus_ = Status::NONE;
    std::string m_path;

    // Dataset medido completo (solo cuando isMeasured()): HRIRs float32 en
    // orden [pos][oído L=0/R=1][tap], posiciones [pos][az, el, r].
    std::vector<float> m_ir;
    std::vector<float> m_srcPos;
    size_t m_numPos = 0;
    size_t m_taps = 0;
    float  m_sampleRateHz = 0.0f;
};

} // namespace Ivanna
