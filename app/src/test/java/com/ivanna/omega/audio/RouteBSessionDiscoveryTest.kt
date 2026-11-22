// RouteBSessionDiscoveryTest.kt
// ============================================================================
// PRUEBAS UNITARIAS — Descubrimiento de sesiones de Ruta B (v2.5.375)
// © 2026 Luis Uriel Pimentel Pérez
//
// Cubre la lógica pura del fix de Ruta B (v2.5.375):
//  - isUsableSessionId: solo sessionId > 0 es utilizable. -1 es la copia
//    anonimizada de AOSP (AudioSystem.AUDIO_SESSION_ALLOCATE, lo que devuelve
//    getActivePlaybackConfigurations() sin MODIFY_AUDIO_ROUTING) y 0 es
//    AUDIO_SESSION_NONE. El escáner de la 374 era solo diagnóstico porque
//    no distinguía estos valores; el de la 375 abre las sesiones con ID real
//    y cae al omega global (sesión 0) cuando todas llegan anonimizadas.
//  - usableSessionIds: extrae los IDs utilizables de pares
//    (sessionId, clientUid) reflejados, sin duplicados.
// La reflexión real (getSessionId/getClientUid @hide) y la creación del
// efecto en sesión 0 solo se pueden verificar en el teléfono: en logcat
// 'scanActivePlaybackSessions: N configs, R con sessionId real, A
// anonimizadas' dice si este ROM anonimiza o no, y 'Ruta B GLOBAL activa'
// confirma el fallback.
// ============================================================================

