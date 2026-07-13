/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

package com.qualcomm.fmradio

import android.content.Context
import android.util.Log

/**
 * Production-grade API Manager for Qualcomm Snapdragon FM Radio receiver hardware.
 * 
 * Provides safe, thread-safe access, robust validations, and maps lower-level native JNI
 * errors into clean Kotlin runtime exceptions.
 */
class FMManager(private val context: Context) {

    companion object {
        private const val TAG = "QualcommFM_Manager"

        // Native bridge error codes
        const val FM_SUCCESS = 0
        const val FM_ERROR_UNSUPPORTED = -1
        const val FM_ERROR_HAL_FAILED = -2
        const val FM_ERROR_NOT_INITIALIZED = -3
        const val FM_ERROR_INVALID_PARAM = -4
        const val FM_ERROR_PERMISSION_DENIED = -5
    }

    private val stateLock = Any()
    private var isPoweredOn = false
    private var currentFrequency = 98.5f
    private var isMuted = false
    private var currentVolume = 10

    /**
     * Powers UP the Qualcomm Snapdragon FM Radio module.
     * Initializes the underlying vendor.qti.hardware.fm HAL service.
     * 
     * @return True if power-up succeeded.
     * @throws RuntimeException If the hardware or HAL reports a critical launch error.
     */
    fun powerUp(): Boolean {
        synchronized(stateLock) {
            Log.d(TAG, "Requesting FM Radio hardware power-up...")
            
            // Check custom hardware permissions
            if (!hasFmHardwarePermission()) {
                throw SecurityException("App does not have access rights to Qualcomm vendor HAL socket nodes.")
            }

            val result = FMBridge.setPower(true)
            if (result == FM_SUCCESS) {
                isPoweredOn = true
                Log.i(TAG, "FM Radio is powered UP and locked to active baseband PLL.")
                return true
            } else {
                handleNativeError("Power Up", result)
                return false
            }
        }
    }

    /**
     * Powers DOWN the FM module to minimize SoC battery consumption.
     */
    fun powerDown(): Boolean {
        synchronized(stateLock) {
            Log.d(TAG, "Requesting FM Radio hardware power-down...")
            val result = FMBridge.setPower(false)
            if (result == FM_SUCCESS) {
                isPoweredOn = false
                Log.i(TAG, "FM Radio successfully powered DOWN. Baseband in sleep state.")
                return true
            } else {
                handleNativeError("Power Down", result)
                return false
            }
        }
    }

    /**
     * Checks if the FM radio is currently powered on.
     */
    fun isPoweredOn(): Boolean {
        synchronized(stateLock) {
            return isPoweredOn
        }
    }

    /**
     * Tunes to a specific frequency.
     * 
     * @param frequencyMHz Target FM frequency (e.g., 94.1f). Supported band: [76.0 - 108.0 MHz].
     * @return True if tuned successfully.
     * @throws IllegalStateException If the tuner is currently powered down.
     * @throws IllegalArgumentException If frequency lies outside standard international bands.
     */
    fun setFrequency(frequencyMHz: Float): Boolean {
        synchronized(stateLock) {
            if (!isPoweredOn) {
                throw IllegalStateException("Cannot set frequency: Qualcomm FM Tuner is powered down.")
            }
            if (frequencyMHz < 76.0f || frequencyMHz > 108.0f) {
                throw IllegalArgumentException("Invalid frequency: $frequencyMHz MHz. Supported band: 76.0 - 108.0 MHz.")
            }

            Log.d(TAG, "Tuning baseband to: $frequencyMHz MHz")
            val result = FMBridge.setFrequency(frequencyMHz)
            if (result == FM_SUCCESS) {
                currentFrequency = frequencyMHz
                Log.i(TAG, "Tuned and locked to $frequencyMHz MHz successfully.")
                return true
            } else {
                handleNativeError("Tune frequency ($frequencyMHz)", result)
                return false
            }
        }
    }

    /**
     * Reads active tuned frequency directly from hardware register PLL locks.
     * 
     * @return Active tuned frequency in MHz.
     */
    fun getCurrentFrequency(): Float {
        synchronized(stateLock) {
            if (!isPoweredOn) {
                Log.w(TAG, "Tuner is powered off; returning cached last-known frequency.")
                return currentFrequency
            }

            val freq = FMBridge.getCurrentFrequency()
            if (freq < 0.0f) {
                Log.w(TAG, "Failed to read physical PLL; returning cached value. Error code: $freq")
                return currentFrequency
            }
            currentFrequency = freq
            return freq
        }
    }

