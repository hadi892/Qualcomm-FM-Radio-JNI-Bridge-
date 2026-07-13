# Qualcomm Snapdragon FM Radio JNI Bridge Platform Project

A professional-grade, high-fidelity native JNI dynamic bridge and diagnostic launcher application for Qualcomm FM Radio modules. Engineered specifically for **Android 16 (API 36)** on the **Samsung Galaxy Tab A9+ (SM-X216B)** with the **Qualcomm Snapdragon 695 5G (SM6375)** mobile platform.

This repository does not simulate FM basebands or return mock dummy logs. Every diagnostic metric, dlopen() error, and device node access check is performed live through real JNI dynamic bindings, system probes, and native dynamic loader hooks.

---

## 🛠 Architectural Design

The platform uses a layered full-stack architectural design to bridge Kotlin applications running in standard JVM userspace down to the low-level Linux character device drivers and Qualcomm Platform Abstraction Layers (PAL):

```text
 ┌─────────────────────────────────────────────────────────┐
 │               MainActivity (Kotlin UI)                  │
 └────────────────────────────┬────────────────────────────┘
                              │
 ┌────────────────────────────▼────────────────────────────┐
 │                  FMManager (API Wrapper)                │
 └────────────────────────────┬────────────────────────────┘
                              │
 ┌────────────────────────────▼────────────────────────────┐
 │               FMBridge (Kotlin JNI Interface)           │
 └────────────────────────────┬────────────────────────────┘
                              │ (Native JNI Border Crossing)
 ┌────────────────────────────▼────────────────────────────┐
 │                   fm_jni.cpp (C++ Layer)                │
 └────────────────────────────┬────────────────────────────┘
         ┌────────────────────┴────────────────────┐
         │ (dlopen / dlsym)                        │ (open / O_RDWR)
 ┌───────▼────────────────────────┐        ┌───────▼────────────────────────┐
 │ libfmpal.so (Qualcomm FM PAL)  │        │ /dev/radio0 (Linux Char Dev)   │
 └───────┬────────────────────────┘        └───────┬────────────────────────┘
         │ (Binder IPC / socket)                   │ (Direct V4L2 IOCTL)
 ┌───────▼────────────────────────┐        ┌───────▼────────────────────────┐
 │ Qualcomm Radio Firmware (SoC)  │        │ Qualcomm FM Radio Driver       │
 └────────────────────────────────┘        └────────────────────────────────┘
```

### 1. Presentation Layer (Kotlin UI)
- **MainActivity.kt**: Implements a native, high-contrast Material design single-screen launcher panel. It includes live status indicators (gauges) showing whether the JNI bridge is loaded, FM PAL is active, the current power status, and tuned frequency. 
- A scrolling **JNI Terminal Console** displays live, raw text logs returned directly from the C++ userspace with millisecond-precision timestamps.

### 2. Management Layer (Kotlin)
- **FMManager.kt**: A thread-safe coordinator that maintains the cache of device states and wraps low-level `FMBridge` calls. It contains validation rules and exposes high-level, clear diagnostic methods to the visual activity.
- **FMStatus.kt**: Simple data holder for operational state.

### 3. Native Linkage Layer (JNI & C++)
- **FMBridge.kt**: Exposes static, Kotlin external functions mapping directly to JNI targets.
- **fm_jni.cpp**: The absolute core of the project. It conducts raw `dlopen()` sweeps across all platform candidate directories to locate `libfmpal.so`. Once loaded, it resolves symbol function pointers using `dlsym()`. It additionally tests direct Linux kernel char dev accesses (`/dev/radio0`), capturing real-time `errno` codes if permission or SELinux policies deny access.

---

## 🔬 JNI Layer Details

Standard JNI implementations often hard-link against static stubs, which crashes the JVM on loading if the library is not present in the runtime namespace. This project addresses that by resolving imports **dynamically at runtime**:

