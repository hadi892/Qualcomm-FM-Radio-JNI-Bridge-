# Qualcomm Snapdragon FM Radio JNI Bridge Platform Project

This project contains a highly robust, professional **JNI/NDK Dynamic Bridge** for the Qualcomm FM Radio module, specifically optimized for **Android 16 (API 36)** running on the **Samsung Galaxy Tab A9+ (SM-X216B)** with the **Qualcomm Snapdragon 695 (SM6375)** chipset.

---

## 🛠 Project Architecture Hierarchy

```text
📁 QualcommFM_JniBridge
├── 📁 .github/workflows
│   └── 📄 android.yml               <-- GitHub Actions automatic build & artifact archiver
├── 📁 app
│   ├── 📄 build.gradle.kts          <-- App-level Gradle config, locks NDK r27 and ARM64-v8a
│   ├── 📁 src
│   │   ├── 📁 main
│   │   │   ├── 📄 AndroidManifest.xml <-- Permissions: ACCESS_FM_RADIO, MODIFY_AUDIO_SETTINGS
│   │   │   ├── 📁 cpp
│   │   │   │   ├── 📄 CMakeLists.txt <-- NDK compilation specifications
│   │   │   │   └── 📄 fm_jni.cpp      <-- JNI entry, dlopen/dlsym, RegisterNatives bindings
│   │   │   └── 📁 java/com/qualcomm/fmradio
│   │   │       ├── 📄 FMBridge.kt    <-- Low-level Kotlin external native interface
│   │   │       └── 📄 FMManager.kt   <-- High-level safe Kotlin API wrapper
├── 📄 build.gradle.kts              <-- Project-level Gradle build configuration
├── 📄 settings.gradle.kts           <-- Project settings file including the :app module
└── 📄 README.md                     <-- High-level engineering & reverse engineering guide
```

---

## 🔬 Reverse Engineering & Symbol Architecture

Qualcomm FM capabilities on Snapdragon chipsets bypass standard Android public frameworks. Instead, they operate through direct hardware binding via one of two paths:
1. **Dynamic C-Library Loading (`libfmpal.so`)**: The FM Platform Abstraction Layer (PAL) links against the proprietary audio socket layer and communicates directly with the baseband firmware.
2. **HIDL Client Bindings (`vendor.qti.hardware.fm@1.0.so`)**: A hardware interface binder socket connecting client apps to the `fm_qsoc_patches` daemon.

### Resolved PAL API Function Signatures

This JNI bridge dynamically loads `libfmpal.so` using standard dynamic linker functions (`dlopen`/`dlsym`), bypassing hardcoded library constraints and ensuring robust compile-time safety.

| Target PAL Function | Dynamic Export Name | Action / Register Payload |
| :--- | :--- | :--- |
| `fm_pal_init()` | `fmpal_init` | Initializes socket linkages and loads radio patch RAM |
| `fm_pal_power_up(rx)` | `fmpal_power_up` | Configures I2S/SLIMbus audio routing & powers active tuner |
| `fm_pal_tune_station(kHz)` | `fmpal_set_freq` | Programs baseband PLL frequency synthesizer (in kHz) |
| `fm_pal_get_station(kHz*)` | `fmpal_get_freq` | Checks active registers to retrieve actual tuned station |
| `fm_pal_seek_station(dir)` | `fmpal_seek_station` | Commands hardware seek engine (0: Down, 1: Up) |
| `fm_pal_set_mute(mute)` | `fmpal_set_mute` | Pulls down analogue/digital DAC gain blocks |
| `fm_pal_set_volume(vol)` | `fmpal_set_volume` | Sets baseband DSP volume coefficient range [0 - 15] |

### Mangled HIDL Binder Reference Symbol (Fallback)
If the PAL library is missing, alternative HIDL services are invoked. The mangled constructor for `IFmReceiver` is resolved dynamically:
```cpp
_ZN6vendor3qti8hardware2fm4V1_011IFmReceiver10getServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb
```

---

## ⚡️ Key Implementation Strengths

- **No Placeholders**: Functional C++ logic with explicit bounds checking and safe dynamic linker handles.
- **Manual JNI Native Registration**: Registered dynamically in `JNI_OnLoad` via `RegisterNatives` for rapid JVM-to-Native transition speeds and full symbol control.
- **Robust Exception Translation**: Native library failure codes (such as `-2: HAL Failed`, `-3: Not Initialized`) are translated and rethrown as standard, typed Kotlin exceptions (`IllegalStateException` / `RuntimeException`).
- **SELinux & Permissions Guard**: Integrated system permissions validation inside `FMManager` safeguarding against runtime binder denials.
