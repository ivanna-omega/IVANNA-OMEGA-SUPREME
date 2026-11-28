package com.ivanna.omega.organism

import android.content.Context
import android.util.Log
import androidx.startup.Initializer
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.TimeUnit

private const val TAG = "OrganismFlushWorker"

/**
 * T8/510 — Supervivencia en fondo: WorkManager para flush + sondeo de
 * perfil del Organismo.
 *
 * PROBLEMA: `flushJob` y `pollJob` viven en el `scope` de
 * `OrganismController.start()`; con la app cerrada no hay vaciado de la
 * cola ni sondeo de perfil. Este worker periódico (mínimo del sistema:
 * 15 min, con red) cubre ese hueco sin tocar el controlador.
 *
 * QUÉ HACE CADA EJECUCIÓN (un solo `doWork()`, flush + sondeo juntos):
 *   1. Interruptor maestro: si el Organismo está OFF (`isEnabled()`),
 *      no-op total. Nada sale del teléfono con el interruptor en OFF.
 *   2. Si el controlador vive en ESTE proceso (su `State.status != OFF`),
 *      no-op: su `flushJob` (30 s) y su `pollJob` (30 min) ya cubren
 *      flush + sondeo, y el worker NO debe duplicarlos llamando a
 *      `flushAndPollOnce` encima (dos vaciadores a la vez podrían
 *      mandar el mismo chunk dos veces).
 *   3. Flush + sondeo por la entrada pública del controlador:
 *      `OrganismController.flushAndPollOnce(ctx)` reutiliza
 *      `flushOnce()` + `pollProfileOnce()` con TODAS sus guardas
 *      (dwell, tope, veto, timeout A/B, single-flight, C2, C8,
 *      read-back, confirmación POST). El backoff 30 s → 15 min de la
 *      cola SE REUTILIZA, no se reimplementa.
 *
 * REGLAS DE REINTENTO (no despertar el teléfono en bucle):
 *   * `doWork()` devuelve SIEMPRE `Result.success()`, también con el
 *     servidor caído (`Outcome.RETRY`), con 401, sin token o con
 *     excepción. NUNCA `Result.retry()`: el próximo intento es el
 *     próximo ciclo periódico (15 min), no un backoff de WorkManager
 *     encima del backoff de la cola. Un worker que devolviera retry
 *     con el servidor caído despertaría el teléfono en bucle.
 *   * Sin `setBackoffCriteria` en la request por la misma razón: no
 *     hay reintento de WorkManager que configurar.
 *
 * CICLO DE VIDA (sondeo hijo del mismo ciclo que el flush):
 *   * Flush y sondeo son DOS PASOS DEL MISMO `doWork()`, no dos
 *     trabajos: cancelar el trabajo único (`OrganismWorkScheduler.
 *     cancel()`) cancela ambos a la vez, y no existe un sondeo
 *     huérfano que siga corriendo sin su flush. Es el equivalente
 *     WorkManager de «pollJob hijo del mismo scope que flushJob».
 *   * La programación es única (`enqueueUniquePeriodicWork` +
 *     `ExistingPeriodicWorkPolicy.KEEP`): cada arranque de la app
 *     reintenta programar y KEEP conserva el trabajo ya encolado sin
 *     reiniciar el reloj de 15 min.
 *
 * INTEGRACIÓN T8 (Fase 6, coordinador): la entrada pública
 * `OrganismController.flushAndPollOnce(context)` YA EXISTE (frente C)
 * y es lo que llama este worker; `OrganismController.stop()` YA
 * cancela el trabajo encolado (`OrganismWorkScheduler.cancel`).
 * La APLICACIÓN de un perfil desde fondo sigue diferida por dos
 * razones estructurales documentadas en el controlador: (1)
 * `onApplyProfile` solo lo registra MainActivity con la UI viva —
 * sin él el install falla cerrado (diario PENDING, reintento en el
 * próximo sondeo); (2) el dwell es memoria por proceso: un proceso
 * nacido por el worker no acumula 2 sondeos de la misma versión.
 */
