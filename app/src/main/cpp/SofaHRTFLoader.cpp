// SofaHRTFLoader.cpp — v2.5.472 Fase 2 ítem D: Data.IR se lee DE VERDAD.
//
// Lector HDF5 MÍNIMO (sin dependencias externas) para los SOFA del catálogo
// (netCDF4 / HDF5 1.10, superbloque v2). Decodificado empíricamente byte a
// byte contra h5py y validado exacto (np.array_equal) en 5 archivos:
//   MIT_KEMAR_normal_pinna.sofa, MIT_KEMAR_large_pinna.sofa,
//   TU-Berlin_QU_KEMAR_anechoic_radius_0.5m.sofa (2 chunks),
//   subject_003.sofa (CIPIC), demo_FreeFieldHRTF_1_IR.sofa (THK KU100).
// Lee Data.IR + SourcePosition + Data.SamplingRate: superbloque v2 ->
// OHDR raíz -> Link Info -> fractal heap + B-tree v2 (nombres) -> OHDR del
// dataset -> dataspace / datatype / filter pipeline / data layout v3 ->
// chunk B-tree v1 -> inflate (libz, vía dlopen) + unshuffle -> ensamblado
// N-d por offset de chunk. El datatype puede vivir en un bloque de
// continuación OCHK (mensaje 0x10): Data.IR/SourcePosition/Data.Delay NO lo
// llevan inline; SamplingRate/C/M/N sí. El offset del heap-ID se mide desde
// la FIRMA del bloque FHDB (incluye su cabecera de 21 bytes).
//
// La descompresión usa libz (sistema, NDK) vía dlopen/dlsym — NO hay cambio
// de CMake ni nueva dependencia de link (el ámbito de este ítem no toca
// CMakeLists): si libz no está, el parseo falla y se cae al fallback PCA
// (fail-closed, sin crash). En host se resuelve "libz.so.1" o "libz.so".
//
// Si el parseo falla por cualquier motivo, load() sigue devolviendo true con
// el HRIR RECONSTRUIDO desde la base maestra SOFA-PCA (comportamiento
// histórico, Status::VALID_NOT_PARSED / UNSUPPORTED): el camino de audio
// nunca se queda sin HRIR por un SOFA que no se puede leer.
#include "SofaHRTFLoader.hpp"
#include "spatial/SofaSafRirMasterKnowledge.hpp"
#include "spatial/HrirPoseSelect.hpp"
#include <fstream>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <dlfcn.h>
#include <android/log.h>