package com.ivanna.omega.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RouteBSessionDiscoveryTest {

    // ── isUsableSessionId ───────────────────────────────────────────────────
    @Test fun positiveSessionIdsAreUsable() {
        assertTrue(isUsableSessionId(1))
        assertTrue(isUsableSessionId(36081))
        assertTrue(isUsableSessionId(Int.MAX_VALUE))
    }

    @Test fun anonymizedMinusOneIsNotUsable() {
        // AUDIO_SESSION_ALLOCATE (-1): copia anonimizada de AOSP sin
        // MODIFY_AUDIO_ROUTING. Es el valor que el sondeo ve en el Moto G85
        // para TIDAL/YouTube Music mientras la app no sea privilegiada.
        assertFalse(isUsableSessionId(-1))
    }

    @Test fun zeroAndNegativeAreNotUsable() {
        assertFalse(isUsableSessionId(0))   // AUDIO_SESSION_NONE
        assertFalse(isUsableSessionId(-2))
        assertFalse(isUsableSessionId(Int.MIN_VALUE))
    }

    // ── usableSessionIds ────────────────────────────────────────────────────
    @Test fun extractsOnlyUsableIds() {
        val pairs = listOf(36081 to 10123, -1 to -1, 129 to 10456, 0 to 0, -1 to -1)
        assertEquals(listOf(36081, 129), usableSessionIds(pairs))
    }

    @Test fun allAnonymizedYieldsEmpty() {
        val pairs = listOf(-1 to -1, 0 to 0, -1 to -1)
        assertTrue(usableSessionIds(pairs).isEmpty())
    }

    @Test fun emptyInputYieldsEmpty() {
        assertTrue(usableSessionIds(emptyList()).isEmpty())
    }

    @Test fun deduplicatesRepeatedSessionIds() {
        val pairs = listOf(36081 to 10123, 36081 to 10123, 36081 to 10999)
        assertEquals(listOf(36081), usableSessionIds(pairs))
    }

    // ── shouldAttachDiscoveredSession (v2.5.379) ─────────────────────────────
    // Decisión pura: ¿hay que (re)acoplar omega a la sesión descubierta por el
    // daemon root? La sesión 0 es un callejón sin salida (el efecto se crea
    // pero nunca recibe frames) y el sondeo por reflexión no ve sessionIds
    // reales, así que el daemon (root) descubre el ID real por dumpsys.

    @Test fun noDiscoveredSessionMeansNoAttach() {
        // discoveredId 0 = sin música (fail-closed): no se toca nada, sigue sesión 0.
        assertFalse(shouldAttachDiscoveredSession(0, 0, false))
        assertFalse(shouldAttachDiscoveredSession(-1, 0, false))
    }

    @Test fun firstDiscoveredSessionAttaches() {
        assertTrue(shouldAttachDiscoveredSession(14177, 0, false))
    }

    @Test fun alreadyAttachedToSameSessionDoesNothing() {
        assertFalse(shouldAttachDiscoveredSession(14177, 14177, true))
    }

    @Test fun discoveredSessionChangedReattaches() {
        // Los IDs son dinámicos (YTM era 297 y luego 14177): al cambiar hay
        // que liberar la anterior y re-acoplar a la nueva.
        assertTrue(shouldAttachDiscoveredSession(14177, 297, true))
        assertTrue(shouldAttachDiscoveredSession(14177, 297, false))
    }

    @Test fun attachedIdButOmegaDeadReattaches() {
        // Estamos "acoplados" a 14177 pero el omega murió: reintentar.
        assertTrue(shouldAttachDiscoveredSession(14177, 14177, false))
    }

    // -- Supresión de huérfano desenganchado (0038-addendum) --
    @Test fun detachedOrphanSessionDoesNotReattach() {
        // Sesión 2329 desenganchada tras agotar reintentos: el watchdog no
        // debe re-acoplarla (si no, el desenganche no sirve de nada).
        assertFalse(shouldAttachDiscoveredSession(2329, 0, false, 2329))
    }

    // ── FASE A (2026-11-18) — sesión propia excluida del manager global ───
    // El manager es EXCLUSIVO para apps de terceros (IvannaBridgePlayer):
    // la sesión propia YA pasó por la cadena nativa; engancharle omega
    // sería doble procesado (Ruta A + Ruta B sobre el mismo audio).

    @Test fun ownSessionIsExcluded() {
        assertTrue(isExcludedOwnSession("com.ivanna.omega", "com.ivanna.omega"))
    }

    @Test fun thirdPartySessionIsNotExcluded() {
        assertFalse(isExcludedOwnSession("com.aspiro.tidal", "com.ivanna.omega"))
        assertFalse(isExcludedOwnSession("com.google.android.apps.youtube.music", "com.ivanna.omega"))
    }

    @Test fun unknownOriginIsNotExcluded() {
        // null = origen desconocido (sesión descubierta por el daemon, que
        // ya excluye la propia): fail-open, no se excluye.
        assertFalse(isExcludedOwnSession(null, "com.ivanna.omega"))
    }
    @Test fun newSessionAfterDetachAttaches() {
        // Una sesión DISTINTA (nueva app / rotación de Qobuz) sí puede
        // acoplarse aunque la anterior esté suprimida.
        assertTrue(shouldAttachDiscoveredSession(2417, 0, false, 2329))
    }

    // ── FASE A (2026-11-18) — cambio de app en foreground ──────────────────
    // La guarda de huérfano expira solo al cambiar a OTRA app de MÚSICA;
    // la sesión rancia (fondo) no se acopla mientras el foreground es otra
    // app de música.

    @Test fun musicPackageHintsMatchKnownApps() {
        assertTrue(packageLooksLikeMusic("com.aspiro.tidal"))
        assertTrue(packageLooksLikeMusic("com.qobuz.music"))
        assertTrue(packageLooksLikeMusic("com.spotify.music"))
        assertTrue(packageLooksLikeMusic("com.google.android.apps.youtube.music"))
        assertTrue(packageLooksLikeMusic("com.apple.android.music"))
    }

    @Test fun musicPackageHintsRejectNonMusicApps() {
        assertFalse(packageLooksLikeMusic("com.android.chrome"))
        assertFalse(packageLooksLikeMusic("com.example.game"))
        assertFalse(packageLooksLikeMusic("com.android.systemui"))
        assertFalse(packageLooksLikeMusic(""))
    }

    @Test fun orphanGuardExpiresOnMusicAppSwitch() {
        // Qobuz -> Tidal: la guarda de la sesión vieja expira.
        assertTrue(shouldExpireOrphanGuard("com.aspiro.tidal", "com.qobuz.music"))
    }

    @Test fun orphanGuardDoesNotExpireWithoutMusicSwitch() {
        // Misma app, foreground desconocido o cambio a app no musical:
        // la guarda se queda (la sesión de fondo sigue siendo la que suena).
        assertFalse(shouldExpireOrphanGuard("com.qobuz.music", "com.qobuz.music"))
        assertFalse(shouldExpireOrphanGuard("", "com.qobuz.music"))
        assertFalse(shouldExpireOrphanGuard("com.aspiro.tidal", ""))
        assertFalse(shouldExpireOrphanGuard("com.android.chrome", "com.qobuz.music"))
    }

    @Test fun staleDiscoveredSessionBlockedWhileForegroundIsOtherMusicApp() {
        // Daemon publica Qobuz 585 (fondo, en pausa) pero Tidal está en
        // foreground: no acoplarse a la rancia; esperar a su sesión.
        assertTrue(isStaleDiscoveredSession("com.aspiro.tidal", "com.qobuz.music"))
    }

    @Test fun discoveredSessionNotStaleWhenPackagesMatchOrForegroundIsNotMusic() {
        assertFalse(isStaleDiscoveredSession("com.qobuz.music", "com.qobuz.music"))
        // Chrome en foreground y Tidal sonando en segundo plano: la
        // descubierta (Tidal) NO es rancia.
        assertFalse(isStaleDiscoveredSession("com.android.chrome", "com.aspiro.tidal"))
        // Desconocido en cualquiera de los dos: no se bloquea (fail-closed).
        assertFalse(isStaleDiscoveredSession("", "com.qobuz.music"))
        assertFalse(isStaleDiscoveredSession("com.aspiro.tidal", ""))
    }
}
