// effect_local_files.h — pre-creación de los archivos locales de Ruta B.
//
// Causa raíz (Moto G85, 2026-11-09): libomega_effect.so corre en audioserver/HAL
// con UID no-root y NO PUEDE crear archivos en un directorio que no es
// grabable para él. El archivo de control
// /data/adb/ivanna_omega/omega_effect_ctrl_local (una página, ver
// OmegaControlBus::REGION_SIZE), el beacon
// /data/adb/ivanna_omega/omega_effect_beacon (OMEGA_EFFECT_BEACON_SIZE) y la
// evidencia /data/adb/ivanna_omega/omega_effect_evidence (256 B) los
// pre-crea ÚNICAMENTE el demonio (root) en cada arranque: con el archivo ya
// existente, el open(O_CREAT) del efecto ya no necesita permiso de creación
// del directorio. NO se escribe magic ni snapshot aquí: solo existencia
// + permisos + tamaño. El efecto los inicializa (magic, contadores) al mapearlos.
//
// 0043 (2026-11-17): antes vivían en /data/local/tmp y ahí el kernel los veía
// como `unlabeled` pese a chcon ivanna_data_file (beacon 72 B en ceros, G85
// v2.5.462). En /data/adb/ivanna_omega/ heredan ivanna_data_file del
// directorio padre (lo etiqueta service.sh); el re-etiquetado de
// precreateAndRelabelLocalFile queda como cinturón + tirantes.
//
// Header-only y sin dependencias IVANNA para poder probarlo en host sin NDK
// (ver tests/test_effect_local_files.cpp).
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <pwd.h>
#endif

namespace ivanna {

// Resultado de pre-crear un archivo. `ok == false` no es fatal: el llamador
// loguea el motivo y sigue (el efecto puede reintentarlo en su propio open()).
struct LocalFilePrecreateResult {
    bool ok = false;
    std::string detail;
};

// Garantiza que `path` exista con modo 0666 y tamaño `size`:
//   - open(O_RDWR|O_CREAT, 0666) + fchmod(0666) (el modo de open lo recorta
//     umask; el fchmod lo fuerza: audioserver necesita escritura para abrir
//     el beacon/control con O_RDWR).
//   - ftruncate(fd, size) SOLO si el tamaño actual es 0: nunca se trunca un
//     archivo que el efecto ya está usando (tamaño correcto), ni uno parcial
//     de origen desconocido (se reporta como advertencia, sin tocar).
// No escribe contenido: el efecto inicializa el magic al mapearlo.
inline LocalFilePrecreateResult precreateLocalFile(const char* path, std::size_t size) noexcept {
    LocalFilePrecreateResult r;
    if (path == nullptr || path[0] == '\0' || size == 0) {
        r.detail = "ruta/tamaño inválido";
        return r;
    }
    const int fd = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) {
        const int e = errno;
        r.detail = std::string("open failed: ") + std::strerror(e);
        return r;
    }
    if (::fchmod(fd, 0666) < 0) {
        const int e = errno;
        ::close(fd);
        r.detail = std::string("fchmod 0666 failed: ") + std::strerror(e);
        return r;
    }
#if defined(__ANDROID__)
    // 0044 (Fallo 6, reconciliado con 0043): dar propiedad directa a
    // audioserver para evitar el bloqueo de fs.protected_regular cuando
    // el efecto abre el archivo con O_CREAT (un archivo 0666 de otro
    // dueño — root — puede dar EACCES). Si getpwnam falla, se sigue sin
    // chown (el precreate + 0666 + herencia de tipo de 0043 ya cubren
    // el caso normal). Solo Android: en host no existe audioserver.
    if (const struct passwd* pw = ::getpwnam("audioserver")) {
        (void)::fchown(fd, pw->pw_uid, pw->pw_gid);
    }
#endif
    struct stat st {};
    if (::fstat(fd, &st) < 0) {
        const int e = errno;
        ::close(fd);
        r.detail = std::string("fstat failed: ") + std::strerror(e);
        return r;
    }
    if (st.st_size == 0) {
        if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
            const int e = errno;
            ::close(fd);
            r.detail = std::string("ftruncate failed: ") + std::strerror(e);
            return r;
        }
        r.detail = "creado con tamaño " + std::to_string(size);
    } else if (static_cast<std::size_t>(st.st_size) == size) {
        r.detail = "ya existía con tamaño correcto, sin tocar";
    } else {
        ::close(fd);
        r.detail = "tamaño inesperado " + std::to_string(st.st_size) + " (esperado " +
                   std::to_string(size) + "); no se trunca";
        return r;
    }
    ::close(fd);
    r.ok = true;
    return r;
}

// Resultado de re-etiquetar un archivo. `ok == false` no es fatal: el
// llamador loguea el motivo y sigue (el doctor puede reintentarlo en vivo
// vía root). En host no existe /system/bin/chcon: devuelve ok=false con
// detail legible, sin tocar el archivo.
struct LocalFileRelabelResult {
    bool ok = false;
    std::string detail;
};

// Pre-crea `path` (ver precreateLocalFile) y lo re-etiqueta al contexto
// SELinux `selinux_type` con fork+execv de /system/bin/chcon (sin shell y
// sin libselinux: no está enlazada en el daemon ni en el build host).
// Causa raíz (0038, Moto G85 2026-11-17): el beacon nacía con el contexto
// heredado de su directorio y audioserver recibía EACCES al abrirlo;
// sepolicy.rule ya permite a audioserver escribir ivanna_data_file.
// 0043: el directorio ya no es /data/local/tmp (ahí el kernel veía el
// archivo como `unlabeled` pese al chcon) sino /data/adb/ivanna_omega/,
// etiquetado ivanna_data_file por service.sh; los archivos nuevos heredan
// el tipo del padre y este re-etiquetado queda como cinturón + tirantes.
// `ok == true` solo si el archivo quedó con 0666 + tamaño correcto Y el
// chcon terminó con exit 0. Nunca trunca un archivo con tamaño correcto.
inline LocalFileRelabelResult precreateAndRelabelLocalFile(
    const char* path, std::size_t size, const char* selinux_type) noexcept {
    LocalFileRelabelResult r;
    const LocalFilePrecreateResult pre = precreateLocalFile(path, size);
    if (!pre.ok) {
        r.detail = std::string("precreate falló: ") + pre.detail;
        return r;
    }
    if (selinux_type == nullptr || selinux_type[0] == '\0') {
        r.detail = "contexto SELinux vacío";
        return r;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        r.detail = std::string("fork falló: ") + std::strerror(errno);
        return r;
    }
    if (pid == 0) {
        // Hijo: chcon CONTEXTO ARCHIVO. Sin shell (sin quoting).
        const char* argv[] = {"chcon", selinux_type, path, nullptr};
        ::execv("/system/bin/chcon", const_cast<char* const*>(argv));
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            r.detail = std::string("waitpid falló: ") + std::strerror(errno);
            return r;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        r.ok = true;
        r.detail = std::string("relabel OK (") + pre.detail + ")";
        return r;
    }
    r.detail = std::string("chcon falló (") + pre.detail + ")";
    return r;
}

}  // namespace ivanna