#define LOG_TAG "IvannaSofaHRTF"
// SOFA_OFFLINE_CACHE (2026-09-17): evita recargar HDF5 desde emisores
// repetidos (cambio de escala de habitación por UI, re-entry de la app).
// Guardado del último path válido en el daemon/app — solo una lectura
// de disco adicional se intenta si el anterior no es válido os del
// cambio de proveación (subject/angle).
static std::string g_sofa_lastPath;
static bool   g_sofa_lastOk = false;
// Las dos estáticas de arriba se comparten entre instancias e hilos (carga desde la UI y desde
// el servicio): sin exclusión mutua eran una carrera de datos sobre un std::string. NO-RT.
static std::mutex g_sofa_cacheMutex;
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace Ivanna {
namespace {

// ── Lectores little-endian con chequeo de límites ────────────────────────────
bool rd16(const unsigned char* d, size_t n, size_t off, uint16_t& v) noexcept {
    if (off + 2 > n) return false;
    v = static_cast<uint16_t>(d[off] | (static_cast<uint16_t>(d[off + 1]) << 8));
    return true;
}
bool rd32(const unsigned char* d, size_t n, size_t off, uint32_t& v) noexcept {
    if (off + 4 > n) return false;
    v = static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
        (static_cast<uint32_t>(d[off + 2]) << 16) |
        (static_cast<uint32_t>(d[off + 3]) << 24);
    return true;
}
bool rd64(const unsigned char* d, size_t n, size_t off, uint64_t& v) noexcept {
    if (off + 8 > n) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= (static_cast<uint64_t>(d[off + i]) << (8 * i));
    return true;
}

// ── libz vía dlopen (sin dependencia de link) ───────────────────────────────
// int uncompress(unsigned char* dest, unsigned long* destLen,
//                const unsigned char* src, unsigned long srcLen); -> 0 = Z_OK.
using UncompressFn = int (*)(unsigned char*, unsigned long*, const unsigned char*,
                             unsigned long);
UncompressFn getUncompress() noexcept {
    static UncompressFn fn = []() -> UncompressFn {
        void* h = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) return nullptr;
        return reinterpret_cast<UncompressFn>(dlsym(h, "uncompress"));
    }();
    return fn;
}

// Deshace el filtro shuffle (HDF5): el stream es [byte0 de todos los
// elementos][byte1 de todos]...; el elemento i, byte k, vive en k*n+i.
void unshuffle(std::vector<unsigned char>& buf, size_t elemSize) {
    const size_t n = buf.size() / elemSize;
    if (n == 0 || elemSize <= 1) return;
    std::vector<unsigned char> tmp(buf.size());
    for (size_t k = 0; k < elemSize; ++k)
        for (size_t i = 0; i < n; ++i)
            tmp[i * elemSize + k] = buf[k * n + i];
    buf.swap(tmp);
}

// ── Mensajes de object header ───────────────────────────────────────────────
struct Msg {
    uint8_t type = 0;
    size_t bodyOff = 0;  // offset en el archivo
    size_t bodyLen = 0;
};

bool walkMessages(const std::vector<unsigned char>& d, size_t pos, size_t end,
                  size_t cord, std::vector<Msg>& out, int depth) {
    if (depth > 8) return false;  // continuaciones anidadas: corrupt loop guard
    const size_t n = d.size();
    const unsigned char* p = d.data();
    while (pos + 4 + cord <= end) {
        const uint8_t type = p[pos];
        uint16_t size = 0;
        if (!rd16(p, n, pos + 1, size)) return false;
        if (type == 0) {  // NIL: relleno, se salta (NO es fin de mensajes)
            pos += 4 + cord + size;
            continue;
        }
        if (pos + 4 + cord + size > end) break;  // checksum/cola del bloque
        const size_t bodyOff = pos + 4 + cord;
        out.push_back(Msg{type, bodyOff, size});
        if (type == 0x10) {  // continuación de object header -> bloque OCHK
            uint64_t ca = 0, cs = 0;
            if (size < 16 || !rd64(p, n, bodyOff, ca) || !rd64(p, n, bodyOff + 8, cs))
                return false;
            if (ca + 4 > n || cs > n || ca + 4 + (size_t)cs > n) return false;
            if (std::memcmp(p + ca, "OCHK", 4) != 0) return false;
            // En el OCHK los mensajes empiezan tras la firma (4 B), con la
            // misma regla de creation-order del OHDR padre.
            if (!walkMessages(d, (size_t)ca + 4, (size_t)ca + 4 + (size_t)cs, cord, out,
                              depth + 1))
                return false;
        }
        pos += 4 + cord + size;
    }
    return true;
}

bool readOhdrMessages(const std::vector<unsigned char>& d, size_t ohdr,
                      std::vector<Msg>& out) {
    const size_t n = d.size();
    if (ohdr + 8 > n || std::memcmp(d.data() + ohdr, "OHDR", 4) != 0) return false;
    if (d[ohdr + 4] != 2) return false;  // object header v2
    const uint8_t flags = d[ohdr + 5];
    const size_t w = size_t(1) << (flags & 3);
    if (w > 8 || ohdr + 6 + w > n) return false;
    size_t msize = 0;
    for (size_t i = 0; i < w; ++i) msize |= (size_t)d[ohdr + 6 + i] << (8 * i);
    const size_t cord = (flags & 0x04) ? 2 : 0;
    const size_t start = ohdr + 6 + w;
    // msize no puede superar el archivo (evita el wrap de start+msize).
    if (msize > n || start > n || start + msize > n) return false;
    return walkMessages(d, start, start + msize, cord, out, 0);
}

const Msg* findMsg(const std::vector<Msg>& msgs, uint8_t type) noexcept {
    for (const Msg& m : msgs)
        if (m.type == type) return &m;
    return nullptr;
}

// ── Resolución de enlaces del grupo raíz ────────────────────────────────────
struct Link {
    std::string name;
    size_t ohdr = 0;
};

bool resolveRootLinks(const std::vector<unsigned char>& d, size_t rootAddr,
                      std::vector<Link>& out) {
    const size_t n = d.size();
    const unsigned char* p = d.data();
    std::vector<Msg> root;
    if (!readOhdrMessages(d, rootAddr, root)) return false;
    const Msg* li = findMsg(root, 0x02);  // Link Info
    if (!li || li->bodyLen < 34) return false;
    uint64_t heap = 0, btree = 0;
    if (!rd64(p, n, li->bodyOff + 10, heap) || !rd64(p, n, li->bodyOff + 18, btree))
        return false;
    // B-tree v2 (índice de nombres de enlace)
    if (btree + 38 > n || std::memcmp(p + btree, "BTHD", 4) != 0) return false;
    uint16_t recSize = 0, rootNRec = 0;
    uint64_t rootNode = 0;
    if (!rd16(p, n, (size_t)btree + 10, recSize) ||
        !rd64(p, n, (size_t)btree + 16, rootNode) ||
        !rd16(p, n, (size_t)btree + 24, rootNRec))
        return false;
    if (recSize != 11 || rootNRec == 0 || rootNRec > 100000) return false;
    if (rootNode + 6 > n || std::memcmp(p + rootNode, "BTLF", 4) != 0) return false;
    // Bloques FHDB del fractal heap (el offset del heap-ID se mide desde la
    // FIRMA del bloque, no desde sus datos: incluye la cabecera de 21 B).
    struct Block {
        uint64_t blkOff = 0;
        size_t start = 0;
    };
    std::vector<Block> blocks;
    for (size_t i = 0; i + 21 <= n;) {
        const unsigned char* f =
            static_cast<const unsigned char*>(std::memchr(p + i, 'F', n - i));
        if (!f) break;
        const size_t at = static_cast<size_t>(f - p);
        if (at + 21 > n) break;
        if (std::memcmp(p + at, "FHDB", 4) == 0) {
            uint64_t ha = 0;
            if (rd64(p, n, at + 5, ha) && ha == heap) {
                uint32_t bo = 0;
                if (rd32(p, n, at + 13, bo)) blocks.push_back(Block{bo, at});
            }
        }
        i = at + 1;
    }
    if (blocks.empty()) return false;
    for (uint16_t r = 0; r < rootNRec; ++r) {
        const size_t rp = (size_t)rootNode + 6 + (size_t)r * recSize;
        if (rp + recSize > n) return false;
        // Registro = hash (4 B) + heap ID (7 B): offset = LE16 en heapID[1:3],
        // longitud = heapID[5]. Validado 16/16 en MIT KEMAR.
        uint16_t off = 0;
        if (!rd16(p, n, rp + 5, off)) return false;
        const uint8_t len = p[rp + 9];
        const Block* blk = nullptr;
        for (const Block& b : blocks)
            if (b.blkOff <= off && (!blk || b.blkOff > blk->blkOff)) blk = &b;
        if (!blk) return false;
        const size_t pos = blk->start + (size_t)(off - blk->blkOff);
        if (pos + len > n) return false;
        if (p[pos] != 1) return false;  // link message v1
        const uint8_t flags = p[pos + 1];
        size_t q = pos + 2;
        if (flags & 0x04) q += 8;  // creation order (bit 2, no bit 3)
        if (flags & 0x02) q += 1;  // link type
        uint16_t namelen = 0;
        if (flags & 0x01) {
            if (!rd16(p, n, q, namelen)) return false;
            q += 2;
        } else {
            if (q >= n) return false;
            namelen = p[q];
            q += 1;
        }
        if (q + namelen + 8 > pos + len) return false;
        uint64_t ohdr = 0;
        if (!rd64(p, n, q + namelen, ohdr)) return false;
        if (ohdr + 4 > n || std::memcmp(p + ohdr, "OHDR", 4) != 0) return false;
        out.push_back(Link{std::string(reinterpret_cast<const char*>(p + q), namelen),
                           (size_t)ohdr});
    }
    return !out.empty();
}

// ── Dataset ─────────────────────────────────────────────────────────────────
struct Dataset {
    std::vector<uint64_t> dims;
    int dtypeClass = -1;      // 1 = float
    size_t elemSize = 0;      // 4 o 8
    bool hasDeflate = false;  // filtro 1
    bool hasShuffle = false;  // filtro 2
    std::vector<uint64_t> chunkDims;
    struct Chunk {
        uint64_t csize = 0;  // tamaño comprimido en archivo
        uint64_t addr = 0;
        std::vector<uint64_t> offset;  // posición del chunk en el dataset
    };
    std::vector<Chunk> chunks;
};

bool readDatasetMeta(const std::vector<unsigned char>& d,
                     const std::vector<Link>& links, const char* name,
                     Dataset& ds, bool& unsupported) {
    const size_t n = d.size();
    const unsigned char* p = d.data();
    size_t ohdr = 0;
    bool found = false;
    for (const Link& l : links)
        if (l.name == name) {
            ohdr = l.ohdr;
            found = true;
            break;
        }
    if (!found) return false;
    std::vector<Msg> msgs;
    if (!readOhdrMessages(d, ohdr, msgs)) return false;
    const Msg* dsp = findMsg(msgs, 0x01);  // dataspace
    const Msg* dt = findMsg(msgs, 0x03);   // datatype (puede faltar: ver abajo)
    const Msg* flt = findMsg(msgs, 0x0b);  // filter pipeline
    const Msg* lay = findMsg(msgs, 0x08);  // data layout
    if (!dsp || !lay) return false;
    // Dataspace v2/v3: ver, ndims, flags, reservado, dims (ndims x u64).
    if (dsp->bodyLen < 4) return false;
    const uint8_t ver = p[dsp->bodyOff];
    if (ver != 2 && ver != 3) return false;
    const int ndims = p[dsp->bodyOff + 1];
    if (ndims < 1 || ndims > 8) return false;
    if (dsp->bodyLen < 4 + (size_t)ndims * 8) return false;
    ds.dims.clear();
    uint64_t total = 1;
    for (int i = 0; i < ndims; ++i) {
        uint64_t v = 0;
        if (!rd64(p, n, dsp->bodyOff + 4 + (size_t)i * 8, v)) return false;
        if (v == 0 || v > 1000000) return false;
        if (total > (uint64_t(1) << 31) / v) return false;
        total *= v;
        ds.dims.push_back(v);
    }
    // Filtros (pipeline v2 compacto): ver=2, numFiltros, por filtro
    // id(2) numClient(2) flags(2) clientData(numClient x u32).
    if (flt) {
        if (flt->bodyLen < 2 || p[flt->bodyOff] != 2) return false;
        const int nf = p[flt->bodyOff + 1];
        if (nf < 0 || nf > 8) return false;
        size_t q = flt->bodyOff + 2;
        for (int i = 0; i < nf; ++i) {
            if (q + 6 > flt->bodyOff + flt->bodyLen) return false;
            uint16_t fid = 0, nc = 0;
            if (!rd16(p, n, q, fid) || !rd16(p, n, q + 2, nc)) return false;
            q += 6;
            if (q + (size_t)nc * 4 > flt->bodyOff + flt->bodyLen) return false;
            if (fid == 1)
                ds.hasDeflate = true;
            else if (fid == 2)
                ds.hasShuffle = true;
            else
                return false;  // filtro desconocido: fail-closed
            q += (size_t)nc * 4;
        }
    }
    // Layout v3 chunked: ver=3, class=2, idx B-tree (8), chunk dims (ndims x u32),
    // tamaño de elemento (u32). class 0/1 (contiguo/compacto) fuera de alcance.
    if (lay->bodyLen < 11 + (size_t)ndims * 4 + 4) return false;
    if (p[lay->bodyOff] != 3 || p[lay->bodyOff + 1] != 2) {
        unsupported = true;
        return false;
    }
    uint64_t idxAddr = 0;
    if (!rd64(p, n, lay->bodyOff + 3, idxAddr)) return false;
    ds.chunkDims.clear();
    for (int i = 0; i < ndims; ++i) {
        uint32_t v = 0;
        if (!rd32(p, n, lay->bodyOff + 11 + (size_t)i * 4, v)) return false;
        if (v == 0 || v > 1000000) return false;
        ds.chunkDims.push_back(v);
    }
    uint32_t elem = 0;
    if (!rd32(p, n, lay->bodyOff + 11 + (size_t)ndims * 4, elem)) return false;
    if (elem != 4 && elem != 8) return false;
    ds.elemSize = elem;
    // Datatype: class = body[0]>>4, tamaño = u32 en body+4. Si el mensaje no
    // está (Data.IR, SourcePosition, Data.Delay lo llevan en la continuación
    // OCHK; si ni ahí está, se infiere del tamaño de elemento del layout).
    if (dt) {
        if (dt->bodyLen < 8) return false;
        ds.dtypeClass = p[dt->bodyOff] >> 4;
        uint32_t sz = 0;
        if (!rd32(p, n, dt->bodyOff + 4, sz)) return false;
        if (ds.dtypeClass != 1 || (sz != 4 && sz != 8)) {
            unsupported = true;
            return false;
        }
        if (sz != elem) return false;  // datatype y layout no concuerdan
    } else {
        ds.dtypeClass = 1;
    }
    // Chunk B-tree v1: cabecera 24 B, registros de 24+8*ndims B:
    // [tamaño comprimido(8)][offset del chunk ((ndims+1) x u64)][dirección(8)].
    // Validado en MIT (1 chunk) y TU-Berlin (2 chunks: rec1 offset (0,1,0),
    // dirección = fin del chunk0, tamaño 5237307).
    if (idxAddr + 24 > n || std::memcmp(p + idxAddr, "TREE", 4) != 0) return false;
    uint16_t nused = 0;
    if (!rd16(p, n, (size_t)idxAddr + 6, nused)) return false;
    if (nused == 0 || nused > 1000000) return false;
    const size_t stride = 24 + (size_t)ndims * 8;
    ds.chunks.clear();
    for (uint16_t i = 0; i < nused; ++i) {
        const size_t rp = (size_t)idxAddr + 24 + (size_t)i * stride;
        if (rp + stride > n) return false;
        Dataset::Chunk c;
        if (!rd64(p, n, rp, c.csize)) return false;
        c.offset.clear();
        for (int j = 0; j < ndims; ++j) {
            uint64_t v = 0;
            if (!rd64(p, n, rp + 8 + (size_t)j * 8, v)) return false;
            c.offset.push_back(v);
        }
        if (!rd64(p, n, rp + 8 + (size_t)(ndims + 1) * 8, c.addr)) return false;
        if (c.csize == 0 || c.addr + c.csize > n) return false;
        for (int j = 0; j < ndims; ++j)
            if (c.offset[j] + ds.chunkDims[j] > ds.dims[j]) return false;
        ds.chunks.push_back(std::move(c));
    }
    return true;
}

// Coloca un chunk (elementos en orden C) en su offset del dataset destino.
template <typename T>
bool placeChunk(const T* src, const std::vector<uint64_t>& chunkDims,
                const std::vector<uint64_t>& offset,
                const std::vector<uint64_t>& dims, std::vector<float>& out) {
    const size_t nd = dims.size();
    std::vector<uint64_t> ostr(nd, 1);
    for (size_t i = nd - 1; i-- > 0;) ostr[i] = ostr[i + 1] * dims[i + 1];
    size_t total = 1;
    for (uint64_t v : chunkDims) total *= (size_t)v;
    std::vector<uint64_t> idx(nd, 0);
    for (size_t flat = 0; flat < total; ++flat) {
        size_t oi = 0;
        for (size_t i = 0; i < nd; ++i) oi += (size_t)(offset[i] + idx[i]) * (size_t)ostr[i];
        if (oi >= out.size()) return false;
        out[oi] = static_cast<float>(src[flat]);
        for (size_t i = nd; i-- > 0;) {
            if (++idx[i] < chunkDims[i]) break;
            idx[i] = 0;
        }
    }
    return true;
}

bool readDatasetData(const std::vector<unsigned char>& d, const Dataset& ds,
                     std::vector<float>& out) {
    const size_t n = d.size();
    uint64_t total = 1;
    for (uint64_t v : ds.dims) total *= v;
    out.assign((size_t)total, 0.0f);
    uint64_t chunkElems = 1;
    for (uint64_t v : ds.chunkDims) chunkElems *= v;
    const size_t chunkBytes = (size_t)chunkElems * ds.elemSize;
    UncompressFn uncompress = ds.hasDeflate ? getUncompress() : nullptr;
    if (ds.hasDeflate && !uncompress) return false;  // sin libz: fail-closed
    std::vector<unsigned char> buf;
    for (const Dataset::Chunk& c : ds.chunks) {
        buf.assign(d.begin() + (ptrdiff_t)c.addr,
                   d.begin() + (ptrdiff_t)(c.addr + c.csize));
        if (ds.hasDeflate) {
            std::vector<unsigned char> inflated(chunkBytes);
            unsigned long destLen = (unsigned long)chunkBytes;
            const int rc = uncompress(inflated.data(), &destLen, buf.data(),
                                      (unsigned long)buf.size());
            if (rc != 0 || destLen != chunkBytes) return false;
            buf.swap(inflated);
        } else if (buf.size() != chunkBytes) {
            return false;
        }
        if (ds.hasShuffle) unshuffle(buf, ds.elemSize);
        // buf viene de std::vector (operator new: alineado a max_align_t,
        // >= 8 B), así que el reinterpret a float/double está alineado.
        if (ds.elemSize == 8) {
            const auto* src = reinterpret_cast<const double*>(buf.data());
            if (!placeChunk<double>(src, ds.chunkDims, c.offset, ds.dims, out))
                return false;
        } else {
            const auto* src = reinterpret_cast<const float*>(buf.data());
            if (!placeChunk<float>(src, ds.chunkDims, c.offset, ds.dims, out))
                return false;
        }
    }
    for (float v : out)
        if (!std::isfinite(v)) return false;
    return true;
}

} // namespace

