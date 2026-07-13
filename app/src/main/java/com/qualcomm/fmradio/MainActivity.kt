/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

package com.qualcomm.fmradio

import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Interactive Diagnostic Launcher Activity for the Qualcomm Snapdragon FM Radio JNI Bridge.
 * Displays real-time dynamic loader, SELinux, and HAL results coming directly from JNI execution.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var fmManager: FMManager
    
    // UI Elements
    private lateinit var tvJniStatus: TextView
    private lateinit var tvFmStatus: TextView
    private lateinit var tvPowerStatus: TextView
    private lateinit var tvFrequencyStatus: TextView
    private lateinit var tvConsoleLogs: TextView
    private lateinit var scrollConsole: ScrollView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        fmManager = FMManager(this)

        // Build elegant, high-contrast UI programmatically to guarantee zero resource/R link failures
        val rootLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.parseColor("#F5F5F7")) // Elegant soft off-white background
            setPadding(32, 32, 32, 32)
            id = View.generateViewId()
        }

        // Header Card with Device details
        val headerCard = createCardLayout().apply {
            addView(TextView(this@MainActivity).apply {
                text = "Qualcomm Snapdragon FM JNI Bridge"
                textSize = 22f
                setTextColor(Color.parseColor("#1D1D1F")) // Deep Charcoal Gray
                setTypeface(null, Typeface.BOLD)
                id = View.generateViewId()
            })
            addView(TextView(this@MainActivity).apply {
                text = "Target: Samsung Galaxy Tab A9+ (SM-X216B) • Snapdragon 695 • Android 16 (API 36) • ARM64-v8a Only"
                textSize = 12f
                setTextColor(Color.parseColor("#86868B")) // Balanced gray
                setPadding(0, 8, 0, 0)
                id = View.generateViewId()
            })
        }
        rootLayout.addView(headerCard)

        // Status Gauges Board
        val statusCard = createCardLayout().apply {
            addView(TextView(this@MainActivity).apply {
                text = "REAL-TIME JNI & HAL STATUS"
                textSize = 13f
                setTextColor(Color.parseColor("#86868B"))
                setTypeface(null, Typeface.BOLD)
                setPadding(0, 0, 0, 16)
                id = View.generateViewId()
            })

            val statusGrid = LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                weightSum = 4f
                id = View.generateViewId()
            }

            tvJniStatus = createStatusBadge("JNI Loader", "UNLOADED", "#86868B", 1f)
            tvFmStatus = createStatusBadge("FM PAL", "UNINITIALIZED", "#86868B", 1f)
            tvPowerStatus = createStatusBadge("Power State", "POWER DOWN", "#86868B", 1f)
            tvFrequencyStatus = createStatusBadge("Frequency", "101.9 MHz", "#0071E3", 1f) // Apple Blue accent

            statusGrid.addView(tvJniStatus)
            statusGrid.addView(tvFmStatus)
            statusGrid.addView(tvPowerStatus)
            statusGrid.addView(tvFrequencyStatus)
            addView(statusGrid)
        }
        rootLayout.addView(statusCard)

        // Action / Diagnostic Button Console
        val buttonsCard = createCardLayout().apply {
            addView(TextView(this@MainActivity).apply {
                text = "DIAGNOSTIC CONTROLS"
                textSize = 13f
                setTextColor(Color.parseColor("#86868B"))
                setTypeface(null, Typeface.BOLD)
                setPadding(0, 0, 0, 16)
                id = View.generateViewId()
            })

            val row1 = LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                weightSum = 2f
                id = View.generateViewId()
            }
            row1.addView(createActionButton("Load Native Library", View.OnClickListener {
                runDiagnosticAction("Load Native Library") {
                    val res = fmManager.loadNativeLibrary()
                    updateGauges()
                    res
                }
            }, 1f))
            row1.addView(createActionButton("Initialize FM", View.OnClickListener {
                runDiagnosticAction("Initialize FM") {
                    val res = fmManager.initFm()
                    updateGauges()
                    res
                }
            }, 1f))
            addView(row1)

            val row2 = LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                weightSum = 2f
                setPadding(0, 16, 0, 0)
                id = View.generateViewId()
            }
            row2.addView(createActionButton("Power On", View.OnClickListener {
                runDiagnosticAction("Power On") {
                    val res = fmManager.setPower(true)
                    updateGauges()
                    res
                }
            }, 1f))
            row2.addView(createActionButton("Tune 101.9 MHz", View.OnClickListener {
                runDiagnosticAction("Tune 101.9 MHz") {
                    val res = fmManager.setFrequency(101.9f)
                    updateGauges()
                    res
                }
            }, 1f))
            addView(row2)

            val row3 = LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                weightSum = 2f
                setPadding(0, 16, 0, 0)
                id = View.generateViewId()
            }
            row3.addView(createActionButton("Read Current Frequency", View.OnClickListener {
                runDiagnosticAction("Read Current Frequency") {
                    val res = fmManager.getCurrentFrequency()
                    updateGauges()
                    res
                }
            }, 1f))
            row3.addView(createActionButton("Show Diagnostic Report", View.OnClickListener {
                runDiagnosticAction("System Diagnostic Probe") {
                    fmManager.getDiagnosticReport()
                }
            }, 1f))
            addView(row3)
        }
        rootLayout.addView(buttonsCard)

        // Monospaced System Console Outputs
        val consoleCard = createCardLayout().apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            ).apply {
                setMargins(0, 16, 0, 0)
            }
            
            addView(TextView(this@MainActivity).apply {
                text = "NATIVE JNI TERMINAL CONSOLE"
                textSize = 13f
                setTextColor(Color.parseColor("#86868B"))
                setTypeface(null, Typeface.BOLD)
                setPadding(0, 0, 0, 12)
                id = View.generateViewId()
            })

            scrollConsole = ScrollView(this@MainActivity).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT
                )
                background = GradientDrawable().apply {
                    setColor(Color.parseColor("#1D1D1F")) // Smooth dark gray console background
                    cornerRadius = 16f
                }
                setPadding(24, 24, 24, 24)
                id = View.generateViewId()
            }

            tvConsoleLogs = TextView(this@MainActivity).apply {
                text = "System initialized. JNI core binding active.\nTap any control to probe Snapdragon FM baseband..."
                textSize = 12f
                setTextColor(Color.parseColor("#34C759")) // Retro diagnostic bright green
                setTypeface(Typeface.MONOSPACE)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
                id = View.generateViewId()
            }

            scrollConsole.addView(tvConsoleLogs)
            addView(scrollConsole)
        }
        rootLayout.addView(consoleCard)

        setContentView(rootLayout)
        updateGauges()
    }

    private fun createCardLayout(): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                setMargins(0, 0, 0, 16)
            }
            background = GradientDrawable().apply {
                setColor(Color.WHITE)
                cornerRadius = 24f
            }
            setPadding(24, 24, 24, 24)
            id = View.generateViewId()
        }
    }

    private fun createStatusBadge(label: String, initialValue: String, activeColor: String, weight: Float): TextView {
        return TextView(this).apply {
            text = "$label\n$initialValue"
            textSize = 11f
            setTextColor(Color.parseColor(activeColor))
            setTypeface(null, Typeface.BOLD)
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                weight
            )
            id = View.generateViewId()
        }
    }

    private fun createActionButton(label: String, listener: View.OnClickListener, weight: Float): Button {
        return Button(this).apply {
            text = label
            textSize = 12f
            setTextColor(Color.WHITE)
            isAllCaps = false
            setTypeface(null, Typeface.BOLD)
            setOnClickListener(listener)
            
            // Nice modern pill styling
            background = GradientDrawable().apply {
                setColor(Color.parseColor("#1D1D1F"))
                cornerRadius = 16f
            }
            
            layoutParams = LinearLayout.LayoutParams(
                0,
                110, // Height 110px (~48dp conforming touch targets)
                weight
            ).apply {
                setMargins(8, 0, 8, 0)
            }
            id = View.generateViewId()
        }
    }

    private fun runDiagnosticAction(actionName: String, block: () -> String) {
        val formatter = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)
        val timeStamp = formatter.format(Date())
        
        appendConsoleLog("\n[$timeStamp] [ACTION] $actionName triggered.")
        try {
            val result = block()
            appendConsoleLog(result)
        } catch (e: Exception) {
            appendConsoleLog("CRITICAL JNI EXCEPTION:\n${e.message}")
        }
    }

    private fun appendConsoleLog(logText: String) {
        runOnUiThread {
            val currentText = tvConsoleLogs.text.toString()
            val newText = "$currentText\n$logText"
            tvConsoleLogs.text = newText
            
            // Auto scroll console to bottom
            scrollConsole.post {
                scrollConsole.fullScroll(View.FOCUS_DOWN)
            }
        }
    }

    private fun updateGauges() {
        runOnUiThread {
            // Update JNI Loader state
            if (fmManager.isJniLoaded) {
                tvJniStatus.text = "JNI Loader\nLOADED"
                tvJniStatus.setTextColor(Color.parseColor("#34C759")) // Active Green
            } else {
                tvJniStatus.text = "JNI Loader\nUNLOADED"
                tvJniStatus.setTextColor(Color.parseColor("#FF3B30")) // Alert Red
            }

            // Update FM PAL status
            if (fmManager.isFmInitialized) {
                tvFmStatus.text = "FM PAL\nINITIALIZED"
                tvFmStatus.setTextColor(Color.parseColor("#34C759"))
            } else {
                tvFmStatus.text = "FM PAL\nUNINITIALIZED"
                tvFmStatus.setTextColor(Color.parseColor("#86868B"))
            }

            // Update Power state
            if (fmManager.isPoweredOn) {
                tvPowerStatus.text = "Power State\nACTIVE"
                tvPowerStatus.setTextColor(Color.parseColor("#34C759"))
            } else {
                tvPowerStatus.text = "Power State\nPOWER DOWN"
                tvPowerStatus.setTextColor(Color.parseColor("#86868B"))
            }

            // Update Frequency
            tvFrequencyStatus.text = "Frequency\n${String.format(Locale.US, "%.1f", fmManager.currentFrequencyMHz)} MHz"
        }
    }
}
