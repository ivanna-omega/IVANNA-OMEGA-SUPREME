// wav.hpp — Lectura/escritura WAV PCM 16-bit a 48 kHz para el banco adversarial.
// Sin dependencias externas. Solo PCM little-endian, 1 o 2 canales, 16 bits.
// El banco trabaja SIEMPRE a 48 kHz (la tasa del motor); cualquier otra tasa es error.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace adv {

constexpr double kFs = 48000.0;  // frecuencia de muestreo del banco (Hz)
constexpr int kHop = 128;        // hop del motor (muestras) — unidad de reconvergencia
constexpr int kFft = 512;        // FFT del motor (muestras)

// Señal estéreo en doble precisión. Rango nominal [-1, 1]; los casos OOD pueden
// excederlo ANTES de cuantizar (la escritura recorta a [-1, 1] como un DAC real).
struct Signal {
    std::vector<double> l, r;
    int channels = 2;  // 1 (r == l) o 2
    double sampleRate = kFs;
    size_t frames() const { return l.size(); }
    double duration_s() const {
        return sampleRate > 0.0 ? static_cast<double>(l.size()) / sampleRate : 0.0;
    }
};

namespace detail {

inline void write_u16(std::ofstream& f, uint16_t v) {
    f.put(static_cast<char>(v & 0xFF));
    f.put(static_cast<char>((v >> 8) & 0xFF));
}

inline void write_u32(std::ofstream& f, uint32_t v) {
    for (int i = 0; i < 4; ++i) f.put(static_cast<char>((v >> (8 * i)) & 0xFF));
}

inline uint16_t read_u16(std::ifstream& f) {
    uint8_t b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    if (f.gcount() != 2) throw std::runtime_error("WAV truncado (u16)");
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
}

inline uint32_t read_u32(std::ifstream& f) {
    uint8_t b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    if (f.gcount() != 4) throw std::runtime_error("WAV truncado (u32)");
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

inline std::string read_tag(std::ifstream& f) {
    char t[4];
    f.read(t, 4);
    if (f.gcount() != 4) throw std::runtime_error("WAV truncado (tag)");
    return std::string(t, 4);
}

}  // namespace detail

inline void write_wav(const std::string& path, const Signal& s) {
    if (s.l.size() != s.r.size())
        throw std::runtime_error("write_wav: canales L/R de distinta longitud");
    if (s.sampleRate != kFs)
        throw std::runtime_error("write_wav: el banco solo escribe a 48 kHz");
    if (s.channels != 1 && s.channels != 2)
        throw std::runtime_error("write_wav: channels debe ser 1 o 2");

    const uint32_t n = static_cast<uint32_t>(s.l.size());
    const uint32_t ch = static_cast<uint32_t>(s.channels);
    const uint32_t data_bytes = n * ch * sizeof(int16_t);

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("write_wav: no se pudo abrir " + path);
    f.write("RIFF", 4);
    detail::write_u32(f, 36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    detail::write_u32(f, 16);
    detail::write_u16(f, 1);  // PCM
    detail::write_u16(f, static_cast<uint16_t>(ch));
    detail::write_u32(f, 48000);
    detail::write_u32(f, 48000 * ch * 2);
    detail::write_u16(f, static_cast<uint16_t>(ch * 2));  // block align
    detail::write_u16(f, 16);
    f.write("data", 4);
    detail::write_u32(f, data_bytes);

    auto q16 = [](double v) {
        v = std::clamp(v, -1.0, 1.0);
        return static_cast<int16_t>(std::lrint(v * 32767.0));
    };
    for (uint32_t i = 0; i < n; ++i) {
        detail::write_u16(f, static_cast<uint16_t>(q16(s.l[i])));
        if (ch == 2) detail::write_u16(f, static_cast<uint16_t>(q16(s.r[i])));
    }
    if (!f) throw std::runtime_error("write_wav: error al escribir " + path);
}

inline Signal read_wav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("read_wav: no se pudo abrir " + path);
    if (detail::read_tag(f) != "RIFF") throw std::runtime_error("read_wav: no es RIFF");
    (void)detail::read_u32(f);  // tamaño total
    if (detail::read_tag(f) != "WAVE") throw std::runtime_error("read_wav: no es WAVE");

    uint16_t audio_format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    bool have_fmt = false, have_data = false;
    std::vector<int16_t> pcm;

    while (f && !have_data) {
        std::string tag = detail::read_tag(f);
        uint32_t size = detail::read_u32(f);
        if (tag == "fmt ") {
            audio_format = detail::read_u16(f);
            channels = detail::read_u16(f);
            sample_rate = detail::read_u32(f);
            (void)detail::read_u32(f);  // byte rate
            (void)detail::read_u16(f);  // block align
            bits = detail::read_u16(f);
            if (size > 16) f.seekg(static_cast<std::streamoff>(size - 16), std::ios::cur);
            have_fmt = true;
        } else if (tag == "data") {
            if (!have_fmt) throw std::runtime_error("read_wav: data antes de fmt");
            if (audio_format != 1) throw std::runtime_error("read_wav: solo PCM (format 1)");
            if (bits != 16) throw std::runtime_error("read_wav: solo 16 bits");
            if (channels != 1 && channels != 2)
                throw std::runtime_error("read_wav: solo 1 o 2 canales");
            if (sample_rate != 48000)
                throw std::runtime_error("read_wav: solo 48 kHz (tasa del motor)");
            if (size % (2 * channels) != 0)
                throw std::runtime_error("read_wav: data con tamaño no múltiplo del frame");
            const size_t frames = size / (2 * channels);
            pcm.resize(frames * channels);
            f.read(reinterpret_cast<char*>(pcm.data()),
                   static_cast<std::streamsize>(size));
            if (static_cast<size_t>(f.gcount()) != size)
                throw std::runtime_error("read_wav: data truncada");
            have_data = true;
        } else {
            // Chunk desconocido (LIST, fact, ...): saltarlo (par, con pad si es impar).
            std::streamoff skip = static_cast<std::streamoff>(size + (size & 1u));
            f.seekg(skip, std::ios::cur);
            if (!f) throw std::runtime_error("read_wav: chunk desconocido truncado");
        }
    }
    if (!have_data) throw std::runtime_error("read_wav: sin chunk data");

    Signal s;
    s.channels = channels;
    s.sampleRate = 48000.0;
    const size_t frames = pcm.size() / channels;
    s.l.resize(frames);
    s.r.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        s.l[i] = static_cast<double>(pcm[i * channels]) / 32768.0;
        s.r[i] = (channels == 2)
                     ? static_cast<double>(pcm[i * channels + 1]) / 32768.0
                     : s.l[i];
    }
    return s;
}

}  // namespace adv