bool SofaHRTFLoader::loadCached(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_sofa_cacheMutex);
    // La caché (ruta + ok) es GLOBAL pero m_hrtf es de ESTA instancia: un acierto solo vale si
    // esta instancia ya tiene el HRIR de esa ruta. Antes devolvía true con m_hrtf vacío.
    if (g_sofa_lastOk && path == g_sofa_lastPath &&
        (isMeasured() || isReconstructedFromMasterBasis()) &&
        !m_hrtf.left.empty() && m_hrtf.left.size() == m_hrtf.right.size()) {
        return true; // caché caliente
    }
    const bool ok = load(path);
    g_sofa_lastOk = ok;
    if (ok) g_sofa_lastPath = path; else g_sofa_lastPath.clear();
    return ok;
}

bool SofaHRTFLoader::parseMeasured(const std::vector<unsigned char>& file,
                                   bool& unsupported) {
    unsupported = false;
    const size_t n = file.size();
    const unsigned char* p = file.data();
    // Superbloque v2: firma (8 B) + versión. v0/v1/v3 fuera de alcance
    // (los hpir_nh*.sofa son v0): UNSUPPORTED, no un fallo de parseo.
    if (n < 48 || p[8] != 2) {
        unsupported = true;
        return false;
    }
    uint64_t root = 0;
    if (!rd64(p, n, 36, root)) return false;
    std::vector<Link> links;
    if (!resolveRootLinks(file, (size_t)root, links)) return false;

    Dataset ir, sp, sr;
    if (!readDatasetMeta(file, links, "Data.IR", ir, unsupported)) return false;
    if (ir.dims.size() != 3 || ir.dims[1] != 2) return false;
    if (!readDatasetMeta(file, links, "SourcePosition", sp, unsupported)) return false;
    if (sp.dims.size() != 2 || sp.dims[1] != 3 || sp.dims[0] != ir.dims[0]) return false;
    if (!readDatasetMeta(file, links, "Data.SamplingRate", sr, unsupported)) return false;
    if (sr.dims.size() != 1 || sr.dims[0] != 1) return false;

    std::vector<float> irData, spData, srData;
    if (!readDatasetData(file, ir, irData)) return false;
    if (!readDatasetData(file, sp, spData)) return false;
    if (!readDatasetData(file, sr, srData)) return false;
    const double fs = srData[0];
    if (!std::isfinite(fs) || fs < 1000.0 || fs > 384000.0) return false;

    const size_t nPos = (size_t)ir.dims[0];
    const size_t taps = (size_t)ir.dims[2];
    if (nPos == 0 || taps == 0 || irData.size() != nPos * 2 * taps ||
        spData.size() != nPos * 3)
        return false;
    for (size_t i = 0; i < nPos; ++i) {
        const double r = spData[i * 3 + 2];
        if (!std::isfinite(spData[i * 3]) || !std::isfinite(spData[i * 3 + 1]) ||
            !std::isfinite(r) || r <= 0.0)
            return false;
    }
    // Commit: solo aquí se toca el estado medido (si algo falló arriba, la
    // instancia queda sin dataset medido y load() cae al fallback PCA).
    m_numPos = nPos;
    m_taps = taps;
    m_sampleRateHz = (float)fs;
    m_ir.swap(irData);
    m_srcPos.swap(spData);
    return true;
}

