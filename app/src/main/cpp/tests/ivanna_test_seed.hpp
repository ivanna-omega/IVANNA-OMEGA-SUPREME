// ivanna_test_seed.hpp — Contrato de determinismo C10 (release 510, Fase 4).
//
// Semilla global única de los tests: IVANNA_TEST_SEED (default fijo
// 1795857152818 — id numérico del perfil vivo; propuesta C10, no dato
// medido). Cada generador de azar de los tests deriva su flujo de esta
// semilla con IvannaTestSeedFor(sitio): mismo comando + misma semilla =
// mismo resultado. Sin IVANNA_TEST_SEED se usa el default; el valor
// histórico de cada sitio NO se conserva: el default ES la semilla.
//
// Excepción documentada: test_p0_reference_lock.cpp NO usa esta semilla:
// sus valores de referencia congelados (p0_reference_values.hpp) SON la
// semilla — cambiarla invalidaría el lock por diseño. Ese archivo mantiene
// su XorShift64 con semilla fija y no se cablea.
//
// Uso en un test que genera azar:
//   #include "ivanna_test_seed.hpp"
//   namespace { [[maybe_unused]] const bool kIvannaSeedLogged =
//       [] { ivanna::test::PrintIvannaTestSeed(); return true; }(); }
//   std::mt19937 g(ivanna::test::IvannaTestSeed32(1));  // sitio 1
//   std::srand(ivanna::test::IvannaTestSeed32(2));      // sitio 2
//
// Solo para tests (host). No incluir en código de producción: el motor
// no lee variables de entorno para sembrar nada.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace ivanna::test {

// Default fijo de la propuesta C10 (id numérico del perfil vivo).
inline constexpr std::uint64_t kIvannaTestSeedDefault = 1795857152818ULL;

// Semilla global: IVANNA_TEST_SEED si está fijada y es un entero decimal
// válido, si no el default. Se lee una sola vez por binario.
inline std::uint64_t IvannaTestSeed() {
    static const std::uint64_t kSeed = [] {
        const char* e = std::getenv("IVANNA_TEST_SEED");
        if (e != nullptr && *e != '\0') {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(e, &end, 10);
            if (end != e && *end == '\0') return static_cast<std::uint64_t>(v);
        }
        return kIvannaTestSeedDefault;
    }();
    return kSeed;
}

// Deriva la semilla de un sitio (1, 2, 3… por archivo) a partir de la
// global: cada generador tiene su propio flujo sin compartir estado.
// Mezcla splitmix64: dos sitios distintos nunca colisionan en la práctica.
inline std::uint64_t IvannaTestSeedFor(std::uint64_t site) {
    std::uint64_t s = IvannaTestSeed() + 0x9E3779B97F4A7C15ULL * (site + 1);
    s = (s ^ (s >> 30)) * 0xBF58476D1CE4E5B9ULL;
    s = (s ^ (s >> 27)) * 0x94D049BB133111EBULL;
    return s ^ (s >> 31);
}

// Variante de 32 bits para std::mt19937 / std::srand.
inline std::uint32_t IvannaTestSeed32(std::uint64_t site) {
    const std::uint64_t s = IvannaTestSeedFor(site);
    return static_cast<std::uint32_t>(s >> 32) ^ static_cast<std::uint32_t>(s);
}

// Imprime la línea `seed=…` al inicio del log (una vez por binario).
// Llamar desde un inicializador estático del test para que salga antes
// que la salida de gtest.
inline void PrintIvannaTestSeed() {
    static const bool kPrinted = [] {
        std::printf("[ivanna-test-seed] seed=%llu\n",
                    static_cast<unsigned long long>(IvannaTestSeed()));
        std::fflush(stdout);
        return true;
    }();
    (void)kPrinted;
}

}  // namespace ivanna::test
