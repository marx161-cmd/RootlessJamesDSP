package james.dsp

import android.util.Log

/**
 * JNI bridge to the LSP (Linux Studio Plugins) dynamics units — compressor,
 * limiter, gate — cross-compiled from lsp-plugins/lsp-dsp-units 1.2.26.
 *
 * Package intentionally does not match the app's own package: the native
 * symbols in liblsp-dynamics-bridge.so are fixed as Java_james_dsp_LSPDynamics_*,
 * so this class must live at exactly this package/name to bind via JNI.
 */
object LSPDynamics {
    init {
        try {
            System.loadLibrary("lsp-combined")
            System.loadLibrary("lsp-dynamics-bridge")
        } catch (e: UnsatisfiedLinkError) {
            Log.e("LSPDynamics", "Load failed: ${e.message}")
        }
    }

    // Lifecycle
    external fun nativeCreateCompressor(): Long
    external fun nativeCreateLimiter(sampleRate: Int): Long
    external fun nativeCreateGate(): Long
    external fun nativeDestroy(ptr: Long)

    // Parameter setters
    external fun nativeSetCompressorParams(
        ptr: Long, thresholdOpen: Float, thresholdClose: Float,
        ratio: Float, attack: Float, release: Float
    )

    external fun nativeSetLimiterParams(
        ptr: Long, threshold: Float, attack: Float, release: Float
    )

    external fun nativeSetGateParams(
        ptr: Long, thresholdOpen: Float, thresholdClose: Float,
        attack: Float, release: Float
    )

    // Per-sample processing
    external fun nativeProcessCompressorSample(ptr: Long, env: Float, `in`: Float): Float
    external fun nativeProcessLimiterSample(ptr: Long, gain: Float, `in`: Float): Float
    external fun nativeProcessGateSample(ptr: Long, env: Float, `in`: Float): Float

    // Buffer processing
    external fun nativeProcessCompressorMono(ptr: Long, out: FloatArray, env: FloatArray, `in`: FloatArray)
    external fun nativeProcessLimiterMono(ptr: Long, gain: FloatArray, sc: FloatArray)
    external fun nativeProcessGateMono(ptr: Long, out: FloatArray, env: FloatArray, `in`: FloatArray)
}
