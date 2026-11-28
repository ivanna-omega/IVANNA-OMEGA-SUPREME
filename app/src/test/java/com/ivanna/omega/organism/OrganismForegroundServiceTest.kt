package com.ivanna.omega.organism

import android.app.Notification
import android.app.NotificationManager
import android.app.Service
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.Shadows.shadowOf
import org.robolectric.RobolectricTestRunner

/**
 * Tests del foreground service del Organismo (v2.5.510, fix del «nada»
 * de la 509). Robolectric: lee el Manifest real y ejecuta el servicio
 * sin dispositivo.
 *
 * QUÉ CUBRE:
 *   * El servicio está declarado en el Manifest con
 *     foregroundServiceType="microphone", exported=false, y el permiso
 *     FOREGROUND_SERVICE_MICROPHONE está declarado.
 *   * onStartCommand() llama a startForeground() con una notificación
 *     ongoing (persistente) que lleva la acción «Detener organismo»,
 *     y el canal de notificación existe.
 *   * El ID de notificación no colisiona con los servicios existentes
 *     (2 y 3: PlaybackCaptureService; 4: BridgePlaybackService).
 *   * La acción de detener apaga el interruptor maestro (la preferencia
 *     organism_enabled queda en OFF, no solo se para el servicio).
 *
 * QUÉ NO CUBRE (queda para la CI y para el G85):
 *   * Que OrganismController.start()/stop() arranquen/detengan el
 *     servicio: el cableado se verifica por construcción (grep, ver
 *     informe de la 510) y el arranque real del controlador exige
 *     AudioRecord + red, fuera de un unit test.
 *   * Que Android no congele el proceso ni silencie el mic con la app
 *     en segundo plano: solo lo cierra la verificación en vivo en el
 *     G85 (app en segundo plano con música sonando → las sesiones
 *     siguen llegando).
 */
@RunWith(RobolectricTestRunner::class)
class OrganismForegroundServiceTest {

    private val app: Context = ApplicationProvider.getApplicationContext()

    private fun serviceInfo(): ServiceInfo {
        val pm = app.packageManager
        return pm.getServiceInfo(
            ComponentName(app, OrganismForegroundService::class.java),
            PackageManager.GET_META_DATA
        )
    }

    @Test
    fun `servicio declarado con tipo microphone y no exportado`() {
        val info = serviceInfo()
        assertEquals(
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE,
            info.foregroundServiceType
        )
        assertFalse(info.exported)
    }

    @Test
    fun `permiso FOREGROUND_SERVICE_MICROPHONE declarado en el Manifest`() {
        val pm = app.packageManager
        val pkg = pm.getPackageInfo(app.packageName, PackageManager.GET_PERMISSIONS)
        assertTrue(
            pkg.requestedPermissions?.contains(
                "android.permission.FOREGROUND_SERVICE_MICROPHONE"
            ) == true
        )
    }

    @Test
    fun `id de notificacion sin colision con los demas servicios`() {
        assertEquals(5, OrganismForegroundService.NOTIFICATION_ID)
        assertTrue(
            OrganismForegroundService.NOTIFICATION_ID !in setOf(2, 3, 4)
        )
    }

    @Test
    fun `onStartCommand entra en primer plano con notificacion ongoing`() {
        val service = Robolectric.buildService(OrganismForegroundService::class.java)
            .create().get()
        val result = service.onStartCommand(null, 0, 0)

        assertEquals(Service.START_NOT_STICKY, result)
        val notification = shadowOf(service).lastForegroundNotification
        assertNotNull(notification)
        assertTrue(
            notification!!.flags and Notification.FLAG_ONGOING_EVENT != 0
        )
        // El canal propio existe (IMPORTANCE_LOW, sin sonido ni vibración).
        val nm = app.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        assertNotNull(shadowOf(nm).getNotificationChannel(OrganismForegroundService.CHANNEL_ID))
        // La notificación lleva la acción para detener el organismo.
        val titles = notification.actions?.map { it.title.toString() } ?: emptyList()
        assertTrue(titles.contains("Detener organismo"))
    }

    @Test
    fun `accion de detener apaga el interruptor maestro`() {
        assertTrue(OrganismController.isEnabled(app))
        val service = Robolectric.buildService(OrganismForegroundService::class.java)
            .create().get()
        val stopIntent = Intent(app, OrganismForegroundService::class.java).apply {
            action = OrganismForegroundService.ACTION_STOP_ORGANISM
        }
        val result = service.onStartCommand(stopIntent, 0, 0)

        assertEquals(Service.START_NOT_STICKY, result)
        assertFalse(OrganismController.isEnabled(app))
    }
}
