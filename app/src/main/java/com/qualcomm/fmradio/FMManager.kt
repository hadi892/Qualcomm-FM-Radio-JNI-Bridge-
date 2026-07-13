/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

package com.qualcomm.fmradio

import android.content.Context
import android.content.pm.PackageManager
import android.util.Log

/**
 * Thread-safe API manager that wraps lower-level FMBridge JNI invocations
 * and exposes clean diagnostics and states for MainActivity.
 */
class FMManager(private val context: Context) {

    companion object {
        private const val TAG = "QualcommFM_Manager"
    }

    private val stateLock = Any()
    
    // In-memory cache of current hardware states (updated during JNI calls)
    var isJniLoaded = false
        private set
    var isFmInitialized = false
        private set
    var isPoweredOn = false
        private set
    var currentFrequencyMHz = 101.9f
        private set

    /**
     * Executes native load diagnostics.
     */
    fun loadNativeLibrary(): String {
        synchronized(stateLock) {
            Log.d(TAG, "Invoking low-level JNI dlopen/dlsym sequence...")
            val result = FMBridge.loadNativeLibrary()
            if (result.contains("SUCCESS: Loaded")) {
                isJniLoaded = true
            }
            return result
        }
    }

    /**
     * Executes native fmpal_init.
     */
    fun initFm(): String {
        synchronized(stateLock) {
            Log.d(TAG, "Invoking native fmpal_init()...")
            val result = FMBridge.initFm()
            if (result.contains("SUCCESS:")) {
                isFmInitialized = true
            }
            return result
        }
    }

    /**
     * Executes native fmpal_power_up.
     */
    fun setPower(power: Boolean): String {
        synchronized(stateLock) {
            Log.d(TAG, "Invoking native fmpal_power_up($power)...")
            val result = FMBridge.setPower(power)
            if (result.contains("SUCCESS:")) {
                isPoweredOn = power
            }
            return result
        }
    }

    /**
     * Executes native fmpal_set_freq.
     */
    fun setFrequency(frequencyMHz: Float): String {
        synchronized(stateLock) {
            Log.d(TAG, "Invoking native fmpal_set_freq($frequencyMHz)...")
            val result = FMBridge.setFrequency(frequencyMHz)
            if (result.contains("SUCCESS:")) {
                currentFrequencyMHz = frequencyMHz
            }
            return result
        }
    }

    /**
     * Executes native fmpal_get_freq.
     */
    fun getCurrentFrequency(): String {
        synchronized(stateLock) {
            Log.d(TAG, "Invoking native fmpal_get_freq()...")
            val result = FMBridge.getCurrentFrequency()
            // Try to extract frequency from JNI success message (e.g., "SUCCESS: Frequency: 101.9 MHz")
            if (result.contains("SUCCESS: Frequency:")) {
                try {
                    val parts = result.split(" ")
                    for (i in parts.indices) {
                        if (parts[i] == "Frequency:" && i + 1 < parts.size) {
                            currentFrequencyMHz = parts[i + 1].toFloat()
                            break
                        }
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to parse frequency from JNI result string: ${e.message}")
                }
            }
            return result
        }
    }

    /**
     * Returns a full, deep diagnostic probe of dynamic linking, SELinux, permissions, and HIDL.
     */
    fun getDiagnosticReport(): String {
        synchronized(stateLock) {
            Log.d(TAG, "Generating live native diagnostic report...")
            return FMBridge.getDiagnosticReport()
        }
    }

    /**
     * Verifies if the app possesses target hardware execution permissions.
     */
    fun hasFmPermission(): Boolean {
        return context.checkSelfPermission("android.permission.ACCESS_FM_RADIO") == PackageManager.PERMISSION_GRANTED
    }
}