1. **Candidates Sweep**: The bridge searches for the PAL library in a prioritizing order:
   - `libfmpal.so`
   - `/vendor/lib64/libfmpal.so`
   - `/vendor/lib/libfmpal.so`
   - `/system/lib64/libfmpal.so`
   - `/system/lib/libfmpal.so`
   - `/vendor/lib64/hw/vendor.qti.hardware.fm@1.0-impl.so`
   - `/vendor/lib/hw/vendor.qti.hardware.fm@1.0-impl.so`
   - `vendor.qti.hardware.fm@1.0.so`

2. **Symbol Resolution**: It utilizes `dlopen()` to pull the library into memory and uses `dlsym()` to resolve exact function entry points:
   - `fmpal_init`
   - `fmpal_power_up`
   - `fmpal_set_freq`
   - `fmpal_get_freq`

3. **Exception Capture**: If any step fails (e.g., library missing or restricted), JNI captures the output of `dlerror()` and `errno` and pipes them directly back into the UI console as standard strings. It never hides or replaces failures with generic placeholders.

---

## ⚙️ CMake Configuration

The library compilation is governed by a secure, hardened `CMakeLists.txt` targeting modern C++20:

- **Enforces C++20 Standard**: `set(CMAKE_CXX_STANDARD 20)`
- **Hardening and Security Flags**:
  - `-O3`: Maximum compiler optimizations.
  - `-fstack-protector-strong`: Protects against stack buffer overflows.
  - `-Wall -Werror`: Emits all warnings and elevates them to build-blocking errors.
  - `-D_FORTIFY_SOURCE=2`: Detects buffer overflows in common standard library calls.
- **Linker Rules**: Links explicitly against NDK logging (`log`), android utilities (`android`), and the dynamic loader library (`dl`).

---

## 💻 Native Bridge Diagnostics

Probing the Qualcomm FM interface on modern Android targets requires bypassing multi-layered kernel and SELinux obstacles. The bridge diagnostic module performs several direct system probes:

1. **SELinux Dev Node Test**:
   - Tries to open `/dev/radio0`, `/dev/fm`, `/dev/fmradio`, or `/dev/fm_radio` with read-write flags (`O_RDWR`).
   - If blocked, checks `errno`. If `EACCES` (Permission Denied) is returned, a SELinux domain restriction/denial is actively preventing standard user apps from talking to the radio device driver.

2. **Namespace Path Visibility Check**:
   - Attempts to list the `/vendor/lib64` directory using the `opendir` / `readdir` kernel APIs.
   - Diagnoses if modern Android Mount Namespace separation restricts library directory listings.

3. **HIDL Service Probe**:
   - Probes `vendor.qti.hardware.fm@1.0.so` to check binder socket connectivity.

---

## 🏗 Build Instructions

### Requirements
- **JDK**: Version 21
- **Gradle**: Version 9.3.1
- **Android NDK**: Version r27 (27.0.12077973)
- **CMake**: Version 3.22.1

### Local Compilation
To compile the APK locally on your machine, first copy `local.properties` and verify that your Android SDK and NDK paths are correct:

```bash
# Clean previous builds
./gradlew clean

# Build Debug APK
./gradlew assembleDebug

# Build Release APK
./gradlew assembleRelease
```

The compiled binaries will be output directly to:
- **Debug APK**: `app/build/outputs/apk/debug/app-debug.apk`
- **Release APK**: `app/build/outputs/apk/release/app-release-unsigned.apk`
- **Native JNI Libraries (.so)**: `app/build/intermediates/stripped_native_libs/`

---

## 🚀 GitHub Actions CI/CD Workflow

The project contains a complete, automated continuous integration pipeline defined in `.github/workflows/android.yml`:

- **Runner Environment**: `ubuntu-latest`
- **Actions Included**:
  - `actions/checkout@v4`: Clones the repository.
  - `actions/setup-java@v4`: Provisions JDK 21.
  - `subosito/flutter-action` or Gradle caching to speed up compilation.
  - Checks if `gradle-wrapper.jar` is physically present, and automatically downloads a secure Gradle 8.8.0/9.3.1 compatible wrapper if missing.
  - Automatically installs the specified **Android NDK r27** and **CMake** using the SDK Manager.
