/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

package com.qualcomm.fmradio

import android.util.Log

/**
 * Native JNI bridge for accessing the Qualcomm FM Radio Platform Abstraction Layer (PAL).
 * Every function returns diagnostic execution logs directly from C++ userspace.
 */
object FMBridge {
    private const val TAG = "QualcommFM_Bridge"

    init {
        try {
            System.loadLibrary("fm_jni")
            Log.i(TAG, "Successfully loaded native 'fm_jni' JNI library.")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "UnsatisfiedLinkError: Failed to load native 'fm_jni' library: ${e.message}")
        } catch (e: Exception) {
            Log.e(TAG, "Unexpected error loading native library: ${e.message}")
        }
    }

    /**
     * Attempts dlopen() on libfmpal.so and maps required function pointers.
     * @return Detailed log output of candidate checks, dlopen results, and symbol resolution status.
     */
    @JvmStatic
    external fun loadNativeLibrary(): String

    /**
     * Calls fmpal_init() via resolved symbol.
     * @return Execution status log.
     */
    @JvmStatic
    external fun initFm(): String

    /**
     * Calls fmpal_power_up(1) (true) or fmpal_power_up(0) (false).
     * @return Execution status log.
     */
    @JvmStatic
    external fun setPower(power: Boolean): String

    /**
     * Calls fmpal_set_freq(freqKHz).
     * @return Execution status log.
     */
    @JvmStatic
    external fun setFrequency(frequencyMHz: Float): String

    /**
     * Calls fmpal_get_freq(&freqKHz).
     * @return Execution status log.
     */
    @JvmStatic
    external fun getCurrentFrequency(): String

    /**
     * Runs full diagnostics on candidate library paths, dlsym, permissions,
     * SELinux, and binder/HIDL status.
     * @return Comprehensive formatted diagnostic report text.
     */
    @JvmStatic
    external fun getDiagnosticReport(): String
}
