// living_room.cpp — P9 simulador en host (ver header).
#include "living_room.hpp"

#include <cmath>

namespace ivanna::fused::living_room {

ModeInfo modeInfo(RoomMode mode) noexcept {
    switch (mode) {
        case RoomMode::kConservation:
            return {"Conservacion", "Minima intervencion",
                    "Solo guardarraíl de P2/P7", true};
        case RoomMode::kReference:
            return {"Referencia", "Maxima neutralidad verificable",
                    "Correccion minima, solo lo medido con confianza "
                    ">= 0.80",
                    true};
        case RoomMode::kMonumental:
            return {"Monumental", "Escala y energia controladas",
                    "Apertura y profundidad dentro de limites de fase y "
                    "voz",
                    true};
        case RoomMode::kIntimate:
            return {"Intimo", "Proximidad y baja dispersion",
                    "Reduce reverberacion percibida, acerca imagen", true};
        case RoomMode::kHistorical:
            return {"Historico", "Recreacion de una firma de epoca",
                    "Perfil de ecualizacion/ambiente documentado, "
                    "declarado como recreacion, no como fidelidad",
                    true};
        case RoomMode::kDirector:
            return {"Director", "Punto de escucha adaptable",
                    "Sigue al oyente (head-tracking + gemelo)", true};
        case RoomMode::kBlind:
            return {"Ciego",
                    "Eliminacion de pistas visuales y expectativas",
                    "Sin UI de modo visible durante la prueba; solo audio",
                    true};
    }
    return {"Desconocido", "", "", false};
}

double roomVolumeM3(const RoomGeometry& room) noexcept {
    if (!std::isfinite(room.lengthM) || !std::isfinite(room.widthM) ||
        !std::isfinite(room.heightM) || room.lengthM <= 0.0 ||
        room.widthM <= 0.0 || room.heightM <= 0.0) {
        return 0.0;
    }
    return room.lengthM * room.widthM * room.heightM;
}

double schroederHz(const RoomGeometry& room) noexcept {
    double v = roomVolumeM3(room);
    if (v <= 0.0 || !std::isfinite(room.rt60Sec) || room.rt60Sec <= 0.0) {
        return 0.0;
    }
    return 2000.0 * std::sqrt(room.rt60Sec / v);
}

double criticalDistanceM(const RoomGeometry& room) noexcept {
    double v = roomVolumeM3(room);
    if (v <= 0.0 || !std::isfinite(room.rt60Sec) || room.rt60Sec <= 0.0) {
        return 0.0;
    }
    return 0.057 * std::sqrt(v / room.rt60Sec);
}

double axialModeHz(const RoomGeometry& room, int axis, int n) noexcept {
    if (n < 1 || !std::isfinite(room.speedOfSoundMps) ||
        room.speedOfSoundMps <= 0.0) {
        return 0.0;
    }
    double length = 0.0;
    switch (axis) {
        case 0: length = room.lengthM; break;
        case 1: length = room.widthM; break;
        case 2: length = room.heightM; break;
        default: return 0.0;
    }
    if (!std::isfinite(length) || length <= 0.0) return 0.0;
    return (room.speedOfSoundMps / 2.0) * static_cast<double>(n) / length;
}

RoomDeltaResult roomDeltaDb(const RoomGeometry& room,
                            double listenerDistanceM) noexcept {
    RoomDeltaResult r;
    double dc = criticalDistanceM(room);
    if (dc <= 0.0 || !std::isfinite(listenerDistanceM) ||
        listenerDistanceM <= 0.0) {
        return r;  // valid=false
    }
    double ratio = listenerDistanceM / dc;
    r.deltaDb = 10.0 * std::log10(1.0 + ratio * ratio);
    r.valid = std::isfinite(r.deltaDb);
    return r;
}

DeltaChangeResult predictDeltaChange(const RoomGeometry& room, double fromM,
                                     double toM) noexcept {
    DeltaChangeResult r;
    RoomDeltaResult a = roomDeltaDb(room, fromM);
    RoomDeltaResult b = roomDeltaDb(room, toM);
    if (!a.valid || !b.valid) return r;  // valid=false
    r.changeDb = b.deltaDb - a.deltaDb;
    r.valid = std::isfinite(r.changeDb);
    return r;
}

}  // namespace ivanna::fused::living_room
