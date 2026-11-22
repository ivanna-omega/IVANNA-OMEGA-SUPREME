// thd_analyze.cpp — Herramienta host para analizar grabaciones del teléfono.
// Lee un WAV PCM 16-bit (mono o estéreo; usa el canal izquierdo) y reporta
// THD+N, niveles y clipping con la misma matemática de ThdMeter.hpp
// (propuesta 0022, Punto 3 de la crítica externa).
// Uso: thd_analyze <archivo.wav> <frecuencia_fundamental_Hz> [tasa_muestreo]
//   Ejemplo: thd_analyze grabacion.wav 1000
// La frecuencia debe tener un número entero de ciclos en el buffer analizado;
// la herramienta recorta el buffer al mayor número entero de ciclos.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dsp/ThdMeter.hpp"

static bool readWav16(const char* path, std::vector<float>& out, int& sampleRate,
                      int& channels) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    auto readU32 = [&]() -> std::uint32_t {
        std::uint8_t b[4];
        if (std::fread(b, 1, 4, f) != 4) return 0;
        return (std::uint32_t)b[0] | ((std::uint32_t)b[1] << 8) |
               ((std::uint32_t)b[2] << 16) | ((std::uint32_t)b[3] << 24);
    };
    auto readU16 = [&]() -> std::uint16_t {
        std::uint8_t b[2];
        if (std::fread(b, 1, 2, f) != 2) return 0;
        return (std::uint16_t)(b[0] | (b[1] << 8));
    };
    char riff[4];
    if (std::fread(riff, 1, 4, f) != 4 || std::memcmp(riff, "RIFF", 4) != 0) {
        std::fclose(f);
        return false;
    }
    readU32();  // tamaño RIFF
    char wave[4];
    if (std::fread(wave, 1, 4, f) != 4 || std::memcmp(wave, "WAVE", 4) != 0) {
        std::fclose(f);
        return false;
    }
    int fmtChannels = 0, fmtRate = 0, fmtBits = 0, fmtAudio = 0;
    std::uint32_t dataBytes = 0;
    long dataPos = -1;
    while (!std::feof(f)) {
        char id[4];
        if (std::fread(id, 1, 4, f) != 4) break;
        const std::uint32_t sz = readU32();
        if (std::memcmp(id, "fmt ", 4) == 0) {
            fmtAudio = readU16();
            fmtChannels = readU16();
            fmtRate = (int)readU32();
            readU32();  // byte rate
            readU16();  // block align
            fmtBits = readU16();
            if (sz > 16) std::fseek(f, (long)(sz - 16), SEEK_CUR);
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataBytes = sz;
            dataPos = std::ftell(f);
            break;
        } else {
            std::fseek(f, (long)sz, SEEK_CUR);
        }
    }
    if (dataPos < 0 || fmtAudio != 1 || fmtBits != 16 || fmtChannels < 1 || fmtRate <= 0) {
        std::fclose(f);
        return false;
    }
    std::fseek(f, dataPos, SEEK_SET);
    const std::size_t frames = dataBytes / (2u * (std::size_t)fmtChannels);
    out.resize(frames);
    std::vector<std::int16_t> raw(frames * (std::size_t)fmtChannels);
    if (std::fread(raw.data(), 2, raw.size(), f) != raw.size()) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    for (std::size_t i = 0; i < frames; ++i)
        out[i] = (float)raw[i * (std::size_t)fmtChannels] / 32768.0f;
    sampleRate = fmtRate;
    channels = fmtChannels;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "Uso: %s <archivo.wav> <frecuencia_Hz> [tasa_muestreo]\n",
                     argv[0]);
        return 2;
    }
    const double freq = std::atof(argv[2]);
    std::vector<float> x;
    int sr = 0, ch = 0;
    if (!readWav16(argv[1], x, sr, ch)) {
        std::fprintf(stderr, "No se pudo leer WAV PCM 16-bit: %s\n", argv[1]);
        return 1;
    }
    if (argc >= 4) sr = std::atoi(argv[3]);
    // Recortar al mayor número entero de ciclos de la fundamental.
    const double cycles = (double)x.size() * freq / (double)sr;
    const std::size_t whole = (std::size_t)std::floor(cycles);
    if (whole < 1) {
        std::fprintf(stderr, "El buffer no contiene ni un ciclo completo.\n");
        return 1;
    }
    const std::size_t useFrames =
        (std::size_t)std::floor((double)whole * (double)sr / freq);
    const auto r = ivanna::measure::measureThdPlusNoise(
        x.data(), useFrames, (double)sr, freq);
    std::printf("archivo=%s canales=%d tasa=%d Hz fundamental=%.1f Hz ciclos=%zu frames=%zu\n",
                argv[1], ch, sr, freq, whole, useFrames);
    if (!r.valid) {
        std::printf("sin fundamental medible (silencio o tono ausente)\n");
        return 0;
    }
    std::printf("THD+N=%.2f dB (%.4f %%) fundamental=%.2f dBFS total=%.2f dBFS "
                "pico=%.2f dBFS rms=%.2f dBFS clipped=%u\n",
                r.thdPlusNoiseDb, r.thdPercent, r.fundamentalDbfs, r.totalDbfs,
                r.peakDbfs, r.rmsDbfs, r.clippedSamples);
    return 0;
}
