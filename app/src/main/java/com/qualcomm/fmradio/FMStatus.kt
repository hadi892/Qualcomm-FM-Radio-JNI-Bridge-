/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

package com.qualcomm.fmradio

/**
 * Data class representing the real-time diagnostic and operational state of the Qualcomm FM Radio JNI Bridge.
 */
data class FMStatus(
    val jniLibLoaded: Boolean = false,
    val fmInitialized: Boolean = false,
    val isPoweredOn: Boolean = false,
    val currentFrequencyMHz: Float = 101.9f,
    val lastOperationLog: String = "App started. Waiting for diagnostic action...",
    val completeDiagnosticReport: String = "Press 'Show Complete Diagnostic Report' to query system..."
)