void SofaHRTFLoader::reconstructFromPca(const std::string& path) {
    m_hrtf.sampleRate = 48000.0f;
    constexpr int kLen = ivanna::master::kMasterHrirLen;
    m_hrtf.left.assign(kLen, 0.0f);
    m_hrtf.right.assign(kLen, 0.0f);

    // Subcadena de la ruta con la coincidencia MÁS LARGA: "kemar" precede a "kemar_large" y a
    // "tu_berlin_kemar" en la tabla, y con la primera coincidencia esos sujetos recibían el ancla
    // de KEMAR estándar.
    const float* qSubj = ivanna::master::kMasterSafGoldenQ;
    size_t bestLen = 0;
    for (size_t s = 0; s < ivanna::master::kNumTrainedSubjects; ++s) {
        const char* id = ivanna::master::kTrainedSubjectAnchors[s].id;
        const size_t idLen = std::strlen(id);
        if (idLen > bestLen && path.find(id) != std::string::npos) {
            qSubj = ivanna::master::kTrainedSubjectAnchors[s].q;
            bestLen = idLen;
        }
    }
    for (int n = 0; n < kLen; ++n) {
        float l = ivanna::master::kMasterSofaP0[n];
        float r = ivanna::master::kMasterSofaP0[kLen + n];
        for (int k = 0; k < ivanna::master::kMasterSafK; ++k) {
            l += qSubj[k] * ivanna::master::kMasterSofaPcaV[k][n];
            r += qSubj[k] * ivanna::master::kMasterSofaPcaV[k][kLen + n];
        }
        m_hrtf.left[n] = l;
        m_hrtf.right[n] = r;
    }
}