class OrganismFlushWorker(
    appContext: Context,
    params: WorkerParameters
) : CoroutineWorker(appContext, params) {

    override suspend fun doWork(): Result = withContext(Dispatchers.IO) {
        runCatching { doWorkOnce() }.getOrElse { e ->
            // Cualquier excepción (disco, Keystore, red): éxito aparente
            // para no entrar en retry de WorkManager; el próximo ciclo
            // periódico (15 min) lo reintenta. Sin esto, un fallo
            // persistente (p. ej. almacén cifrado no disponible) pondría
            // al worker en backoff de reintentos en bucle.
            Log.w(TAG, "T8: flush de fondo falló (${e.javaClass.simpleName}); no se reintenta dentro del worker", e)
            Result.success()
        }
    }

    private fun doWorkOnce(): Result {
        val ctx = applicationContext
        // (1) Interruptor maestro: OFF = no-op total, sin tocar la red.
        if (!OrganismController.isEnabled(ctx)) {
            Log.i(TAG, "T8: Organismo OFF; el worker no hace nada")
            return Result.success()
        }
        // (2) Controlador vivo en este proceso: él ya vacía y sondea.
        // Su State inicial es OFF, así que un proceso recién nacido por
        // WorkManager (controlador jamás arrancado) cae por el camino
        // de abajo, que es justo el caso de supervivencia en fondo.
        if (OrganismController.state.value.status != OrganismController.Status.OFF) {
            Log.i(TAG, "T8: controlador vivo en este proceso; flush + sondeo ya corren ahí, el worker no duplica")
            return Result.success()
        }
        // (3) Flush + sondeo por la entrada pública del controlador:
        // reutiliza flushOnce() + pollProfileOnce() con TODAS sus
        // guardas (dwell, tope 2/día, veto 24 h, timeout A/B,
        // single-flight, C2, C8, read-back, confirmación POST). Si el
        // controlador no vive en este proceso, la entrada hace un
        // arranque ligero SOLO-transporte (sin mic, sin agregador, sin
        // foreground service, sin UI). La aplicación desde fondo sigue
        // diferida (ver cabecera); el flush y el sondeo con guardas
        // sí corren aquí.
        runCatching { OrganismController.flushAndPollOnce(ctx) }.onFailure { e ->
            Log.w(TAG, "T8: flushAndPollOnce lanzó excepción (${e.javaClass.simpleName}); se reintentará en el próximo ciclo (15 min)")
        }
        return Result.success()
    }

}

/**
 * T8/510 — programación del trabajo periódico del Organismo.
 *
 * `schedule()` es idempotente (KEEP): llamarla en cada arranque no
 * reinicia el reloj de 15 min ni duplica el trabajo. `cancel()` es el
 * reverso para `OrganismController.stop()` (ver cabecera del worker).
 */
object OrganismWorkScheduler {
    /** Nombre único del trabajo periódico (flush + sondeo juntos). */
    const val UNIQUE_WORK_NAME = "organism-flush"

    /** Intervalo de repetición: el mínimo que permite WorkManager. */
    const val REPEAT_INTERVAL_MINUTES = 15L

    fun schedule(context: Context) {
        val request = PeriodicWorkRequestBuilder<OrganismFlushWorker>(
            REPEAT_INTERVAL_MINUTES, TimeUnit.MINUTES
        ).setConstraints(
            Constraints.Builder()
                .setRequiredNetworkType(NetworkType.CONNECTED)
                .build()
        ).build()
        runCatching {
            WorkManager.getInstance(context.applicationContext)
                .enqueueUniquePeriodicWork(
                    UNIQUE_WORK_NAME,
                    ExistingPeriodicWorkPolicy.KEEP,
                    request
                )
        }.onFailure { e ->
            Log.w(TAG, "T8: no se pudo programar el trabajo periódico (${e.javaClass.simpleName})")
        }
    }

    fun cancel(context: Context) {
        runCatching {
            WorkManager.getInstance(context.applicationContext)
                .cancelUniqueWork(UNIQUE_WORK_NAME)
        }.onFailure { e ->
            Log.w(TAG, "T8: no se pudo cancelar el trabajo periódico (${e.javaClass.simpleName})")
        }
    }
}

/**
 * T8/510 — Initializer de androidx.startup: programa el trabajo
 * periódico en cada arranque del proceso de la app, sin tocar
 * `IVANNAApplication`, `MainActivity` ni `OrganismController` (fuera
 * del alcance de este frente). La entrada en el manifest es un
 * `<meta-data android:value="androidx.startup">` bajo el
 * `InitializationProvider` (con `tools:node="merge"`); sin ella esta
 * clase existe pero nunca se ejecuta y el trabajo jamás se programa.
 * `create()` no arranca el controlador, no toca el micrófono ni envía
 * nada: solo encola el trabajo (KEEP), que luego decide por sí mismo
 * en cada ejecución si el interruptor está ON y el controlador muerto.
 */
class OrganismWorkSchedulerInitializer : Initializer<Unit> {
    override fun create(context: Context) {
        OrganismWorkScheduler.schedule(context)
    }

    override fun dependencies(): List<Class<out Initializer<*>>> = emptyList()
}
