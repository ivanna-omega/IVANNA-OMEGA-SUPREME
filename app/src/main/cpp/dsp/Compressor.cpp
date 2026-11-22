#if defined(__clang__)
#pragma clang optimize on
#else
#pragma GCC optimize("O3", "unroll-loops")
#endif

#include "../include/Compressor.h"
#include <cmath>
#include <algorithm>

namespace ivanna {

Compressor::Compressor() {
    reset();
}

void Compressor::recomputeMakeup() noexcept {
    // FIX (ruido digital): makeupGain_ se aplicaba como ganancia estatica
    // (hasta ~+11 dB) incluso con la senal por debajo del threshold, es decir
    // sin ninguna reduccion que compensar. Eso empujaba material ya limpio
    // contra el SafetyLimiter, que entraba a trabajar sin razon.
    // Ahora se guarda en dB y en process() se limita a la reduccion REAL
    // instantanea: nunca se devuelve mas de lo que se quito.
    const double reduction =
        threshold_ * (1.0 - 1.0 / ratio_);

    makeupDb_ = -reduction * 0.5;      // objetivo maximo de compensacion
    if (makeupDb_ < 0.0) makeupDb_ = 0.0;

    makeupGain_ = std::pow(10.0, makeupDb_ / 20.0);

    // slope_ eliminado del header (era dead state — nunca leído en process())
}

void Compressor::setParams(const DSPParams& p) {
    sr_ = static_cast<double>(p.sampleRate);

    threshold_ = -24.0 + static_cast<double>(p.alpha) * 24.0;
    ratio_ = 1.0 + static_cast<double>(p.beta) * 19.0;

    double atMs = 5.0 + (1.0 - static_cast<double>(p.gamma)) * 95.0;
    double relMs = 50.0 + (1.0 - static_cast<double>(p.gamma)) * 450.0;

    attackCoef_ =
        std::exp(-1.0 / (sr_ * atMs * 0.001));

    releaseCoef_ =
        std::exp(-1.0 / (sr_ * relMs * 0.001));

    recomputeMakeup();

    // inv_atk_/inv_rel_ eliminados del header (dead state)

    runtimeCoef_ = 0.0;  // forzar recálculo si cambió sr_ (ver process())

    // FIX (chasquidos al mover sliders): setParams() recalculaba el sidechain
    // HPF y ademas reseteaba su estado en CADA llamada. Arrastrar cualquier
    // fader (alpha/beta/gamma) vaciaba el historial del filtro -> el detector
    // veia un transitorio artificial -> salto de ganancia audible por cada
    // evento de UI. Los coeficientes solo dependen del sample rate, asi que se
    // recalculan (y el estado se limpia) unicamente cuando el sr cambia.
    if (p.sampleRate != lastSr_) {
        lastSr_ = p.sampleRate;

        scHpfL_.setHighpass(
            120.0,
            0.70710678,
            static_cast<double>(p.sampleRate));

        scHpfR_.setHighpass(
            120.0,
            0.70710678,
            static_cast<double>(p.sampleRate));

        scHpfL_.reset();
        scHpfR_.reset();
    }
}

void Compressor::setThreshold(float db) {
    threshold_ = static_cast<double>(db);
    recomputeMakeup();
}

void Compressor::setRatio(float ratio) {
    ratio_ = ratio < 1.0f ? 1.0 : static_cast<double>(ratio);
    recomputeMakeup();
}

void Compressor::setAttack(float ms) {
    attackCoef_ =
        std::exp(-1.0 / (sr_ * static_cast<double>(ms) * 0.001));
    // inv_atk_ eliminado del header
}

void Compressor::setRelease(float ms) {
    releaseCoef_ =
        std::exp(-1.0 / (sr_ * static_cast<double>(ms) * 0.001));
    // inv_rel_ eliminado del header
}


void Compressor::process(double* __restrict__ left,
                         double* __restrict__ right,
                         int frames) {

    if (frames <= 0) return;

    constexpr double k20DivLn10 = 8.6858896381;
    constexpr double kLn10Div20 = 0.11512925465;

    const double attackCoef = attackCoef_;
    const double releaseCoef = releaseCoef_;

    // Anti-zipper del runtimeAmount: el motor adaptativo fija el objetivo
    // por bloque (setRuntimeAmount); aquí se converge con un one-pole de
    // ~20 ms para que threshold/ratio no salten de golpe en sostenidos.
    //
    // FIX (medido con el arnés de Fase 1, MeasureChain.SettlingVersusDistortion): este one-pole
    // se actualiza UNA vez por bloque, pero su coeficiente estaba calculado para actualizarse
    // POR MUESTRA (exp(-1/(sr*0.020))). La constante de tiempo real era 20 ms x frames
    // (~9.6 s con bloques de 480; ~10 s con 512) y dependía del tamaño de bloque del
    // dispositivo: la reducción derivaba de -0.79 a -1.61 dB durante 12 s tras empezar a sonar
    // (threshold/ratio arrancan en -12 dB/4:1 y tardaban ~10 s en llegar a sus objetivos).
    // El coeficiente ahora es por bloque: exp(-frames/(sr*0.020)) = 20 ms reales siempre.
    // runtimeCoef_ queda como estado heredado (ya no se lee).
    {
        const double rc = std::exp(-static_cast<double>(frames) / (sr_ * 0.020));
        runtimeAmount_ = rc * runtimeAmount_ + (1.0 - rc) * runtimeTarget_;
        // FIX (discontinuidad audible real): threshold_/ratio_ son ahora
        // el objetivo fijado por setThreshold()/setRatio() — converge
        // hacia ellos con el mismo one-pole de 20ms que runtimeAmount_ ya
        // usa, en vez de que process() los lea directos y salte de golpe
        // en cada actualización térmica o cambio de preset.
        thresholdSmoothed_ = rc * thresholdSmoothed_ + (1.0 - rc) * threshold_;
        ratioSmoothed_     = rc * ratioSmoothed_     + (1.0 - rc) * ratio_;
    }

    const double threshold =
        thresholdSmoothed_ - runtimeAmount_ * 12.0;

    const double effRatio =
        ratioSmoothed_ + runtimeAmount_ * 8.0;

    const double ratioInv =
        1.0 - 1.0 / effRatio;

    const double grAdditional =
        runtimeAmount_ * 12.0 *
        (1.0 - 1.0 / effRatio);

    // Techo de compensacion en dB (estatico + aporte del runtime).
    const double makeupCeilDb =
        makeupDb_ + grAdditional * 0.10;

    double env = env_;
    double envS = envSmooth_;
    // Detector de dos etapas. La envolvente de |x| (pico rectificado) tiene rizado a 2f; con un ataque
    // de 5 ms ese rizado modulaba la ganancia dentro del propio ciclo y generaba armonicos
    // (medido: THD+N -61 dB a 150 Hz con 1-3 dB de reduccion, mejorando ~6 dB/octava). Un segundo
    // one-pole de 3 ms (corte ~53 Hz) atenua el rizado a 300 Hz unos 15 dB sin mover el valor medio
    // (la reduccion estatica es la misma) y solo suma 3 ms al ataque efectivo.
    const double smoothCoef = std::exp(-1.0 / (sr_ * 0.003));

    for (int i = 0; i < frames; ++i) {

        const double dL =
            scHpfL_.process(left[i]);

        const double dR =
            scHpfR_.process(right[i]);

        double peak =
            std::fmax(std::fabs(dL),
                      std::fabs(dR));

        if (peak < 1e-6)
            peak = 1e-6;

        double coef =
            (peak > env) ?
            attackCoef :
            releaseCoef;

        env =
            coef * env +
            (1.0 - coef) * peak;

        envS = smoothCoef * envS + (1.0 - smoothCoef) * env;

        double envDb =
            k20DivLn10 * std::log(envS);

        double gainDb = 0.0;

        constexpr double kKneeHalf = 3.0;

        double overDb =
            envDb - threshold;


        if (overDb <= -kKneeHalf) {

            gainDb = 0.0;

        } else if (overDb < kKneeHalf) {

            double t =
                (overDb + kKneeHalf) /
                (2.0 * kKneeHalf);

            gainDb =
                -(ratioInv * kKneeHalf)
                * t * t;

        } else {

            gainDb =
                overDb * (-ratioInv);

        }


        // Makeup acoplado a la reduccion real: gainDb es <= 0 (o 0 si no hay
        // compresion), asi que -gainDb es la reduccion aplicada en dB. Con la
        // senal bajo threshold no hay reduccion -> makeup 0 dB -> unidad
        // exacta, sin ganancia gratuita hacia el limiter.
        const double grDb = -gainDb;
        const double makeupDb =
            grDb < makeupCeilDb ? grDb : makeupCeilDb;

        double lin =
            std::exp((gainDb + makeupDb) * kLn10Div20);

        left[i] *= lin;
        right[i] *= lin;
    }

    env_ = env;
    envSmooth_ = envS;
}

void Compressor::process(float* left, float* right, int frames) {
    if (frames <= 0) return;
    if (static_cast<int>(wrapL_.size()) < frames) {
        wrapL_.resize(static_cast<size_t>(frames));
        wrapR_.resize(static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
        wrapL_[static_cast<size_t>(i)] = static_cast<double>(left[i]);
        wrapR_[static_cast<size_t>(i)] = static_cast<double>(right[i]);
    }
    process(wrapL_.data(), wrapR_.data(), frames);
    for (int i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(wrapL_[static_cast<size_t>(i)]);
        right[i] = static_cast<float>(wrapR_[static_cast<size_t>(i)]);
    }
}


void Compressor::reset() {

    env_ = 0.0;
    envSmooth_ = 0.0;

    scHpfL_.reset();
    scHpfR_.reset();
}

}