bool SofaHRTFLoader::load(const std::string& path) {
    LOGI("SofaHRTFLoader: Validando SOFA en %s", path.c_str());

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        lastStatus_ = Status::FILE_NOT_FOUND;
        LOGE("SofaHRTFLoader: Archivo no encontrado o sin permisos: %s", path.c_str());
        return false;
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    // FIX: umbral mínimo reducido de 1024 → 512.
    // Un SOFA con pocos ángulos medidos puede tener cabecera de ~600 bytes.
    // El umbral anterior rechazaba archivos SOFA válidos pequeños como
    // archivos corruptos — silencioso, el fallback sintético quedaba activo
    // sin que nadie supiera que el archivo era válido pero demasiado pequeño
    // para el umbral.
    if (size < 512) {
        lastStatus_ = Status::CORRUPT;
        LOGE("SofaHRTFLoader: Archivo demasiado pequeño (%ld bytes) — "
             "corrompido o placeholder truncado.", (long)size);
        return false;
    }
    // Guarda: un "SOFA" de >1 GiB no es un SOFA del catálogo; no intentar
    // cargarlo entero en memoria (fail-closed, sin OOM).
    if (size > (std::streamsize(1) << 30)) {
        lastStatus_ = Status::CORRUPT;
        LOGE("SofaHRTFLoader: Archivo demasiado grande (%ld bytes).", (long)size);
        return false;
    }

    // Firma HDF5 completa (8 bytes): 0x89 H D F 0x0D 0x0A 0x1A 0x0A
    // El check anterior solo comparaba los primeros 4 bytes (0x89 H D F)
    // y dejaba pasar archivos binarios que empezaran con esa secuencia
    // pero no fueran HDF5 reales (ej. algunos formatos de audio propietarios).
    // FIX: comparar los 8 bytes completos de la firma.
    uint8_t magic[8] = {};
    if (!file.read(reinterpret_cast<char*>(magic), 8) || !file.good()) {
        lastStatus_ = Status::CORRUPT;
        LOGE("SofaHRTFLoader: Error leyendo cabecera de %s", path.c_str());
        return false;
    }

    const bool isHDF5 = (magic[0] == 0x89u && magic[1] == 'H'  &&
                         magic[2] == 'D'   && magic[3] == 'F'  &&
                         magic[4] == 0x0Du && magic[5] == 0x0Au &&
                         magic[6] == 0x1Au && magic[7] == 0x0Au);

    if (!isHDF5) {
        lastStatus_ = Status::NOT_HDF5;
        // Distinguir "no es SOFA" de "archivo corrompido" en el log para que
        // el desarrollador sepa exactamente qué pasó sin inspeccionar el archivo.
        LOGE("SofaHRTFLoader: %s NO es un SOFA AES69 válido — "
             "firma HDF5 incorrecta (esperado 89 48 44 46 0D 0A 1A 0A, "
             "encontrado %02X %02X %02X %02X %02X %02X %02X %02X).",
             path.c_str(),
             magic[0], magic[1], magic[2], magic[3],
             magic[4], magic[5], magic[6], magic[7]);
        return false;
    }

    // Leer el archivo entero (TU-Berlin: 10.5 MB; carga NO-RT, una sola vez).
    std::vector<unsigned char> bytes((size_t)size);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size) || !file.good()) {
        lastStatus_ = Status::CORRUPT;
        LOGE("SofaHRTFLoader: Error leyendo %s", path.c_str());
        return false;
    }

    // Estado medido previo: se limpia antes del intento (si el parseo falla,
    // no queda un dataset viejo presentándose como el de este archivo).
    m_ir.clear();
    m_srcPos.clear();
    m_numPos = 0;
    m_taps = 0;
    m_sampleRateHz = 0.0f;
    m_path = path;

    bool unsupported = false;
    if (parseMeasured(bytes, unsupported)) {
        lastStatus_ = Status::PARSED_MEASURED;
        // hrtf() = par frontal medido (az 0, el 0) a taps/SR nativos.
        HRTFIR front;
        if (hrtfAt(0.0f, 0.0f, front)) {
            m_hrtf = std::move(front);
        } else {
            // No debería pasar (parseMeasured validó >= 1 posición finita):
            // fail-closed al PCA antes que dejar un hrtf() vacío con éxito.
            lastStatus_ = Status::VALID_NOT_PARSED;
            reconstructFromPca(path);
            return true;
        }
        LOGI("SofaHRTFLoader: MEDIDO %s: %zu posiciones, %zu taps/oído, %.0f Hz "
             "(Data.IR leído de verdad, azimut por posición más cercana).",
             path.c_str(), m_numPos, m_taps, (double)m_sampleRateHz);
        return true;
    }

    // Solo se comprobó la FIRMA HDF5 o el parseo falló: el HRIR de 128 taps
    // por oído se RECONSTRUYE con la variedad PCA entrenada sobre las 255
    // mediciones SOFA + 12 datasets IHR1 (p0 + V * q_subj). Esto NO es una
    // carga de las mediciones del archivo: el sujeto (q_subj) se elige
    // buscando su id en la RUTA.
    lastStatus_ = unsupported ? Status::UNSUPPORTED : Status::VALID_NOT_PARSED;
    reconstructFromPca(path);
    LOGI("SofaHRTFLoader: firma HDF5 OK (%ld bytes); mediciones del archivo NO leídas "
         "(%s) -> HRIR RECONSTRUIDO desde base maestra SOFA-PCA (%d taps/oído).",
         (long)size, unsupported ? "formato no soportado" : "parseo fallido",
         ivanna::master::kMasterHrirLen);
    return true;
}

