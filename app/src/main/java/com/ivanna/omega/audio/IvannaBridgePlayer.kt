package com.ivanna.omega.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.AudioTimestamp
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import android.os.Build
import android.os.Process
import android.os.SystemClock
import android.util.Log
import com.ivanna.omega.ai.PerceptualState
import com.ivanna.omega.ai.PerceptualStateListener
import com.ivanna.omega.dsp.DSPBridge
import com.ivanna.omega.dsp.AdaptiveDSPState
import com.ivanna.omega.audio.AudioRoutingManager
import com.ivanna.omega.neuromorphic.IvannaNpeEngine
import com.ivanna.omega.neuromorphic.IvannaNpeNative
import com.ivanna.omega.audio.effects.NeuromorphicProcessingEngine
import com.ivanna.omega.audio.effects.VolterraH2Processor
import com.ivanna.omega.audio.VolterraSwitch
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.OEMTelemetry
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.util.concurrent.Executors
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * IvannaBridgePlayer — reproductor propio de la app.
 *
 * POR QUÉ EXISTE ESTE ARCHIVO:
 *   Hasta ahora el pipeline nativo completo (DSPBridge → ParametricEQ →
 *   Compressor → HarmonicExciter → StereoWidener → PDEngine/NHO/Spatial →
 *   Kernel Evolutivo) solo tenía un camino real hacia el altavoz:
 *   AudioPipeline (loop mic → DSP → altavoz, pensado para "escuchar el
 *   mundo" en vivo, no para reproducir música).
 *
 *   Para apps de terceros (Spotify, YouTube, etc.) IvannaGlobalEffectManager
 *   solo puede enganchar efectos de stock de Android (Equalizer, BassBoost,
 *   Virtualizer, LoudnessEnhancer, DynamicsProcessing) — Android no permite
 *   inyectar DSP custom en el proceso de audio de otra app sin root. Todo
 *   el trabajo fino en C++ (NHO, HRTF, Kernel Evolutivo) queda invisible
 *   para el usuario que solo escucha Spotify.
 *
 *   IvannaBridgePlayer decodifica archivos locales (MediaExtractor +
 *   MediaCodec → PCM), pasa cada bloque por DSPBridge.process() —el mismo
 *   camino ya afinado y con los fixes de PDEngine/Kernel Evolutivo— y
 *   escribe el resultado a un AudioTrack propio. Es la única forma real
 *   de que todo el motor suene con música de verdad, no con el micrófono.
 *
 * FIXES v3.6:
 *   - pausa/reanudación reales en AudioTrack (sin seguir drenando buffer)
 *   - soporte correcto para salida PCM_FLOAT y PCM_16BIT del decoder
 *   - buffers reutilizables para evitar allocs por chunk
 *   - release() explícito del clasificador de Voice Protection
 */
// ── AUDIT FIX PR 1: IvannaBridgePlayer ahora recibe cambios de PerceptualCortex ──
class IvannaBridgePlayer(private val context: Context) : PerceptualStateListener {

    // FIX (issue 5 — conflicto de foco con Volterra/switches externos):
    // antes NO existía gestión de AudioFocus alguna para este reproductor
    // (AudioCallbackManager estaba definida pero jamás instanciada en todo
    // el proyecto). Ahora se solicita foco cooperativo al iniciar playback
    // y se PAUSA (no se destruye el track) en pérdida transitoria,
    // reanudando solo si el usuario no había pausado manualmente.
    // Puerta Hi-Fi USB (0030, v2.5.459): `encoding` es el formato del
    // AudioTrack (PCM_FLOAT en la ruta actual, PCM_24_BIT_PACKED en la
    // ruta direct); `direct` dice si el track va por direct_pcm (sin
    // mezclador) o por primary output. Solo USB puede ser direct; BT y
    // altavoz quedan siempre en la ruta actual (16-bit/48k por hardware).
    private data class OutputRoute(
        val sampleRate: Int,
        val preferredDevice: AudioDeviceInfo?,
        val encoding: Int = AudioFormat.ENCODING_PCM_FLOAT,
        val direct: Boolean = false
    )

    /**
     * Chooses the app-owned AudioTrack output clock. USB keeps priority; A2DP uses
     * the selected device's negotiated codec rate when Android exposes it. This
     * does not control the remote sink clock or the Qualcomm vendor HAL.
     */
    private fun selectOutputRoute(): OutputRoute {
        val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val outputs = runCatching {
            audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        }.getOrNull().orEmpty()

        val usb = outputs.firstOrNull {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                it.type == AudioDeviceInfo.TYPE_USB_HEADSET ||
                it.type == AudioDeviceInfo.TYPE_USB_ACCESSORY
        }
        if (usb != null) {
            // Puerta Hi-Fi USB (0030, v2.5.459): si el flag está ON y el SO
            // anuncia un perfil direct 24-bit para este DAC, la ruta es
            // direct_pcm (sin mezclador, sin remuestreo). El DAC de Esse
            // fue verificado en vivo (2026-11-16 13:29 CST) anunciando
            // PCM_24_BIT_PACKED @ 44100/48000. Si no hay perfil direct,
            // se cae a la ruta float actual (fail-closed, con log).
            // El probe lee los formatos del propio `usb` (AudioDeviceInfo).
            if (hiresDirectUsbEnabled && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val direct = runCatching {
                    DirectRouteProbe.select(DirectRouteProbe.query(usb), usb.type)
                }.getOrNull()
                // FIX 5 (v2.5.465): la ruta direct puede anunciar 96 kHz
                // o más en otro DAC; la cadena DSP del reproductor solo
                // está presupuestada a ≤48 kHz (ver TARGET_SAMPLE_RATE).
                // Un direct >48 kHz se ignora (fail-closed a float 48 kHz,
                // con log) en vez de correr el DSP al doble de coste. El
                // DAC de Esse anuncia 44100/48000, así que su direct
                // 24-bit/48 kHz no cambia.
                val clampedDirect = clampDirectRouteForDsp(direct)
                if (clampedDirect != null) {
                    Log.i(TAG, "USB Hi-Fi direct: ${usb.productName}; encoding=${clampedDirect.encoding}; rate=${clampedDirect.sampleRate} Hz (direct_pcm, sin mezclador)")
                    return OutputRoute(clampedDirect.sampleRate, usb, clampedDirect.encoding, direct = true)
                }
                if (direct != null) {
                    Log.i(TAG, "USB Hi-Fi direct a ${direct.sampleRate} Hz ignorado (>48 kHz, DSP del reproductor a 48 kHz); ruta float/mezclador")
                } else {
                    Log.i(TAG, "USB Hi-Fi direct no disponible para ${usb.productName}; ruta float/mezclador")
                }
            }
            val rates = usb.sampleRates
            val rate = selectBridgePlayerRate(rates)
            Log.i(TAG, "USB audio output selected: ${usb.productName}; rate=${rate} Hz; advertised=${rates.contentToString()}")
            return OutputRoute(rate, usb)
        }

        val bluetooth = outputs.firstOrNull {
            it.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                it.type == AudioDeviceInfo.TYPE_BLE_HEADSET ||
                it.type == AudioDeviceInfo.TYPE_BLE_SPEAKER
        }
        if (bluetooth != null) {
            // Never fall back to the first connected headset when Android does not expose
            // a stable address for this output; that could apply another device's codec rate.
            val codec = if (
                bluetooth.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP &&
                BluetoothOutputRatePolicy.hasReliableDeviceAddress(bluetooth.address)
            ) {
                runCatching { BluetoothCodecDetector.detectActiveCodec(context, bluetooth.address) }.getOrNull()
            } else null
            val codecRate = codec?.sampleRateHz ?: 0
            val candidateRate = BluetoothOutputRatePolicy.select(codecRate)
            val candidateMinBuffer = AudioTrack.getMinBufferSize(
                candidateRate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT
            )
            // FIX 5 (v2.5.465): la política BT puede devolver 88.2/96 kHz;
            // la cadena DSP del reproductor solo está presupuestada a
            // ≤48 kHz (ver TARGET_SAMPLE_RATE). Se capa a 48 kHz.
            val rate = selectBridgePlayerRate(
                intArrayOf(if (candidateMinBuffer > 0) candidateRate else BluetoothOutputRatePolicy.FALLBACK_RATE_HZ)
            )
            Log.i(TAG, "Bluetooth output selected: ${bluetooth.productName}; codec=${codec?.codec ?: "BLE/unknown"}; negotiatedRate=${codecRate}Hz; trackRate=${rate}Hz")
            return OutputRoute(rate, bluetooth)
        }
        return OutputRoute(TARGET_SAMPLE_RATE, null)
    }

    /**
     * Construye el AudioTrack de una ruta (0030, puerta Hi-Fi USB).
     * El encoding viene de la ruta: PCM_24_BIT_PACKED en direct,
     * PCM_FLOAT en la ruta actual. null si el track no inicializa (el
     * llamador hace fallback a float); el track fallido se libera aquí.
     * El colchón ×12 es el de la F2 v2.5.452 (medido en vivo 2026-11-15,
     * StageTimings): HOLOS media 6–8.5 ms sola supera el presupuesto de
     * 6.67 ms/bloque (p99 17–48 ms) y el DSP total va ~2× sobre
     * presupuesto; un solo pico de 142 ms vaciaba el buffer de 160 ms
     * (7696 frames) → underrun → micro-corte. Con ×12 (320 ms) el
     * colchón absorbe los picos del DSP y el gobernador de shed (que
     * acota la carga media) evita el drenado sostenido. Latencia mayor
     * (~320 ms) pero aceptable en un reproductor de música (no es
     * monitoreo en vivo). El servo PLL se adapta solo
     * (objetivo = capacidad/2). En DIRECT el buffer lo fija el HAL; se
     * mantiene el mismo ×12 hasta medir xruns en la ruta direct.
     */
    private fun buildAudioTrack(route: OutputRoute, sampleRate: Int): AudioTrack? {
        val channelMask = AudioFormat.CHANNEL_OUT_STEREO
        val minBuf = AudioTrack.getMinBufferSize(sampleRate, channelMask, route.encoding)
        if (minBuf <= 0) {
            Log.e(TAG, "AudioTrack no soporta $sampleRate Hz encoding=${route.encoding}")
            return null
        }
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(route.encoding)
                    .setSampleRate(sampleRate)
                    .setChannelMask(channelMask)
                    .build()
            )
            .setBufferSizeInBytes(minBuf * 12)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        route.preferredDevice?.let { preferred ->
            if (!track.setPreferredDevice(preferred)) {
                Log.w(TAG, "Android rechazó la ruta preferida ${preferred.productName}; se conserva la ruta elegida por AudioPolicy")
            }
        }
        if (track.state != AudioTrack.STATE_INITIALIZED) {
            Log.w(TAG, "AudioTrack no inicializó (encoding=${route.encoding}, rate=$sampleRate)")
            runCatching { track.release() }
            return null
        }
        return track
    }

    private val focusManager by lazy {
        AudioCallbackManager(
            context.getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
        ) { focusChange ->
            when (focusChange) {
                android.media.AudioManager.AUDIOFOCUS_LOSS,
                android.media.AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                    if (state == State.PLAYING) {
                        pauseRequested = true
                        // FIX (tronido al cambiar de ventana): pause() en seco y
                        // setVolume escalonado cortaban la onda en medio de un
                        // ciclo. Fade corto a 0 antes de pausar.
                        fadeVolume(0f, 40) { runCatching { audioTrack?.pause() } }
                        state = State.PAUSED
                    }
                }
                android.media.AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                    fadeVolume(0.3f, 80)
                }
                android.media.AudioManager.AUDIOFOCUS_GAIN -> {
                    if (state == State.PAUSED && pauseRequested) {
                        runCatching { audioTrack?.setVolume(0f) }
                        resume()
                    }
                    fadeVolume(1.0f, 80)
                }
            }
        }
    }

    // Rampa lineal de volumen del AudioTrack (evita escalones = clicks).
    @Volatile private var fadeToken = 0
    private fun fadeVolume(target: Float, ms: Int, onDone: () -> Unit = {}) {
        val token = ++fadeToken
        val track = audioTrack ?: run { onDone(); return }
        Thread {
            val steps = (ms / 5).coerceAtLeast(1)
            var from = 1.0f
            // Volumen inicial real no legible vía API: se asume el último destino.
            from = lastVolume
            for (i in 1..steps) {
                if (token != fadeToken) return@Thread
                val v = from + (target - from) * i / steps
                runCatching { track.setVolume(v) }
                try { Thread.sleep(5) } catch (_: InterruptedException) { return@Thread }
            }
            lastVolume = target
            onDone()
        }.start()
    }
    @Volatile private var lastVolume = 1.0f

    companion object {
        private const val TAG = "IVANNA.BridgePlayer"
        private const val TIMEOUT_US = 10_000L
        // FIX 5 (v2.5.465, reproductor tronando + volumen sube/baja): la
        // tasa DSP del reproductor era 96 kHz. TODA la cadena (HOLOS STFT,
        // Φ, Volterra, RIR, EvoEQ 512, NPE) está medida y presupuestada a
        // 48 kHz (StageTimings 2026-11-15: HOLOS sola 6–8.5 ms vs
        // presupuesto 6.67 ms/bloque, DSP total ~2× sobre presupuesto, p99
        // 17–48 ms) y el DAC de Esse solo anuncia 24-bit @ 44100/48000
        // (ALSA card 1, verificado en vivo 2026-11-16): a 96 kHz el coste
        // por segundo se duplica, el periodo por bloque de 2048 frames
        // (21.3 ms) queda por debajo del coste real de la cadena y el
        // gobernador de shed suelta bloques a seco de forma crónica
        // (procesado↔seco por bloque = tronido + saltos de nivel), y el
        // trim LUFS corría al doble de dB/s. A 48 kHz el bloque de 2048
        // frames dura 42.7 ms y la cadena cabe en su presupuesto.
        private const val TARGET_SAMPLE_RATE = BRIDGE_PLAYER_DSP_SAMPLE_RATE_HZ
        private const val MAX_CHUNK_FRAMES = 2048

        /** Última instancia activa — usada por PiLstmBridge para AGC seguro. */
        @Volatile var activeInstance: IvannaBridgePlayer? = null

        /**
         * Puerta Hi-Fi USB (0030, v2.5.459): default ON — el DAC de Esse fue
         * verificado en vivo (2026-11-16 13:29 CST) anunciando
         * PCM_24_BIT_PACKED @ 44100/48000, así que la condición "hasta
         * medir" del diseño ya se cumplió; y Esse ordenó activarla en
         * caliente (2026-11-16 13:30 CST). Si algo falla (sin perfil
         * direct, track no inicializa), el fallback a float/mezclador es
         * silencioso (fail-closed, con log). Poner en false para
         * desactivar la puerta (vuelve la ruta float siempre).
         */
        @Volatile var hiresDirectUsbEnabled = true
    }

    enum class State { IDLE, PLAYING, PAUSED, STOPPED, ERROR }

    // ── Estado explícito del ciclo de vida de MediaCodec / AudioTrack ──
    // Independiente de State (estado lógico hacia la UI). CodecState
    // refleja qué llamadas son LEGALES sobre los recursos nativos:
    //   IDLE      → no hay recursos asignados
    //   CONFIGURED→ codec.configure() ok, aún sin start()
    //   RUNNING   → codec.start() + track.play() en curso
    //   STOPPED   → codec ya arrancó y ahora está detenido
    // codec.stop() sólo se invoca desde RUNNING; codec.start() sólo desde
    // CONFIGURED/STOPPED. Toda transición va bajo codecMutex — elimina el
    // IllegalStateException clásico al llamar play()/stop() repetido o
    // fuera de estado válido.
    private enum class CodecState { IDLE, CONFIGURED, RUNNING, STOPPED }

    @Volatile
    private var codecState: CodecState = CodecState.IDLE

    // Serializa todas las transiciones de codecState: play()/stop()/
    // release() concurrentes no pueden solapar un codec.start() con un
    // codec.stop()/release() de otro hilo.
    private val codecMutex = Mutex()

    @Volatile var state: State = State.IDLE
        private set

    private val scope = CoroutineScope(Dispatchers.Default + SupervisorJob())

    // v2.5.423: hilo de audio DEDICADO con prioridad URGENT_AUDIO.
    // El bucle decode+DSP+write corría en Dispatchers.Default (prioridad
    // normal, pool compartido): el sistema lo expropiaba y HOLOS llegaba a
    // p99 15-23 ms (presupuesto 6.67 ms/bloque) → underruns → cortes a
    // silencio (verificado en vivo 2026-11-12 14:55, xruns 834→1292, salida
    // rms=0.00). Con prioridad urgente el hilo de audio expropia a lo
    // demás, no al revés. La prioridad se fija DENTRO del Runnable del
    // hilo (no en el factory: ahí corre el hilo que crea, no el creado).
    // Se cierra en release(); el buffer sube a minBuf×6 (la 423 NO paró los
    // cortes: verificado en vivo 2026-11-12 16:54, xruns=3549, HOLOS p99
    // 10.4 ms, cola llena occ 16522 vs objetivo 7696 — el hilo urgente no
    // movió ni la cola ni los picos; el cuello es el cómputo del DSP, y el
    // colchón mayor absorbe los picos. Si los cortes siguen, el siguiente
    // paso es atacar los picos de HOLOS mismos).
    private val audioDispatcher = Executors.newSingleThreadExecutor { r ->
        Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            r.run()
        }, "IvannaAudio")
    }.asCoroutineDispatcher()
    private var job: Job? = null
    private var audioTrack: AudioTrack? = null
    // Puerta Hi-Fi USB (0030): true mientras el track vivo va por
    // direct_pcm + PCM_24_BIT_PACKED. Lo lee el poller de telemetría
    // (hires_direct=1 en la línea IVANNA.Telemetry). Se fija al construir
    // el track y se limpia en releaseTrackLocked().
    @Volatile private var hiresDirectActive = false
    // Session-scoped counters used only for diagnostic queue-latency telemetry.
    @Volatile private var submittedFramesForMetrics: Long = 0L
    // Último underrunCount publicado a OEMTelemetry: el contador de
    // AudioTrack es ACUMULADO, así que solo el delta alimenta el contador
    // de OEMTelemetry (reportUnderrun() suma de uno en uno).
    @Volatile private var lastUnderrunCountForOem: Int = 0
    // Separate ITD/crossfeed history from the 48 kHz system-wide capture path.
    private val playbackSpatialEngine = com.ivanna.omega.spatial.IvannaSpatialEngine.createIsolated()
    @Volatile private var voiceProtectionEnabled = true

    @Volatile private var pauseRequested = false
    @Volatile private var stopRequested = false
    private val analyzeTickCounter = java.util.concurrent.atomic.AtomicInteger(0)

    // --- Posición y duración reales para la barra de progreso ---
    private val _currentPositionMs = MutableStateFlow(0L)
    val currentPositionMs: StateFlow<Long> = _currentPositionMs.asStateFlow()

    private val _durationMs = MutableStateFlow(0L)
    val durationMs: StateFlow<Long> = _durationMs.asStateFlow()

    // seekTarget: -1 = sin seek pendiente. seekTo() escribe aquí; el loop lo consume.
    @Volatile private var seekTargetUs = -1L

    // ── PARCHE 1: Puente de métricas agregadas ───────────────────────────────
    private val _omegaMetrics = MutableStateFlow(OmegaMetrics())
    val omegaMetrics: StateFlow<OmegaMetrics> = _omegaMetrics.asStateFlow()


    /**
     * Estima la cola pendiente del AudioTrack (frames aceptados por la app menos
     * frames ya presentados según AudioTimestamp). No es una medición acústica
     * del retardo del altavoz/DAC: ese requiere loopback físico calibrado.
     */
    private fun estimateQueuedOutputLatencyMs(): Float {
        return try {
            val track = audioTrack ?: return 0f
            val rate = track.sampleRate.takeIf { it > 0 } ?: return 0f
            val timestamp = AudioTimestamp()
            if (!track.getTimestamp(timestamp)) return 0f

            val submitted = submittedFramesForMetrics
            val queuedFrames = (submitted - timestamp.framePosition).coerceAtLeast(0L)
            (queuedFrames * 1_000f / rate).coerceAtLeast(0f)
        } catch (_: Throwable) {
            0f
        }
    }

    // Solo lo tocan el decode loop y pollOmegaMetrics(), que corre dentro del mismo bucle.
    @Volatile private var predictiveAnalyzer: PredictiveAudioAnalyzer? = null

    private fun pollOmegaMetrics() {
        try {
            val pipelineState = IvannaUnifiedPipeline.state.value

            val dspActive = IvannaNativeLib.isLoaded &&
                try { IvannaNativeLib.nativeIsAdaptiveEngineRunning() } catch (_: Throwable) { false }

            val hrtfActive = try {
                IvannaNativeLib.isLoaded &&
                IvannaNativeLib.nativeGetSpatialState().contains("HRTF_ON")
            } catch (_: Throwable) { false }

            val clipCount = try {
                if (IvannaNativeLib.isLoaded) IvannaNativeLib.nativeGetClipCount() else 0
            } catch (_: Throwable) { 0 }

            // FIX: telemetry[3] es target_gain (0..2), no CPU: antes se mostraba la ganancia
            // objetivo como "% de CPU". Sin fuente nativa real de CPU → 0 = sin medición.
            val cpuPercent = 0f

            val yamnetCategory = try {
                if (IvannaNpeEngine.isReady) IvannaNpeNative.nativeGetDetectedGenre() else "—"
            } catch (_: Throwable) { "—" }

            val yamnetConfidence = try {
                if (IvannaNpeEngine.isReady) {
                    val sig = IvannaNpeNative.nativeGetSynthClassify()
                    if (sig.size > 1) sig[1] else 0f  // [cluster_id, confidence, ...]: antes leía el id de cluster
                } else 0f
            } catch (_: Throwable) { 0f }

            val predictive = predictiveAnalyzer?.snapshot() ?: PredictiveSnapshot()
            OmegaMetrics.updateSharedPredictive(predictive.valid, predictive.confidence, predictive.predictionGainDb, predictive.transient)

            // Underruns del AudioTrack propio (v2.5.411, tarea 4): la captura
            // ya publica los suyos en OmegaMetrics.shared vía
            // updateRefinedTelemetry; el BridgePlayer no publicaba ninguno,
            // así que el contador de xruns de la telemetría 1 Hz se quedaba
            // en el último valor de la captura (o en 0). Total acumulado
            // desde que se creó el track (API 24+); el poller calcula el
            // delta por segundo y clampa a 0 al recrearse el track. NO hay
            // AAudio en la app: AudioTrack.getUnderrunCount() es el contador
            // equivalente (verificado por grep: ningún AAudioStream en el
            // código; el I/O es AudioTrack/AudioRecord).
            runCatching {
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
                    val underruns = audioTrack?.underrunCount ?: 0
                    OmegaMetrics.updateRefinedTelemetry(underrunCount = underruns)
                    // F2 / cortes BridgePlayer (2026-11-14): el snapshot del
                    // puente leía underruns de OEMTelemetry, pero
                    // reportUnderrun() tampoco tenía llamadores: el snapshot
                    // decía 0 aunque hubiera xruns. Se publica el DELTA por
                    // poll (el contador es acumulado y se resetea a 0 al
                    // recrearse el track: el delta negativo se clampa a 0).
                    val delta = (underruns - lastUnderrunCountForOem).coerceAtLeast(0)
                    repeat(delta) { OEMTelemetry.reportUnderrun() }
                    lastUnderrunCountForOem = underruns
                }
            }

            _omegaMetrics.value = OmegaMetrics(
                predictiveValid      = predictive.valid,
                predictiveConfidence = predictive.confidence,
                predictiveGainDb     = predictive.predictionGainDb,
                predictiveTransient  = predictive.transient,
                rmsLevel         = pipelineState.rms,
                peakLevel        = pipelineState.peak,
                clipCount        = clipCount,
                cpuPercent       = cpuPercent,
                latencyMs        = estimateQueuedOutputLatencyMs(),
                sampleRate       = audioTrack?.sampleRate ?: AudioPipeline.SAMPLE_RATE,
                yamnetCategory   = yamnetCategory,
                yamnetConfidence = yamnetConfidence,
                dspActive        = dspActive,
                hrtfActive       = hrtfActive,
                spatialWidth     = pipelineState.spatialWidth
            )
        } catch (t: Throwable) {
            android.util.Log.w("IVANNA.BridgePlayer", "pollOmegaMetrics: ${t.message}")
        }
    }
    // ── FIN PARCHE 1 ─────────────────────────────────────────────────────────

    // ── Motor Neuromórfico Kotlin (ESN puro, sin JNI) ────────────────────────
    // Inicializado lazy para no pagar el coste de la matriz de pesos
    // (O(N²) = 64² floats) hasta que se active por primera vez.
    @Volatile var npeKotlinEnabled: Boolean = false
    @Volatile var volterraEnabled: Boolean = false
    private val npeKotlin: NeuromorphicProcessingEngine by lazy {
        NeuromorphicProcessingEngine(
            neuronCount       = 64,
            spectralRadius    = 0.9f,
            inputScaling      = 0.4f,
            leakRate          = 0.08f,
            threshold         = 1.0f,
            outputScaling     = 0.25f,
            plasticityRate    = 0.003f,
            homeostasisRate   = 0.001f,
            resonanceBankSize = 8
        )
    }
    private val volterraProcessor: VolterraH2Processor by lazy { VolterraH2Processor() }

    /** Procesa un bloque PCM estéreo intercalado a través del NPE Kotlin.
     *  Llamado desde PlaybackCaptureService para aplicar el mismo motor
     *  neuromórfico al audio capturado de apps externas. */
    fun processBlockThroughNpeKotlin(buffer: FloatArray): FloatArray =
        npeKotlin.process(buffer)
    fun updateNpeKotlinParams(
        spectralRadius: Float  = 0.9f,
        inputScaling:   Float  = 0.4f,
        outputScaling:  Float  = 0.25f,
        plasticityRate: Float  = 0.003f
    ) {
        if (npeKotlinEnabled) {
            npeKotlin.updateParameters(
                spectralRadius = spectralRadius,
                inputScaling   = inputScaling,
                outputScaling  = outputScaling,
                plasticityRate = plasticityRate
            )
        }
    }

    // FIX (reproducción consecutiva): cola real. play() sigue soportando
    // un solo Uri (comportamiento previo intacto); playQueue() agrega
    // avance automático real cuando el track termina por EOS natural
    // (no cuando el usuario para manualmente con stop()).
    private val queue = mutableListOf<Uri>()
    private var queueIndex = -1
    var onQueueAdvance: ((Uri) -> Unit)? = null

    fun playQueue(uris: List<Uri>, startIndex: Int = 0) {
        queue.clear()
        queue.addAll(uris)
        if (queue.isEmpty()) {
            queueIndex = -1
            stop()
            // Notify the UI that the queue is cleared without inventing a track URI.
            onQueueAdvance?.invoke(Uri.EMPTY)
            return
        }
        queueIndex = startIndex.coerceIn(0, queue.lastIndex)
        play(queue[queueIndex])
    }

    private fun advanceQueueIfAny() {
        if (queueIndex < 0 || queueIndex >= queue.lastIndex) return
        queueIndex++
        val next = queue[queueIndex]
        onQueueAdvance?.invoke(next)
        play(next)
    }

    /**
     * Avance explícito solicitado por controles del sistema (wearables,
     * auriculares, pantalla de bloqueo). Comparte la misma cola y callback
     * que el avance por EOS; no depende de que termine el tema actual.
     */
    fun skipToNext(): Boolean {
        if (queueIndex < 0 || queueIndex >= queue.lastIndex) {
            Log.i(TAG, "skipToNext ignored: no next item in queue")
            return false
        }
        queueIndex++
        val next = queue[queueIndex]
        onQueueAdvance?.invoke(next)
        play(next)
        return true
    }

    /** Previous-track action from the Android media session / compatible wearables. */
    fun skipToPrevious(): Boolean {
        if (queueIndex <= 0 || queueIndex > queue.lastIndex) {
            // At the start of the queue, the conventional action restarts this track.
            if (queueIndex == 0) {
                seekTo(0L)
                if (state == State.PAUSED) return true
                return true
            }
            Log.i(TAG, "skipToPrevious ignored: no active queue item")
            return false
        }
        queueIndex--
        val previous = queue[queueIndex]
        onQueueAdvance?.invoke(previous)
        play(previous)
        return true
    }

    private val voiceProtection: VoiceProtectionController? by lazy {
        try {
            VoiceProtectionController(context).also { it.enabled = voiceProtectionEnabled }
        } catch (e: Exception) {
            Log.w(TAG, "Voice Protection no disponible: ${e.message}")
            null
        }
    }

    fun setVoiceProtectionEnabled(enabled: Boolean) {
        voiceProtectionEnabled = enabled
        voiceProtection?.enabled = enabled
        if (!enabled) DSPBridge.setVoiceProtectScore(0f)
    }

    /** Reproduce el archivo en [uri]. Cancela cualquier reproducción previa. */
    fun play(uri: Uri) {
        activeInstance = this
        stop()
        stopRequested = false
        pauseRequested = false
        focusManager.requestAudioFocus() // FIX: foco cooperativo antes de sonar
        job = scope.launch { runDecodeLoop(uri) }
    }

    fun pause() {
        pauseRequested = true
        runCatching {
            audioTrack?.let {
                Log.v(TAG, "pause() prev codecState=$codecState")
                if (codecState == CodecState.RUNNING) it.pause()
                else Log.w(TAG, "pause() skip: codecState=$codecState")
            }
        }.onFailure { Log.w(TAG, "pause() falló (prev=$codecState): ${it.message}") }
        state = State.PAUSED
    }

    fun resume() {
        pauseRequested = false
        runCatching {
            audioTrack?.let {
                Log.v(TAG, "resume() prev codecState=$codecState")
                if (codecState == CodecState.RUNNING) it.play()
                else Log.w(TAG, "resume() skip: codecState=$codecState")
            }
        }.onFailure { Log.w(TAG, "resume() falló (prev=$codecState): ${it.message}") }
        state = State.PLAYING
    }

    /**
     * Solicita un salto a [positionMs] milisegundos.
     * El decode loop lo consume en la próxima iteración:
     *   - llama extractor.seekTo() en modo SEEK_TO_PREVIOUS_SYNC
     *   - vacía el codec (flush + restart)
     *   - actualiza currentPositionMs
     * Si el reproductor estaba en PAUSED sigue en PAUSED (el caller de UI
     * puede llamar resume() si lo desea tras el seek).
     */
    fun seekTo(positionMs: Long) {
        val clamped = positionMs.coerceIn(0L, _durationMs.value)
        _currentPositionMs.value = clamped
        seekTargetUs = clamped * 1_000L
    }

    /**
     * stop() idempotente: sólo actúa sobre track si el estado previo lo
     * permite. El cierre real se serializa en codecMutex para que nunca
     * se solape con un codec.start() en curso dentro de runDecodeLoop.
     */
    fun stop() {
        stopRequested = true
        pauseRequested = false
        job?.cancel()
        job = null
        scope.launch {
            codecMutex.withLock { releaseTrackLocked() }
        }
        focusManager.abandonAudioFocus() // FIX: libera el foco al parar de verdad
        state = State.STOPPED
    }

    fun release() {
        if (activeInstance === this) activeInstance = null
        stop()
        runCatching { voiceProtection?.release() }
        scope.cancel()
        runCatching { audioDispatcher.close() }
    }

    // ── AUDIT FIX PR 1: Implementación de PerceptualStateListener ──────────────
    /**
     * Recibir cambios de estado perceptual de PerceptualCortex.
     * Se aplican dinámicamente durante la reproducción.
     *
     * @param state Nuevo PerceptualState calculado por la corteza perceptual
     * @param deltaMs Tiempo desde última actualización
     */
    override fun onPerceptualStateChanged(state: PerceptualState, deltaMs: Long) {
        // Aplicar dinámicamente los parámetros DSP calculados
        // El DSPBridge ya recibe estos parámetros via el pipeline normal,
        // pero aquí podemos hacer ajustes específicos al bridge player:
        //
        // 1. Ajustar volumen basado en adaptación perceptual
        // 2. Cambiar buffering si hay fatiga auditiva
        // 3. Modular la ganancia del exciter
        // 4. Aplicar EQ dinámico

        try {
            val dspControl = state.dsp
            Log.d("IvannaBridgePlayer", 
                "Perceptual update: gain=${dspControl.gain}, " +
                "compressor=${dspControl.compressor}, " +
                "exciter=${dspControl.exciter}, " +
                "spatial=${dspControl.spatial}")

            // Enviar parámetros al DSPBridge para que se apliquen al stream en vivo
            if (isPlaying()) {
                DSPBridge.applyAdaptiveState(
                    AdaptiveDSPState(
                        gain = dspControl.gain,
                        compressor = dspControl.compressor,
                        exciter = dspControl.exciter,
                        spatial = dspControl.spatial,
                        lowEqDb = dspControl.lowEqDb,
                        midEqDb = dspControl.midEqDb,
                        highEqDb = dspControl.highEqDb,
                        iso226Compensation = dspControl.iso226Compensation,
                        fatigueProtection = dspControl.fatigueProtection
                    )
                )
            }
        } catch (e: Exception) {
            Log.e("IvannaBridgePlayer", "Error aplicando estado perceptual: ${e.message}")
        }
    }

    private fun isPlaying(): Boolean = state == State.PLAYING

    /**
     * Cierre de AudioTrack con conocimiento de estado. DEBE llamarse con
     * codecMutex ya tomado (sufijo "Locked"). Sólo ejecuta las llamadas
     * legales para el estado previo:
     *   RUNNING            → pause + flush + stop + release
     *   STOPPED/CONFIGURED → sólo release
     *   IDLE               → no-op
     */
    private fun releaseTrackLocked() {
        // FIX: ya no se abre sesión de efectos stock para el propio track
        // (ver comentario en runDecodeLoop) — no hay nada que cerrar aquí.
        val prev = codecState
        when (prev) {
            CodecState.RUNNING -> {
                runCatching { audioTrack?.pause() }.onFailure {
                    Log.w(TAG, "releaseTrack pause (from $prev): ${it.message}")
                }
                runCatching { audioTrack?.flush() }.onFailure {
                    Log.w(TAG, "releaseTrack flush (from $prev): ${it.message}")
                }
                runCatching { audioTrack?.stop() }.onFailure {
                    Log.w(TAG, "releaseTrack stop (from $prev): ${it.message}")
                }
                runCatching { audioTrack?.release() }.onFailure {
                    Log.w(TAG, "releaseTrack release (from $prev): ${it.message}")
                }
            }
            CodecState.STOPPED, CodecState.CONFIGURED -> {
                runCatching { audioTrack?.release() }.onFailure {
                    Log.w(TAG, "releaseTrack release (from $prev): ${it.message}")
                }
            }
            CodecState.IDLE -> { /* nada que cerrar */ }
        }
        audioTrack = null
        hiresDirectActive = false
        codecState = CodecState.IDLE
    }

    private suspend fun runDecodeLoop(uri: Uri) = withContext(audioDispatcher) {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        // ASRC MAGISTRAL Ruta A (v2.5.404): se declara ANTES del try porque
        // el finally lo cierra (routeAAsrc.close()); declarado dentro del
        // try, el finally no lo ve ("Unresolved reference", CI 405).
        val routeAAsrc = RouteAAsrc()
        // Downsampler sinc nativo (Fase 4): se declara ANTES del try por
        // la misma razón que routeAAsrc: el finally lo cierra
        // (nativeDownsampler.close()); declarado dentro del try, el
        // finally no lo ve ("Unresolved reference", CI 463).
        val nativeDownsampler = NativeSincDownsampler()
        // Telemetría en vivo 1 Hz (v2.5.407, IVANNA.Telemetry): el Job
        // TAMBIÉN se declara antes del try, por la misma razón: el
        // finally lo cancela. Si se declarara dentro del try, el
        // finally no lo vería y el poller seguiría vivo tras el stop.
        var telemetryJob: Job? = null
        try {
            extractor.setDataSource(context, uri, null)

            var trackIndex = -1
            var format: MediaFormat? = null
            for (i in 0 until extractor.trackCount) {
                val f = extractor.getTrackFormat(i)
                val mime = f.getString(MediaFormat.KEY_MIME) ?: continue
                if (mime.startsWith("audio/")) { trackIndex = i; format = f; break }
            }
            if (trackIndex < 0 || format == null) {
                Log.e(TAG, "Sin pista de audio en $uri")
                state = State.ERROR
                return@withContext
            }
            extractor.selectTrack(trackIndex)

            // Duración real del archivo (µs → ms). KEY_DURATION puede no existir en
            // streams sin cabecera; si es así se deja en 0 (barra indeterminada en UI).
            val rawDurationUs = if (format.containsKey(MediaFormat.KEY_DURATION))
                format.getLong(MediaFormat.KEY_DURATION) else 0L
            _durationMs.value = rawDurationUs / 1_000L
            _currentPositionMs.value = 0L
            seekTargetUs = -1L

            val inputSampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            val outputRoute = selectOutputRoute()
            val sampleRate = outputRoute.sampleRate
            val channelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            val mime = format.getString(MediaFormat.KEY_MIME)!!

            val resampler = StereoAudioResampler(sampleRate)
            resampler.setInputSampleRate(inputSampleRate)
            // Downsampler sinc nativo (Fase 4): solo conversión de tasa entera
            // descendente (192k→48k, 96k→48k, 88.2k→44.1k) con rechazo de alias
            // >130 dB medido. Si el nativo no está disponible o el ratio no es
            // entero, useNativeDownsampler=false y el resampler Kotlin lleva la
            // conversión (fallback, comportamiento actual). Tasa igual: el
            // passthrough bit-perfect ya vive en StereoAudioResampler
            // (inputRate==targetRate y ppm==0 devuelve la entrada sin tocarla).
            // (La instancia vive antes del try; aquí solo se configura.)
            val useNativeDownsampler = inputSampleRate != sampleRate &&
                nativeDownsampler.isAvailable &&
                nativeDownsampler.configure(inputSampleRate, sampleRate)
            if (useNativeDownsampler) {
                Log.i(TAG, "Downsampler sinc nativo activo: $inputSampleRate→$sampleRate Hz, ${nativeDownsampler.taps} taps, latencia ${nativeDownsampler.latencyFrames} frames")
            }
            playbackSpatialEngine.configureSampleRate(sampleRate)
            playbackSpatialEngine.reset()
            // One SAFMUS servo owns both rolling clock-drift estimation and
            // queue-phase correction. Never run a second controller on this resampler.
            val bufferPll = AudioTrackBufferPll(sampleRate)
            // ASRC MAGISTRAL Ruta A (v2.5.404): FIR 319 taps + Catmull-Rom
            // nativo, alimentado por el mismo PLL. Si el nativo no está
            // disponible (isAvailable=false), el remuestreador Kotlin sigue
            // llevando la corrección (fallback, fail-closed); el ASRC es
            // passthrough y el audio no se rompe. (La instancia se crea
            // antes del try; aquí solo se alimenta la tasa.)
            // Ψ VIVA: el ASRC necesita la tasa del track para diseñar el
            // FIR Ψ (mapeo 0..Nyquist) y medir la envolvente 1–8 Hz (η).
            // Ψ se activa sola en el primer bloque; sin esto usaría 48 kHz.
            routeAAsrc.setSampleRate(sampleRate.toFloat())
            // Telemetría en vivo 1 Hz (v2.5.407): una línea por segundo
            // en logcat (tag IVANNA.Telemetry) con TODOS los efectos.
            // Hilo de fondo (Dispatchers.Default), nunca el hilo de audio.
            // Se cancela en el finally; si el scope muere, muere con él.
            // v2.5.419: el poller también recibe el servo de este
            // reproductor (antes pllProvider era null y la telemetría del
            // reproductor salía con pll_drift/pll_dem/pll_rail en 0, lo que
            // escondía el riel del servo del reproductor en vivo).
            telemetryJob = startTelemetryPoller(
                routeAAsrc,
                appContext = context,
                pllProvider = { bufferPll },
                // Puerta Hi-Fi USB (0030): el provider se evalúa por tick,
                // así que lee el valor ya fijado al construir el track.
                hiresDirectProvider = { hiresDirectActive }
            )
            val outputTimestamp = AudioTimestamp()
            // A one-step latency change from an AudioTrack reroute (Bluetooth
            // connect/disconnect, output-device policy change mid-session) is
            // NOT clock drift: it is a discontinuity in acoustic-path delay.
            // Tracked so the poll below can distinguish the two causes instead
            // of letting the rolling regression straddle the jump.
            var lastRoutedDeviceId: Int? = null
            OmegaMetrics.updateClockServoTelemetry(false, false, 0.0, 0.0, 0.0)
            DSPBridge.init(sampleRate)
            val predictive = PredictiveAudioAnalyzer(sampleRate)
            predictiveAnalyzer = predictive

            val npeSampleRateMismatch = IvannaNpeEngine.isReady && IvannaNpeEngine.sampleRate != sampleRate
            if (npeSampleRateMismatch) {
                Log.w(TAG, "NPE inicializado a ${IvannaNpeEngine.sampleRate}Hz, salida bridge=$sampleRate Hz — NPE desactivado para esta reproducción")
            }

            // Puerta Hi-Fi USB (0030): si la ruta es direct y el track no
            // inicializa, fallback silencioso a la ruta float actual
            // (misma tasa, mismo device). Sin error visible, con log.
            // La tasa no cambia en el fallback, así que el resampler, el
            // PLL y el ASRC (ya configurados arriba a `sampleRate`) no se
            // tocan.
            var effectiveRoute = outputRoute
            var trackOrNull: AudioTrack? = buildAudioTrack(outputRoute, sampleRate)
            if (trackOrNull == null && outputRoute.direct) {
                Log.w(TAG, "ruta direct no inicializó; fallback a float/mezclador")
                effectiveRoute = outputRoute.copy(
                    encoding = AudioFormat.ENCODING_PCM_FLOAT, direct = false
                )
                trackOrNull = buildAudioTrack(effectiveRoute, sampleRate)
            }
            if (trackOrNull == null) {
                Log.e(TAG, "AudioTrack no inicializó")
                state = State.ERROR
                return@withContext
            }
            val track = trackOrNull
            hiresDirectActive = effectiveRoute.direct

            // FIX (unificación Vía A / Vía B, distorsión + Peak>1.33): esta
            // sesión YA pasó por la cadena nativa completa (DSPBridge +
            // NPE + Spatial) en el bucle de abajo. Antes también se le
            // enganchaban los efectos STOCK de Android (Equalizer,
            // BassBoost, Virtualizer, LoudnessEnhancer, DynamicsProcessing)
            // vía globalEffectManager sobre el MISMO audioSessionId —
            // doble procesamiento (software + plataforma) sumando ganancia
            // dos veces. globalEffectManager queda reservado EXCLUSIVAMENTE
            // para sesiones de apps de terceros (Spotify/YouTube, Ruta B),
            // que no pasan por DSPBridge y sí necesitan esos efectos.
            audioTrack = track

            codec = MediaCodec.createDecoderByType(mime)
            // Configuración + arranque bajo codecMutex: ningún stop()/
            // release() concurrente puede cerrar el track mientras
            // configure()/start() están en curso (IllegalStateException).
            codecMutex.withLock {
                if (codecState != CodecState.IDLE) {
                    Log.w(TAG, "runDecodeLoop con codecState=$codecState — cerrando antes de recrear")
                    releaseTrackLocked()
                    // audioTrack quedó en null; volver a enlazar el de esta sesión
                    audioTrack = track
                }
                runCatching { codec.configure(format, null, null, 0) }
                    .onFailure {
                        Log.e(TAG, "codec.configure falló (prev codecState=$codecState): ${it.message}")
                        releaseTrackLocked()
                        state = State.ERROR
                        return@withContext
                    }
                codecState = CodecState.CONFIGURED
                runCatching {
                    codec.start()
                    // PRIMING (0025): track.play() DIFERIDO hasta el primer
                    // write aceptado (ver el bucle, abajo). Antes play()
                    // corría con el buffer VACÍO y el mezclador se quedaba
                    // sin audio hasta que llegara el primer bloque
                    // decodificado + DSP (~10-30 ms, más con los picos de
                    // arranque en frío) = underruns en CADA cambio de pista
                    // (294 acumulados, medido en vivo 2026-11-16 11:16 CST;
                    // los cortes que Esse oye al brincar de canción). Con
                    // play() diferido, el mezclador arranca con audio ya en
                    // cola y no hay underrun de arranque. El route readback
                    // vive en el bucle (tras cada write), no aquí.
                }.onFailure {
                    Log.e(TAG, "codec.start falló (prev codecState=CONFIGURED): ${it.message}")
                    releaseTrackLocked()
                    state = State.ERROR
                    return@withContext
                }
                codecState = CodecState.RUNNING
            }
            state = State.PLAYING

            val bufferInfo = MediaCodec.BufferInfo()
            var sawInputEOS = false
            var sawOutputEOS = false
            // AudioTrack.write(float[]) reports float samples, so divide by
            // two to count accepted interleaved stereo frames cumulatively.
            var submittedFrames = 0L
            submittedFramesForMetrics = 0L
            var lastClockPollNs = 0L
            // PRIMING (0025): el track arranca (play()) solo tras el primer
            // write aceptado; ver el comentario en codec.start() arriba.
            var trackStarted = false
            // Pacing de nivel (v2.5.457): último log del pacing (throttle
            // a 1 línea cada 5 s; el pacing mismo corre en cada vuelta).
            var lastPacingLogNs = 0L

            val spatialInL = FloatArray(MAX_CHUNK_FRAMES)
            val spatialInR = FloatArray(MAX_CHUNK_FRAMES)
            val spatialOutL = FloatArray(MAX_CHUNK_FRAMES)
            val spatialOutR = FloatArray(MAX_CHUNK_FRAMES)
            // Salida del ASRC magistral (reutilizado; el FIFO interno del
            // ASRC absorbe la variación de frames por la corrección ppm).
            // Margen +512 frames para la válvula high-water del FIFO.
            val asrcOut = FloatArray((MAX_CHUNK_FRAMES + 512) * 2)
            // Puerta Hi-Fi USB (0030): buffer reutilizable para la
            // conversión float → PCM_24_BIT_PACKED (solo ruta direct):
            // (MAX+512) frames estéreo × 3 bytes/muestra. Sin alloc por
            // bloque.
            val pcm24Out = ByteArray((MAX_CHUNK_FRAMES + 512) * 2 * 3)

            while (isActive && !sawOutputEOS && !stopRequested) {
                while (pauseRequested && !stopRequested) delay(50)
                if (stopRequested) break

                // --- Seek pendiente ---
                val pendingSeekUs = seekTargetUs
                if (pendingSeekUs >= 0L) {
                    seekTargetUs = -1L
                    sawInputEOS = false
                    predictive.reset()
                    routeAAsrc.reset()
                    extractor.seekTo(pendingSeekUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
                    // codec.flush() sólo es legal en estado Executing (nuestro
                    // RUNNING). Guard + log del estado previo — sin guard,
                    // flush en estado inválido lanza IllegalStateException.
                    runCatching {
                        if (codecState == CodecState.RUNNING) codec.flush()
                        else Log.w(TAG, "flush skip: codecState=$codecState")
                    }.onFailure { Log.w(TAG, "codec.flush (prev=$codecState): ${it.message}") }
                    if (npeKotlinEnabled) npeKotlin.reset()
                }

            // Control de nivel por RITMO DE ESCRITURA (v2.5.457, fix raíz del
            // PLL/ASRC): si la ocupación estimada del track supera el
            // objetivo (capacidad/2), el productor (decodificador) se PAUSA
            // y deja que el track drene — NO se escribe este bloque y NO se
            // decodifica más hasta que el nivel baje. Sin pérdida de audio:
            // el codec retiene sus buffers de salida (backpressure natural)
            // y el ASRC/FIFO no se toca. Esto reemplaza al snap de nivel
            // (455, legacy): el snap soltaba 2048 frames de contenido por
            // sondeo pero el productor rellenaba el hueco con el siguiente
            // bloque, así que el nivel se quedaba en ~98k y el audio perdía
            // 17% de su contenido (metralleta, medido en vivo 2026-11-16).
            // El nivel lo determina el ritmo de escritura, no un salto
            // discreto: escribir solo cuando hay sitio por debajo del
            // objetivo hace que el nivel converja al objetivo y se quede
            // ahí (el ppm queda solo para la deriva real, ~+8.5 ppm).
            // La estima es viva (entre sondeos del PLL, anclada al último
            // timestamp); el retardo por iteración se acota a 50 ms y se
            // re-evalúa en cada vuelta, así el drenado del exceso inicial
            // (~52k frames, ~1 s) es progresivo y el track nunca se queda
            // sin audio (siempre guarda ~el objetivo, ~0.96 s). El
            // integrador de nivel del PI se resetea al pausar: el nivel ya
            // lo maneja el pacing, y el PI no debe acumular windup por un
            // error que él no corrige.
            val pacingNowNs = SystemClock.elapsedRealtimeNanos()
            val pacingEstOcc = bufferPll.estimateOccupancy(submittedFrames, pacingNowNs)
            val pacingTarget = bufferPll.targetOccupancyFrames
                .takeIf { it > 0L } ?: (track.bufferSizeInFrames / 2L).coerceAtLeast(1L)
            // 0039: sin ancla de marca de tiempo (ruta direct_pcm: la
            // lectura de la marca no devuelve nunca), estimateOccupancy es
            // lo enviado (monótono) y el pacing se bloquearía para siempre
            // tras ~512 ms. Sin ancla no se paces: la escritura
            // bloqueante ya frena al productor con el buffer lleno.
            if (shouldPaceProducer(pacingEstOcc, pacingTarget, bufferPll.targetOccupancyFrames > 0L)) {
                val excessFrames = pacingEstOcc - pacingTarget
                bufferPll.resetLevelIntegral()
                val drainMs = (excessFrames * 1000L / sampleRate).coerceIn(5L, 50L)
                if (pacingNowNs - lastPacingLogNs >= 5_000_000_000L) {
                    lastPacingLogNs = pacingNowNs
                    Log.d(TAG, "Pacing de nivel activo: occ est. $pacingEstOcc > objetivo $pacingTarget (exceso $excessFrames frames); pausa ${drainMs}ms")
                }
                delay(drainMs)
                continue
            }

            if (!sawInputEOS) {
                    val inIndex = codec.dequeueInputBuffer(TIMEOUT_US)
                    if (inIndex >= 0) {
                        val inBuf = codec.getInputBuffer(inIndex) ?: continue
                        val sampleSize = extractor.readSampleData(inBuf, 0)
                        if (sampleSize < 0) {
                            codec.queueInputBuffer(inIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            sawInputEOS = true
                        } else {
                            val sampleTimeUs = extractor.sampleTime
                            codec.queueInputBuffer(inIndex, 0, sampleSize, sampleTimeUs, 0)
                            _currentPositionMs.value = sampleTimeUs / 1_000L
                            // Poll de métricas cada ~500ms (sin alloc, sin bloqueo)
                            if (sampleTimeUs % 500_000L < 30_000L) pollOmegaMetrics()
                            extractor.advance()
                        }
                    }
                }

                val outIndex = codec.dequeueOutputBuffer(bufferInfo, TIMEOUT_US)
                if (outIndex >= 0) {
                    val outBuf = codec.getOutputBuffer(outIndex) ?: continue
                    if (bufferInfo.size > 0) {
                        outBuf.position(bufferInfo.offset)
                        outBuf.limit(bufferInfo.offset + bufferInfo.size)
                        val floats = pcmToFloat(
                            outBuf,
                            bufferInfo.size,
                            codec.outputFormat.getIntegerOrDefault(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
                        )
                        val stereo = if (channelCount == 1) monoToStereo(floats) else floats
                        val totalFrames = stereo.size / 2
                        var offset = 0
                        while (offset < totalFrames) {
                            // Convert source PCM to the selected output clock BEFORE DSP/HRTF.
                            // Those kernels are initialized at sampleRate, not inputSampleRate.
                            // Keep headroom so upsampling cannot overflow spatial scratch buffers.
                            val maxInputChunkFrames = (
                                (MAX_CHUNK_FRAMES - 64).toDouble() * inputSampleRate / sampleRate
                            ).toInt().coerceAtLeast(1)
                            val inputChunkFrames = minOf(maxInputChunkFrames, totalFrames - offset)
                            val inputChunk = stereo.copyOfRange(offset * 2, (offset + inputChunkFrames) * 2)
                            // Fase 4: el downsampler sinc nativo lleva la conversión de tasa
                            // entera (192k→48k); el resampler Kotlin es el fallback (y su
                            // passthrough bit-perfect para tasa igual).
                            val chunk = if (useNativeDownsampler) nativeDownsampler.process(inputChunk) else resampler.process(inputChunk)
                            val chunkFrames = chunk.size / 2
                            offset += inputChunkFrames
                            if (chunkFrames <= 0) continue
                            voiceProtection?.feed(chunk, chunkFrames, sampleRate)
                            // Ruta A (reproductor propio) y Ruta B (apps de terceros) son
                            // mutuamente excluyentes: AudioSessionReceiver filtra el paquete
                            // de IVANNA, por lo que esta sesión NO tiene omega_effect global.
                            // La conexión del daemon solo acredita el plano de control/socket;
                            // no prueba que AudioFlinger procese este AudioTrack. Mantener aquí
                            // el DSP local evita audio sin procesar cuando el daemon está vivo.
                            val nativeDspHandled = DSPBridge.isLoaded
                            if (DSPBridge.isLoaded) {
                                DSPBridge.process(chunk, chunkFrames)
                            }
                            if (com.ivanna.omega.core.IvannaNativeLib.isLoaded &&
                                analyzeTickCounter.incrementAndGet() % 50 == 0) {
                                runCatching { com.ivanna.omega.core.IvannaNativeLib.nativeAnalyzeAudio(chunk) }
                            }
                            // Árbitro de Ruta Única: si el daemon system-wide está activo o DSPBridge
                            // nativo ya ejecutó la cadena maestra (Volterra C++, 5 Ejes, Spatial C++ y
                            // SafetyLimiter), no apilar procesadores duplicados sobre el mismo bloque.
                            if (!nativeDspHandled) {
                                if (IvannaNpeEngine.isReady && !npeSampleRateMismatch) {
                                    try { IvannaNpeEngine.processInterleavedStereo(chunk, chunkFrames) } catch (e: Exception) { android.util.Log.e("IVANNA_DSP", "Crash NPE evitado: ${e.message}") }
                                }
                                if (npeKotlinEnabled) {
                                    try {
                                        val npeIn = chunk.copyOf()
                                        val npeOut = npeKotlin.process(npeIn)
                                        npeOut.copyInto(chunk)
                                    } catch (e: Exception) {
                                        android.util.Log.e("IVANNA_DSP", "Crash NPE-Kotlin evitado: ${e.message}")
                                    }
                                }
                                if (VolterraSwitch.enabled) {
                                    try {
                                        val volterraOut = volterraProcessor.process(chunk)
                                        volterraOut.copyInto(chunk)
                                    } catch (e: Exception) {
                                        android.util.Log.e("IVANNA_DSP", "Crash Volterra evitado: ${e.message}")
                                    }
                                }
                                if (com.ivanna.omega.dsp.ConcertMode.enabled) {
                                    com.ivanna.omega.dsp.ConcertMode.shared.process(chunk)
                                }
                                if (com.ivanna.omega.spatial.IvannaSpatialEngine.enabled) {
                                    for (i in 0 until chunkFrames) {
                                        spatialInL[i] = chunk[i * 2]
                                        spatialInR[i] = chunk[i * 2 + 1]
                                    }
                                    playbackSpatialEngine.processStereoInput(
                                        spatialInL, spatialInR, spatialOutL, spatialOutR, chunkFrames
                                    )
                                    for (i in 0 until chunkFrames) {
                                        chunk[i * 2] = spatialOutL[i]
                                        chunk[i * 2 + 1] = spatialOutR[i]
                                    }
                                }
                            }
                            predictive.feed(chunk, chunkFrames)   // observador: no modifica `chunk`
                            // ASRC MAGISTRAL Ruta A: DESPUÉS de la cadena DSP,
                            // antes de escribir a AudioTrack. El nativo
                            // remuestrea con FIR 319 taps + Catmull-Rom y
                            // aplica la corrección ppm del PLL; su FIFO
                            // interno absorbe la variación de frames por
                            // bloque (el contrato de bloque del DSP no se
                            // rompe). Sin nativo, passthrough: el
                            // remuestreador Kotlin lleva la corrección.
                            // Si asrcFrames==0 (FIFO vacío: solo al arrancar
                            // o si el nativo falló), NO se escribe nada este
                            // bloque: el audio ya está dentro del ASRC/FIFO
                            // y escribir `chunk` directo lo duplicaría.
                            val asrcFrames = routeAAsrc.processInto(chunk, chunkFrames, asrcOut)
                            val writtenSamples = if (asrcFrames > 0) {
                                if (effectiveRoute.direct) {
                                    // Puerta Hi-Fi USB (0030): el ASRC sigue
                                    // produciendo FloatArray; la conversión
                                    // a PCM_24_BIT_PACKED vive aquí, justo
                                    // antes del write, SOLO en ruta direct.
                                    // track.write(ByteArray) devuelve BYTES:
                                    // 3 bytes por muestra → muestras =
                                    // bytes/3. El PLL y la telemetría no
                                    // cambian (miden frames, no bytes).
                                    val bytes = floatToPcm24Packed(asrcOut, asrcFrames, pcm24Out)
                                    val writtenBytes = track.write(
                                        pcm24Out, 0, bytes, AudioTrack.WRITE_BLOCKING
                                    )
                                    if (writtenBytes > 0) writtenBytes / 3 else writtenBytes
                                } else {
                                    track.write(
                                        asrcOut, 0, asrcFrames * 2, AudioTrack.WRITE_BLOCKING
                                    )
                                }
                            } else {
                                0
                            }
                            // Closed-loop software PLL: estimate queue phase from frames
                            // accepted by this AudioTrack minus its timestamped consumed frames.
                            // No Bluetooth/DAC/vendor-HAL clock writes are attempted.
                            if (writtenSamples > 0) {
                                // PRIMING (0025): primer write aceptado = hay
                                // audio en cola; AHORA se arranca el track.
                                // Antes play() corría en setup con el buffer
                                // vacío (underruns en cada cambio de pista).
                                if (!trackStarted) {
                                    trackStarted = true
                                    runCatching { track.play() }.onFailure {
                                        Log.e(TAG, "track.play diferido falló: ${it.message}")
                                    }
                                }
                                AudioTrackRouteReadback.update(track, outputRoute.preferredDevice, sampleRate)
                                submittedFrames += (writtenSamples / 2).toLong()
                                submittedFramesForMetrics = submittedFrames
                                // Timestamp polling is throttled off the per-chunk hot path.
                                val nowNs = SystemClock.elapsedRealtimeNanos()
                                if (lastClockPollNs == 0L || nowNs - lastClockPollNs >= 250_000_000L) {
                                    lastClockPollNs = nowNs
                                    val routedId = runCatching { track.routedDevice?.id }.getOrNull()
                                    val priorRoutedId = lastRoutedDeviceId
                                    val rerouted = AudioTrackRerouteGuard.isReroute(priorRoutedId, routedId)
                                    if (routedId != null) lastRoutedDeviceId = routedId
                                    if (rerouted) {
                                        // Android moved this SAME AudioTrack to a different output
                                        // (e.g. a Bluetooth A2DP sink connecting/disconnecting) without
                                        // the app recreating the track. The acoustic-path latency just
                                        // took a one-time step, not a sustained clock-rate error: feeding
                                        // that frame/timestamp pair into the rolling regression would be
                                        // misread as drift. Fail closed to unity and let the servo relock
                                        // from a clean window on the new route instead.
                                        Log.i(TAG, "AudioTrack reroute detectado (routedDevice id $priorRoutedId -> $routedId); bufferPll reiniciado, no se interpreta como deriva de reloj")
                                        bufferPll.reset()
                                        resampler.setClockCorrectionPpm(0.0)
                                        routeAAsrc.reset()
                                        routeAAsrc.setClockCorrectionPpm(0.0)
                                        OmegaMetrics.updateClockServoTelemetry(false, false, 0.0, 0.0, 0.0)
                                    } else if (track.getTimestamp(outputTimestamp)) {
                                        // One controller estimates clock slope by centered rolling OLS
                                        // and combines feed-forward drift with bounded queue PI.
                                        val correctionPpm = bufferPll.observe(
                                            submittedFrames = submittedFrames,
                                            consumedFrames = outputTimestamp.framePosition,
                                            bufferCapacityFrames = track.bufferSizeInFrames,
                                            timestampNs = outputTimestamp.nanoTime,
                                            // v2.5.419: los underruns invalidan
                                            // la estima de reloj de ese
                                            // intervalo (inanición, no deriva).
                                            underruns = track.underrunCount.toLong()
                                        )
                                        // ASRC MAGISTRAL: el ppm va al nativo
                                        // (FIR 319 taps) si está disponible; el
                                        // remuestreador Kotlin queda en 0.0
                                        // (solo conversión de tasa). Sin nativo,
                                        // el Kotlin lleva la corrección
                                        // (fallback, comportamiento actual).
                                        // NUNCA los dos a la vez: sería doble
                                        // corrección (2x ppm).
                                        // LEGACY v2.5.457 — el snap de nivel ya
                                        // no se aplica: el nivel lo controla el
                                        // ritmo de escritura (pacing, arriba).
                                        // RouteAAsrc.setLevelSnapFrames queda
                                        // como no-op legacy; no se llama.
                                        // 0033 (bypass nativo, decisión de Esse):
                                        // con el ASRC bypassado (asrcEnabled=
                                        // false, default), el servo PLL TAMBIÉN
                                        // queda en bypass: NINGUNA corrección
                                        // de deriva se aplica (ni al ASRC ni
                                        // al resampler Kotlin); el reloj lo
                                        // maneja el AudioTrack nativo del
                                        // Snapdragon. El PLL sigue observando
                                        // (telemetría) pero su corrección no
                                        // se aplica. Solo si asrcEnabled=true
                                        // y el nativo falta, el Kotlin lleva
                                        // la corrección (fallback histórico).
                                        if (routeAAsrc.isAvailable) {
                                            routeAAsrc.setClockCorrectionPpm(correctionPpm)
                                            resampler.setClockCorrectionPpm(0.0)
                                        } else if (RouteAAsrc.asrcEnabled) {
                                            resampler.setClockCorrectionPpm(correctionPpm)
                                        } else {
                                            resampler.setClockCorrectionPpm(0.0)
                                        }
                                        OmegaMetrics.updateClockServoTelemetry(
                                            timestampAvailable = true,
                                            locked = bufferPll.isLocked,
                                            measuredRateHz = bufferPll.measuredRateHz,
                                            measuredDriftPpm = bufferPll.measuredDriftPpm,
                                            correctionPpm = correctionPpm
                                        )
                                    } else {
                                        bufferPll.reset()
                                        resampler.setClockCorrectionPpm(0.0)
                                        routeAAsrc.reset()
                                        routeAAsrc.setClockCorrectionPpm(0.0)
                                        OmegaMetrics.updateClockServoTelemetry(false, false, 0.0, 0.0, 0.0)
                                    }
                                }
                            }
                        }
                    }
                    codec.releaseOutputBuffer(outIndex, false)
                    if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        sawOutputEOS = true
                    }
                }
            }
            state = State.STOPPED
            // FIX: solo avanzar la cola si terminó por EOS natural, no si
            // el usuario llamó stop() manualmente (stopRequested=true).
            if (!stopRequested) advanceQueueIfAny()
        } catch (t: Throwable) {
            Log.e(TAG, "Error en decode loop", t)
            state = State.ERROR
        } finally {
            // Telemetría en vivo (v2.5.407): parar el poller ANTES de
            // cerrar el ASRC, para que no lea un handle ya destruido.
            telemetryJob?.cancel()
            telemetryJob = null
            routeAAsrc.close()
            nativeDownsampler.close()
            predictiveAnalyzer?.close()
            predictiveAnalyzer = null
            OmegaMetrics.updateSharedPredictive(false, 0f, 0f, false)
            // Cierre SIEMPRE bajo mutex y sólo si el estado lo permite:
            // codec.stop() es ilegal fuera de Executing → nuestro RUNNING.
            codecMutex.withLock {
                val prev = codecState
                codec?.let { c ->
                    when (prev) {
                        CodecState.RUNNING -> {
                            runCatching { c.stop() }.onFailure {
                                Log.w(TAG, "codec.stop (from $prev): ${it.message}")
                            }
                            codecState = CodecState.STOPPED
                        }
                        else -> Log.v(TAG, "codec.stop skip: prev=$prev")
                    }
                    runCatching { c.release() }.onFailure {
                        Log.w(TAG, "codec.release (from $codecState): ${it.message}")
                    }
                }
                releaseTrackLocked() // deja codecState en IDLE
            }
            extractor.release()
        }
    }

    private fun monoToStereo(mono: FloatArray): FloatArray {
        val out = FloatArray(mono.size * 2)
        for (i in mono.indices) {
            out[2 * i] = mono[i]
            out[2 * i + 1] = mono[i]
        }
        return out
    }

    private fun pcmToFloat(buf: ByteBuffer, byteSize: Int, encoding: Int): FloatArray {
        return when (encoding) {
            AudioFormat.ENCODING_PCM_FLOAT -> pcmFloatToFloat(buf, byteSize)
            else -> pcm16ToFloat(buf, byteSize)
        }
    }

    private fun pcm16ToFloat(buf: ByteBuffer, byteSize: Int): FloatArray {
        val bb = buf.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        val sampleCount = byteSize / 2
        val out = FloatArray(sampleCount)
        for (i in 0 until sampleCount) {
            out[i] = bb.short.toFloat() / 32768f
        }
        return out
    }

    private fun pcmFloatToFloat(buf: ByteBuffer, byteSize: Int): FloatArray {
        val bb = buf.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        val sampleCount = byteSize / 4
        val out = FloatArray(sampleCount)
        for (i in 0 until sampleCount) {
            out[i] = bb.float.coerceIn(-1f, 1f)
        }
        return out
    }

    private fun MediaFormat.getIntegerOrDefault(key: String, defaultValue: Int): Int {
        return if (containsKey(key)) getInteger(key) else defaultValue
    }
}

/**
 * Conversión float → PCM_24_BIT_PACKED (0030, puerta Hi-Fi USB).
 * Pura y top-level: sin Android, testeable en host.
 *
 * Por muestra (estéreo entrelazado): v = (f * 8388607f).toInt()
 * .coerceIn(-8388608, 8388607), empaquetado little-endian 3 bytes.
 * Casi sin pérdida (mantisa float 24 bits, cuantización a −144 dB).
 * NaN → 0 (toInt() de NaN es 0 en JVM); ±Inf → ±8388607 (el coerceIn
 * lo acota). Devuelve los bytes escritos en `out` (frames*2*3).
 * Los frames se acotan a lo que caben en `input` y en `out`: nunca
 * desborda, nunca lanza.
 */
internal fun floatToPcm24Packed(input: FloatArray, frames: Int, out: ByteArray): Int {
    val n = minOf(frames, input.size / 2, out.size / 6).coerceAtLeast(0)
    var o = 0
    for (i in 0 until n * 2) {
        val v = (input[i] * 8388607f).toInt().coerceIn(-8388608, 8388607)
        out[o++] = (v and 0xFF).toByte()
        out[o++] = ((v ushr 8) and 0xFF).toByte()
        out[o++] = ((v ushr 16) and 0xFF).toByte()
    }
    return o
}

/**
 * Decisión pura de pacing por ritmo de escritura (0039).
 * Pura y top-level: sin Android, testeable en host.
 *
 * Sin ancla de marca de tiempo no se paces: la estima es lo enviado
 * (monótona, nunca baja) y el gate se bloquearía para siempre en la
 * ruta directa (direct_pcm: la lectura de la marca no devuelve nunca).
 * Con ancla, el pacing normal (estima por encima del objetivo).
 * `hasTimestampAnchor` es `targetOccupancyFrames > 0L` del servo: cero
 * = sin ancla; solo una observación válida lo fija.
 */
internal fun shouldPaceProducer(
    estimatedOccupancy: Long,
    targetOccupancy: Long,
    hasTimestampAnchor: Boolean
): Boolean = hasTimestampAnchor && estimatedOccupancy > targetOccupancy

/**
 * Tasa DSP del reproductor propio (FIX 5, v2.5.465): 48 kHz, la tasa a
 * la que está medida y presupuestada TODA la cadena de Ruta A (HOLOS,
 * Φ, Volterra, RIR, EvoEQ, NPE) y la tasa máxima que anuncia el DAC
 * USB de Esse (24-bit @ 44100/48000, verificado en vivo 2026-11-16).
 * A 96 kHz el coste por segundo se duplica y el gobernador de shed
 * suelta bloques a seco de forma crónica (tronido + saltos de nivel).
 */

/** Tasa máxima a la que corre el DSP del reproductor (48 kHz). */
internal const val BRIDGE_PLAYER_DSP_SAMPLE_RATE_HZ = 48_000

/**
 * Elige la tasa de salida del reproductor entre las anunciadas por el
 * dispositivo, sin superar la tasa DSP (48 kHz). Pura y top-level: sin
 * Android, testeable en host.
 *
 * Sin tasas anunciadas (o ninguna ≤48 kHz): 48 kHz (el mezclador
 * remuestrea; el DSP sigue a su tasa presupuestada). Con 44.1 kHz se
 * usa 44.1 kHz (nativo, sin remuestreo en el DSP).
 */
internal fun selectBridgePlayerRate(advertisedRates: IntArray): Int {
    val capped = advertisedRates.filter { it in 44_100..BRIDGE_PLAYER_DSP_SAMPLE_RATE_HZ }
    return capped.maxOrNull() ?: BRIDGE_PLAYER_DSP_SAMPLE_RATE_HZ
}

/**
 * Acepta una ruta direct USB solo si su tasa no supera la tasa DSP
 * (48 kHz); null en caso contrario (el llamador cae a float 48 kHz).
 * Pura y top-level: sin Android, testeable en host.
 */
internal fun clampDirectRouteForDsp(route: DirectRoute?): DirectRoute? =
    route?.takeIf { it.sampleRate <= BRIDGE_PLAYER_DSP_SAMPLE_RATE_HZ }
