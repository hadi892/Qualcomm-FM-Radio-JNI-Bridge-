/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

package com.qualcomm.fmradio

import android.util.Log

/**
 * Low-level Native JNI Bridge for Qualcomm Snapdragon FM Radio HAL.
 * 
 * Maps directly to C++ JNI implementation in `fm_jni.cpp` registered via manual dynamic
 * binding (RegisterNatives) in JNI_OnLoad.
 */
object FMBridge {
    private const val TAG = "QualcommFM_Bridge"

    init {
        try {
            System.loadLibrary("fm_jni")
            Log.i(TAG, "Successfully loaded native 'fm_jni' library.")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "UnsatisfiedLinkError: Critical failure loading 'fm_jni' JNI bridge: ${e.message}")
        } catch (e: Exception) {
            Log.e(TAG, "Unexpected error loading native library: ${e.message}")
        }
    }

    /**
     * Enables or disables power to the Qualcomm FM receiver band.
     * Maps to native power_up(1) and power_down() PAL configurations.
     * 
     * @param power True to power up the tuner, false to sleep/power down.
     * @return 0 on SUCCESS, negative error code on HAL failure.
     */
    @JvmStatic
    external fun setPower(power: Boolean): Int

    /**
     * Tunes the receiver frequency to the specified MHz.
     * Maps to fmpal_set_freq(freqKHz) in JNI layer.
     * 
     * @param frequencyMHz The target frequency in MHz (e.g. 98.5f).
     * @return 0 on SUCCESS, negative error code on HAL failure.
     */
    @JvmStatic
    external fun setFrequency(frequencyMHz: Float): Int

    /**
     * Reads the current active tuned frequency from baseband registers.
     * Maps to fmpal_get_freq() in JNI layer.
     * 
     * @return Current frequency in MHz, or negative error code on HAL failure.
     */
    @JvmStatic
    external fun getCurrentFrequency(): Float

    /**
     * Triggers the baseband auto-seek engine.
     * Maps to fmpal_seek_station() in JNI layer.
     * 
     * @param direction 1 to seek UPwards, 0 to seek DOWNwards.
     * @return 0 on SUCCESS, negative error code on HAL failure.
     */
    @JvmStatic
    external fun seekStation(direction: Int): Int

    /**
     * Controls audio path muting without sleeping tuner power.
     * Maps to fmpal_set_mute() in JNI layer.
     * 
     * @param mute True to mute audio output, false to unmute.
     * @return 0 on SUCCESS, negative error code on HAL failure.
     */
    @JvmStatic
    external fun setMute(mute: Boolean): Int

    /**
     * Sets hardware-level analog/digital audio gain levels.
     * Maps to fmpal_set_volume() in JNI layer.
     * 
     * @param volume Volume gain level in range [0 - 15].
     * @return 0 on SUCCESS, negative error code on HAL failure.
     */
    @JvmStatic
    external fun setVolume(volume: Int): Int
}