bool SofaHRTFLoader::hrtfAt(float azimuthDeg, float elevationDeg, HRTFIR& out) const {
    if (!isMeasured() || m_numPos == 0 || m_taps == 0) return false;
    if (!std::isfinite(azimuthDeg) || !std::isfinite(elevationDeg)) return false;
    const size_t best = ivanna::spatial::nearestHrirIndex(
        m_numPos,
        [this](size_t i) { return m_srcPos[i * 3]; },
        [this](size_t i) { return m_srcPos[i * 3 + 1]; },
        azimuthDeg, elevationDeg);
    out.sampleRate = m_sampleRateHz;
    out.left.assign(m_ir.begin() + (ptrdiff_t)(best * 2 * m_taps),
                    m_ir.begin() + (ptrdiff_t)(best * 2 * m_taps + m_taps));
    out.right.assign(m_ir.begin() + (ptrdiff_t)((best * 2 + 1) * m_taps),
                     m_ir.begin() + (ptrdiff_t)((best * 2 + 2) * m_taps));
    return out.left.size() == m_taps && out.right.size() == m_taps;
}

bool SofaHRTFLoader::sourcePosition(size_t i, float& azimuthDeg, float& elevationDeg,
                                    float& radiusM) const noexcept {
    if (!isMeasured() || i >= m_numPos) return false;
    azimuthDeg = m_srcPos[i * 3];
    elevationDeg = m_srcPos[i * 3 + 1];
    radiusM = m_srcPos[i * 3 + 2];
    return true;
}

} // namespace Ivanna