    /**
     * Triggers active hardware automatic search (seek) for clear broadcast stations.
     * 
     * @param upward True to sweep upwards (98.5 -> 98.9...), false to sweep downwards.
     * @return True if a search lock has been successfully initiated.
     */
    fun seek(upward: Boolean): Boolean {
        synchronized(stateLock) {
            if (!isPoweredOn) {
                throw IllegalStateException("Cannot seek: Qualcomm FM Tuner is powered down.")
            }

            val direction = if (upward) 1 else 0
            Log.d(TAG, "Triggering automatic broadcast search. Direction: ${if (upward) "UP" else "DOWN"}")
            val result = FMBridge.seekStation(direction)
            if (result == FM_SUCCESS) {
                // Update frequency cache as seek changes frequency inside hardware registers
                val updatedFreq = FMBridge.getCurrentFrequency()
                if (updatedFreq > 76.0f) {
                    currentFrequency = updatedFreq
                }
                Log.i(TAG, "Auto-seek succeeded. Tuned station locked.")
                return true
            } else {
                handleNativeError("Auto-Seek", result)
                return false
            }
        }
    }

    /**
     * Mutes or unmutes the active analog/digital FM audio path.
     * 
     * @param mute True to silence output, false to restore volume.
     */
    fun setMute(mute: Boolean): Boolean {
        synchronized(stateLock) {
            if (!isPoweredOn) {
                throw IllegalStateException("Cannot mute: Qualcomm FM Tuner is powered down.")
            }

            val result = FMBridge.setMute(mute)
            if (result == FM_SUCCESS) {
                isMuted = mute
                Log.i(TAG, "Audio output mute state set: $mute")
                return true
            } else {
                handleNativeError("Set Mute", result)
                return false
            }
        }
    }

    /**
     * Checks if the FM audio output is currently muted.
     */
    fun isMuted(): Boolean = synchronized(stateLock) { isMuted }

    /**
     * Configures the master gain volume step inside the FM chip.
     * 
     * @param volume Target gain step in range [0 - 15].
     */
    fun setVolume(volume: Int): Boolean {
        synchronized(stateLock) {
            if (!isPoweredOn) {
                throw IllegalStateException("Cannot adjust volume: Qualcomm FM Tuner is powered down.")
            }
            if (volume < 0 || volume > 15) {
                throw IllegalArgumentException("Invalid volume step: $volume. Range is [0 - 15].")
            }

            val result = FMBridge.setVolume(volume)
            if (result == FM_SUCCESS) {
                currentVolume = volume
                Log.i(TAG, "Tuner digital gain volume adjusted: $volume")
                return true
            } else {
                handleNativeError("Set Volume", result)
                return false
            }
        }
    }

    /**
     * Reads cached volume index.
     */
    fun getVolume(): Int = synchronized(stateLock) { currentVolume }

    /**
     * Real-time validation for system hardware-level access credentials.
     */
    private fun hasFmHardwarePermission(): Boolean {
        // Since Qualcomm FM runs under system/vendor space, standard client apps need
        // ACCESS_FM_RADIO system permission or runs under privileged BSP context.
        val hasPermission = context.checkSelfPermission("android.permission.ACCESS_FM_RADIO")
        return hasPermission == android.content.pm.PackageManager.PERMISSION_GRANTED
    }

    /**
     * Translates native C++ dynamic linker & HAL return codes into helpful structured runtime exceptions.
     */
    private fun handleNativeError(operation: String, errorCode: Int) {
        val detail = when (errorCode) {
            FM_ERROR_UNSUPPORTED -> "Dynamic linking failed: hardware configuration unsupported."
            FM_ERROR_HAL_FAILED -> "Qualcomm Snapdragon baseband driver/binder interface returned a critical failure."
            FM_ERROR_NOT_INITIALIZED -> "Tuner PAL layers not properly bound or initialized."
            FM_ERROR_INVALID_PARAM -> "Invalid parameter value passed across JNI boundary."
            FM_ERROR_PERMISSION_DENIED -> "Permissions denied inside SELinux vendor context rules."
            else -> "Unclassified hardware error ($errorCode)."
        }
        val msg = "Qualcomm FM Radio error during '$operation': $detail"
        Log.e(TAG, msg)
        throw RuntimeException(msg)
    }
}
