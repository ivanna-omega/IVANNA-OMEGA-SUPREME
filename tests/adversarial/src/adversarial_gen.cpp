// adversarial_gen.cpp — CLI del generador de estímulos del banco adversarial.
// Genera WAV 48 kHz 16-bit para cada caso, y transformadas (delay/ganancia/
// inversión) para probar el harness con salidas de DUT conocidas.
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "adversarial_core.hpp"

namespace {
using namespace adv;
namespace fs = std::filesystem;

void usage(const char* prog) {
    std::cerr << "Uso:\n"
              << "  " << prog << " list\n"
              << "  " << prog
              << " generate <caso> <salida.wav> [--seed N] [--variant mono|stereo|seed]\n"
              << "  " << prog << " all <dir> [--seed N] [--variant mono|stereo|seed]\n"
              << "  " << prog
              << " transform <in.wav> <out.wav> [--delay N] [--gain-db X] [--invert]\n";
}

struct GenFlags {
    uint32_t seed = 0;
    Variant variant = Variant::Mono;
};

GenFlags parse_gen_flags(int argc, char** argv, int start) {
    GenFlags f;
    for (int i = start; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) {
            f.seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (a == "--variant" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "mono")
                f.variant = Variant::Mono;
            else if (v == "stereo")
                f.variant = Variant::Stereo;
            else if (v == "seed")
                f.variant = Variant::Seed;
            else
                throw std::runtime_error("variante desconocida: " + v);
        } else {
            throw std::runtime_error("opción desconocida: " + a);
        }
    }
    return f;
}

Signal transform_signal(const Signal& in, long delay, double gain_db, bool invert) {
    Signal out = in;
    const double g = amp_from_db(gain_db) * (invert ? -1.0 : 1.0);
    std::vector<double> nl(in.l.size(), 0.0), nr(in.r.size(), 0.0);
    for (size_t n = 0; n < in.l.size(); ++n) {
        const long src = static_cast<long>(n) - delay;
        if (src >= 0 && static_cast<size_t>(src) < in.l.size()) {
            nl[n] = in.l[static_cast<size_t>(src)] * g;
            nr[n] = in.r[static_cast<size_t>(src)] * g;
        }
    }
    out.l = std::move(nl);
    out.r = std::move(nr);
    return out;
}

int cmd_list() {
    for (const auto& id : case_ids()) {
        std::cout << id << "\t" << case_duration_s(id) << " s\t" << case_description(id)
                  << "\n";
    }
    return 0;
}

int cmd_generate(const std::string& id, const std::string& out, const GenFlags& f) {
    GenOptions opt;
    opt.seed = f.seed;
    opt.variant = f.variant;
    Signal s = generate_case(id, opt);
    write_wav(out, s);
    std::cout << "generate " << id << " -> " << out << " (" << s.frames() << " frames, "
              << s.duration_s() << " s)\n";
    return 0;
}

int cmd_all(const std::string& dir, const GenFlags& f) {
    fs::create_directories(dir);
    int count = 0;
    for (const auto& id : case_ids()) {
        GenOptions opt;
        opt.seed = f.seed;
        opt.variant = f.variant;
        Signal s = generate_case(id, opt);
        const std::string path = (fs::path(dir) / (id + ".wav")).string();
        write_wav(path, s);
        ++count;
    }
    std::cout << "all: " << count << " casos en " << dir << "\n";
    return 0;
}

int cmd_transform(const std::string& in, const std::string& out, int argc, char** argv,
                  int start) {
    long delay = 0;
    double gain_db = 0.0;
    bool invert = false;
    for (int i = start; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--delay" && i + 1 < argc)
            delay = std::stol(argv[++i]);
        else if (a == "--gain-db" && i + 1 < argc)
            gain_db = std::stod(argv[++i]);
        else if (a == "--invert")
            invert = true;
        else
            throw std::runtime_error("opción desconocida: " + a);
    }
    Signal s = read_wav(in);
    Signal t = transform_signal(s, delay, gain_db, invert);
    write_wav(out, t);
    std::cout << "transform " << in << " -> " << out << " (delay=" << delay
              << " gain_db=" << gain_db << " invert=" << (invert ? "si" : "no") << ")\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage(argv[0]);
            return 2;
        }
        const std::string cmd = argv[1];
        if (cmd == "list") return cmd_list();
        if (cmd == "generate") {
            if (argc < 4) {
                usage(argv[0]);
                return 2;
            }
            return cmd_generate(argv[2], argv[3], parse_gen_flags(argc, argv, 4));
        }
        if (cmd == "all") {
            if (argc < 3) {
                usage(argv[0]);
                return 2;
            }
            return cmd_all(argv[2], parse_gen_flags(argc, argv, 3));
        }
        if (cmd == "transform") {
            if (argc < 4) {
                usage(argv[0]);
                return 2;
            }
            return cmd_transform(argv[2], argv[3], argc, argv, 4);
        }
        usage(argv[0]);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
