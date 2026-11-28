package com.ivanna.omega.organism

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.ivanna.omega.R

/**
 * OrganismForegroundService — servicio en primer plano (microphone) del
 * Organismo (v2.5.510, fix del «nada» de la 509).
 *
 * DIAGNÓSTICO EN VIVO (2026-11-28 06:58 CST, verificado por SSH): el
 * Organismo NO tenía servicio en primer plano. Cuando Esse sale de la
 * app, Android congela el proceso (do_freezer_trap) y silencia el
 * micrófono (RealAcousticSensor, sesión 473, silenced:true), así que el
 * Organismo no captura ni envía nada. El permiso RECORD_AUDIO estaba
 * concedido y organism_enabled=true, pero sin foreground service la
 * captura muere al salir de la app.
 *
 * FIX: este servicio NO hace trabajo pesado por sí mismo (no captura,
 * no agrega, no envía): su único trabajo es mantener el proceso en
 * primer plano para que la captura (RealAcousticSensor) y el envío
 * (flush de OrganismController) sigan corriendo en segundo plano. Un
 * servicio en primer plano eleva TODO el proceso, no solo sus hilos.
 * Sigue el patrón de BridgePlaybackService (mismo paquete de audio,
 * canal propio, notificación ongoing, arranque idempotente con
 * runCatching).
 *
 * HIJO DEL INTERRUPTOR MAESTRO: solo vive mientras el organismo está
 * enabled. OrganismController.start() lo arranca y
 * OrganismController.stop() lo detiene (al final de stop(), para que
 * el proceso siga en primer plano mientras se cierra la sesión y el
 * chunk final queda durable en la cola). La acción «Detener organismo»
 * de la notificación apaga el interruptor maestro
 * (OrganismController.setEnabled(false)), no solo el servicio: si solo
 * se parara el servicio, el próximo arranque en frío lo resucitaría
 * porque la preferencia seguiría en ON.
 *
 * TIPO microphone (Android 14+, targetSdk 35): exige el permiso
 * FOREGROUND_SERVICE_MICROPHONE y foregroundServiceType="microphone"
 * en el Manifest (ambos declarados). Sin RECORD_AUDIO concedido no se
 * arranca (fail-closed, sin crash): el organismo corre como antes,
 * sin primer plano, hasta que se conceda el permiso y se re-arranque
 * (start() re-asegura el servicio en cada llamada, incluido el
 * early-return de scope != null).
 *
 * START_NOT_STICKY (a propósito, distinto de BridgePlaybackService):
 * si el sistema mata el proceso, el organismo muere con él y solo
 * vuelve en el próximo arranque en frío (PersistedStateRestorer),
 * que es la vía única de arranque. Un servicio STICKY resucitaría
 * un primer plano zombi con el controlador muerto.
 *
 * ARRANQUE DESDE SEGUNDO PLANO (Android 12+): arrancar un FGS desde
 * segundo plano está restringido. El camino normal es en primer plano
 * (apertura de la app / toggle en la UI), pero PersistedStateRestorer
 * también corre en Application.onCreate, que puede darse en un
 * arranque en segundo plano (p. ej. BOOT_COMPLETED). Por eso start()
 * va con runCatching: si el sistema lo rechaza se registra y se
 * reintenta en la próxima apertura; sin crash y sin regresión (el
 * organismo queda como estaba en la 509 hasta entonces).
 */
class OrganismForegroundService : Service() {

    companion object {
        private const val TAG = "OrganismForegroundService"
        const val CHANNEL_ID = "ivanna_organism_channel"
        /** IDs en uso: 2 y 3 (PlaybackCaptureService), 4 (BridgePlaybackService). */
        const val NOTIFICATION_ID = 5
        const val ACTION_STOP_ORGANISM = "com.ivanna.omega.organism.ACTION_STOP_ORGANISM"

        @Volatile private var isRunning = false

        /** Arranca el servicio en primer plano (idempotente). */
        fun start(context: Context) {
            if (isRunning) return
            // Sin RECORD_AUDIO, un FGS tipo microphone es rechazado por
            // el sistema (SecurityException en Android 14+): no se
            // arranca y el organismo sigue sin primer plano (fail-closed).
            if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) !=
                PackageManager.PERMISSION_GRANTED
            ) {
                Log.w(TAG, "Sin permiso RECORD_AUDIO: no se arranca el foreground service del organismo")
                return
            }
            runCatching {
                val intent = Intent(context, OrganismForegroundService::class.java)
                ContextCompat.startForegroundService(context, intent)
            }.onFailure { e ->
                Log.w(TAG, "No se pudo arrancar el foreground service del organismo", e)
            }
        }

        /** Detiene el servicio (idempotente). */
        fun stop(context: Context) {
            if (!isRunning) return
            runCatching {
                context.stopService(Intent(context, OrganismForegroundService::class.java))
            }.onFailure { e ->
                Log.w(TAG, "No se pudo detener el foreground service del organismo", e)
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP_ORGANISM) {
            // Acción de la notificación: apaga el interruptor maestro
            // (persiste OFF y detiene controlador + servicio).
            runCatching { OrganismController.setEnabled(this, false) }
            stopSelf()
            return START_NOT_STICKY
        }
        startForeground(
            NOTIFICATION_ID, buildNotification(),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        )
        isRunning = true
        Log.i(TAG, "Servicio en primer plano activo (microphone) — el organismo sigue en segundo plano")
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        isRunning = false
        Log.i(TAG, "Servicio en primer plano del organismo detenido")
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                CHANNEL_ID, "IVANNA Organismo", NotificationManager.IMPORTANCE_LOW
            ).apply { setSound(null, null); enableVibration(false) }
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .createNotificationChannel(ch)
        }
    }

    private fun buildNotification(): Notification {
        val openApp = PendingIntent.getActivity(
            this, 0,
            Intent(this, Class.forName("com.ivanna.omega.MainActivity")),
            PendingIntent.FLAG_IMMUTABLE
        )
        val stopOrganism = PendingIntent.getService(
            this, 0,
            Intent(this, OrganismForegroundService::class.java).apply {
                action = ACTION_STOP_ORGANISM
            },
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("IVANNA OMEGA SUPREME")
            .setContentText("Organismo activo — el micrófono sigue escuchando en segundo plano")
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentIntent(openApp)
            .addAction(0, "Detener organismo", stopOrganism)
            .setOngoing(true)
            .build()
    }
}