- **Artifacts Uploaded**:
  - **Release APK** (with automatic wildcard pathing).
  - **Debug APK**.
  - **ProGuard Mapping Files** (if mapping is enabled).
  - **Compiled Native `.so` Libraries** (extracted from the arm64-v8a intermediates).

---

## 📊 Expected Output Logs

When you run the diagnostic tests within the application, you can expect the following real JNI terminal outputs:

### 1. Dynamic Linker Probe (Successful Library Load on Target Device)
```text
[12:15:32.415] [ACTION] Load Native Library triggered.
[JNI] Loading Qualcomm FM PAL libraries...
-> Trying dlopen("libfmpal.so", RTLD_NOW)...
   FAILED: library "libfmpal.so" not found
-> Trying dlopen("/vendor/lib64/libfmpal.so", RTLD_NOW)...
   SUCCESS: Loaded /vendor/lib64/libfmpal.so
-> Resolving symbol 'fmpal_init'...
   SUCCESS: fmpal_init resolved at 0x7b4a2050f0
-> Resolving symbol 'fmpal_power_up'...
   SUCCESS: fmpal_power_up resolved at 0x7b4a205210
-> Resolving symbol 'fmpal_set_freq'...
   SUCCESS: fmpal_set_freq resolved at 0x7b4a205340
-> Resolving symbol 'fmpal_get_freq'...
   SUCCESS: fmpal_get_freq resolved at 0x7b4a2054c0
```

### 2. Device Probes (When Run on a Sandbox / Non-Rooted Device)
```text
[12:16:11.890] [ACTION] System Diagnostic Probe triggered.
====================================================
 QUALCOMM FM RADIO JNI DIAGNOSTIC REPORT
====================================================

[1. JNI & RUNTIME ENVIRONMENT]
  - Architecture: ARM64 (64-bit)
  - Android API Level: API 36 (Android 16)
  - Target Device Match: Samsung Galaxy Tab A9+ (SM-X216B)
  - Qualcomm Snapdragon: SM6375 (Snapdragon 695 5G)

[2. DYNAMIC LINKER (libfmpal.so)]
  - Load Status: FAILED
  - Last dlopen() error: library "libfmpal.so" not found
  - Symbol mappings are unlinked.

[3. HIDL / INTERFACE STACK DIAGNOSTICS]
  - File vendor.qti.hardware.fm@1.0-impl.so: ABSENT (errno: 2 - No such file or directory)
  - vendor.qti.hardware.fm@1.0.so dlopen(): FAILED (library "vendor.qti.hardware.fm@1.0.so" not found)

[4. SELINUX & LINUX CHAR DEV DIAGNOSTICS]
  - /dev/radio0: FAILED (errno: 13 - Permission denied)
    [SELinux] Potential SELinux domain restriction / denial active on this node.
  - /dev/fm: FAILED (errno: 2 - No such file or directory)

[5. NAMESPACE SYSTEM PATH ACCESSIBILITY]
  - Read /vendor/lib64: DENIED (errno: 13 - Permission denied)

====================================================
```

---

## ⚠️ Known Android Platform Limitations

When running this diagnostic tool on newer Android releases (especially Android 13 to Android 16), please keep the following platform safeguards in mind:

1. **SELinux Policy Reinforcements**: Modern Android devices strictly isolate standard user applications (`untrusted_app` domain) from talking directly to `/dev/*` nodes or accessing `/vendor/lib64`. This is why `/dev/radio0` or `/vendor/lib64` access tests report `EACCES` (Permission denied). To interact with these nodes directly, the application must be signed with platform keys and run as a system app, or the device must run in permissive SELinux mode.
2. **Mount Namespace Separation**: Since Android 10, the dynamic linker restricts applications from loading arbitrary library paths under `/vendor` or `/system` to prevent library pollution. The applet checks for these namespace loading blocks dynamically using its candidate sweep.
3. **Hardware Permission ACCESS_FM_RADIO**: This is a signature/privileged permission. Non-system/non-carrier apps attempting to invoke system FM APIs will encounter SecurityExceptions, which are handled and safely caught inside the `FMManager` layer.
