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
import java.io.File
import java.util.concurrent.TimeUnit

private const val TAG = "OrganismFlushWorker"

/**
 * Nombre del directorio de la cola durable en filesDir. DEBE coincidir
 * con el que usa `OrganismController.start()` (`File(ctx.filesDir,
 * "organism-queue")`): el worker abre LA MISMA cola, no una copia.
 */
private const val QUEUE_DIR_NAME = "organism-queue"

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
 *      flush + sondeo, y el worker NO debe duplicarlos — el worker
 *      construye sus PROPIAS instancias de `OrganismQueue` /
 *      `OrganismSender` (el `@Synchronized` de `flush()` es por
 *      instancia, no entre instancias) y dos vaciadores a la vez
 *      podrían mandar el mismo chunk dos veces.
 *   3. Flush: construye `OrganismQueue` + `OrganismTokenStore` +
 *      `OrganismSender` sobre el `applicationContext` (las tres piezas
 *      ya son independientes de Activity) y llama `sender.flush()`.
 *      El backoff 30 s → 15 min de la cola SE REUTILIZA, no se
 *      reimplementa: `nextReady()` solo devuelve RETRY vencidos y
 *      `flush()` se detiene al primer RETRY.
 *   4. Sondeo de perfil: `ProfileFetcher.fetch(appliedVersion)` — SOLO
 *      COMPROBACIÓN DE VERSIÓN, nunca aplicación (ver BLOQUEADOR abajo).
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
 * BLOQUEADOR — POR QUÉ EL WORKER NO APLICA PERFILES (no es olvido):
 *   * `OrganismController.flushOnce()` y `pollProfileOnce()` son
 *     PRIVADOS: no hay punto de entrada público para «vacía + sondea»
 *     y este frente tiene prohibido modificar el controlador.
 *   * La APLICACIÓN de un perfil no es un GET: pasa por dwell (2
 *     sondeos), tope diario (2/día), veto de 24 h, timeout del A/B,
 *     single-flight, validación C2, diario C8, read-back por
 *     `onApplyProfile` (puente que solo registra MainActivity con la
 *     UI viva) y confirmación POST — TODO privado dentro del
 *     controlador. Duplicar esa lógica en el worker sería un segundo
 *     aplicador divergente; aplicarla a medias (p. ej. solo
 *     `ProfileApplier.applyAndCommit`) rompería el corral T6.
 *   * Lo que SÍ hace el worker: si el GET trae una versión nueva, lo
 *     registra en el Log y la APLICACIÓN queda para el próximo
 *     arranque/sondeo del controlador (que sondea a los 60 s de
 *     arrancar). La versión aplicada en memoria del controlador no
 *     sobrevive al proceso, así que con el controlador muerto el
 *     worker pasa `appliedVersion = null` y el GET devuelve el perfil
 *     completo como `Current` (no `NotModified`); aun así NO se
 *     descarga nada más que el sobre (~2 KB) y NO se aplica.
 *   * Para un T8 completo (flush + sondeo + aplicación en fondo) hace
 *     falta que el frente dueño del controlador exponga UNA entrada
 *     pública (p. ej. `fun flushAndPollOnce(context: Context)`) que
 *     reutilice `flushOnce()` + `pollProfileOnce()` con sus guardas.
 *
 * PENDIENTE DE INTEGRACIÓN (una línea, frente dueño del controlador):
 *   * `OrganismController.stop()` debe llamar
 *     `OrganismWorkScheduler.cancel(context)` para que apagar el
 *     interruptor cancele también el trabajo encolado. Mientras tanto
 *     el worker con el interruptor OFF es no-op por la guarda (1) —
 *     no hay tráfico, solo un despertar cada 15 min que retorna al
 *     instante — pero el trabajo sigue encolado.
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
        // (3) Flush con las piezas independientes de Activity. La cola
        // es LA MISMA que la del controlador (mismo directorio en
        // filesDir): los chunks encolados con la app abierta se vacían
        // aquí, y viceversa. El backoff vive en la cola (markRetry):
        // un RETRY no vencido no sale de nextReady() y flush() se
        // detiene al primer RETRY, así que un servidor caído no se
        // golpea en bucle dentro de esta ejecución.
        val queue = OrganismQueue(File(ctx.filesDir, QUEUE_DIR_NAME))
        val tokens = OrganismTokenStore(ctx)
        val sender = OrganismSender(tokens, queue)
        val flush = runCatching { sender.flush() }.getOrNull()
        if (flush == null) {
            Log.w(TAG, "T8: flush lanzó excepción; se reintentará en el próximo ciclo (15 min)")
            return Result.success()
        }
        Log.i(
            TAG,
            "T8: flush de fondo: enviados=${flush.sent} reintentados=${flush.retried} " +
                "cuarentena=${flush.quarantined} outcome=${flush.outcome}"
        )
        // (4) Sondeo de perfil: solo comprobación de versión (ver
        // BLOQUEADOR en la cabecera: la aplicación sigue siendo del
        // controlador). Con el controlador muerto no hay versión
        // aplicada en memoria (null): el GET trae Current si hay
        // perfil, y solo se registra, no se aplica.
        checkProfileVersion(tokens)
        return Result.success()
    }

    /**
     * GET de versión al endpoint de perfil. No aplica, no confirma, no
     * escribe nada: solo deja constancia en el Log de si hay versión
     * nueva. Todos los resultados son `Result.success()` en el llamador:
     * un RETRY (red/429/5xx) espera al próximo ciclo periódico.
     */
    private fun checkProfileVersion(tokens: TokenStore) {
        val appliedVersion = OrganismController.state.value.appliedProfileVersion
            .takeIf { it > 0L }
        val fetcher = ProfileFetcher(tokens)
        val result = runCatching { fetcher.fetch(appliedVersion) }.getOrNull()
        when (result) {
            null -> Log.w(TAG, "T8: sondeo de perfil lanzó excepción; próximo ciclo")
            is ProfileFetcher.Result.Current -> Log.i(
                TAG,
                "T8: perfil nuevo en el servidor: v${result.version} " +
                    "(${result.status}/${result.abState}); lo aplica el controlador en el " +
                    "próximo arranque/sondeo (dwell + tope diario + veto)"
            )
            is ProfileFetcher.Result.NotModified ->
                Log.i(TAG, "T8: perfil al día (versión aplicada v$appliedVersion)")
            is ProfileFetcher.Result.Retry ->
                Log.i(TAG, "T8: sondeo reintentable (red/429/5xx); próximo ciclo, sin reintento dentro del worker")
            is ProfileFetcher.Result.TokenInvalid ->
                Log.w(TAG, "T8: sondeo: token inválido (401); hay que reautorizar una vez")
            is ProfileFetcher.Result.NoToken ->
                Log.i(TAG, "T8: sondeo: sin token; nada que comprobar")
            is ProfileFetcher.Result.Quarantine ->
                Log.i(TAG, "T8: sondeo en cuarentena: ${result.reason}")
        }
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
