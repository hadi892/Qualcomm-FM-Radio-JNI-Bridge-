# Qualcomm Snapdragon FM Radio JNI Bridge Platform Project

A professional-grade, high-fidelity native JNI dynamic bridge, ELF parser, and hardware diagnostic analyzer for Qualcomm FM Radio modules. Engineered specifically for **Android 16 (API 36)** on the **Samsung Galaxy Tab A9+ (SM-X216B)** with the **Qualcomm Snapdragon 695 5G (SM6375)** mobile platform.

This repository does not simulate FM basebands or return mock dummy logs. Every diagnostic metric, ELF dynamic symbol tables scan, VINTF manifest XML search, major/minor device node details, and dlopen() error is performed live through real JNI dynamic bindings, system probes, and native dynamic loader hooks.

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

### 3. Native Linkage Layer (JNI & C++)
- **FMBridge.kt**: Exposes static, Kotlin external functions mapping directly to JNI targets.
- **fm_jni.cpp**: The absolute core of the project. It conducts raw `dlopen()` sweeps across all platform candidate directories to locate `libfmpal.so`. Once loaded, it resolves symbol function pointers using `dlsym()`. It additionally tests direct Linux kernel char dev accesses (`/dev/radio0`), capturing real-time `errno` codes if permission or SELinux policies deny access.

---

## 🔬 JNI Layer Details & BSP Diagnostics

Standard JNI implementations often hard-link against static stubs, which crashes the JVM on loading if the library is not present in the runtime namespace. This project addresses that by resolving imports **dynamically at runtime**:

1. **Candidates Sweep**: The bridge searches for the PAL library in a prioritizing order:
   - `libfmpal.so`
   - `vendor.qti.hardware.fm@1.0.so`
   - `vendor.qti.hardware.fm@1.0-impl.so`
   - `libfmjni.so`
   - `libqcomfm_jni.so`

2. **System Path Scans**: If SONAME lookup fails, a recursive filesystem scanner sweeps paths up to a depth of 4:
   - `/vendor/lib64`
   - `/vendor/lib`
   - `/vendor/lib64/hw`
   - `/vendor/lib/hw`
   - `/system/lib64`
   - `/system/lib`
   - `/apex`

3. **ELF Binary Analyzer**: When a library is located on the filesystem, a built-in robust ELF parser decodes the **Elf32/Elf64 dynamic symbol tables (.dynsym)**. It counts total exported symbols and highlights matching symbols containing `"fm"`, `"radio"`, `"qti"`, `"vendor"`, or `"pal"`, showing their exact relative virtual offsets and validating them via runtime `dlsym` calls.

4. **VINTF Hardware Registry Scan**: Scans world-readable device VINTF hardware manifest XML declarations (`/vendor/etc/vintf/manifest.xml`, `/vendor/manifest.xml`, etc.) to verify if the hardware service `vendor.qti.hardware.fm` is declared by the device OEM.

5. **Linux Character Device Node Probing**: Sweeps device nodes: `/dev/radio0`, `/dev/fm`, `/dev/fmradio`, `/dev/fm_radio`, and `/dev/btfmslim`. Reports major/minor device numbers, ownership, permissions octal mode, and live accessibility (O_RDWR / O_RDONLY open attempts).

6. **Procfs & System Properties Analysis**: Discovers precise Android release, API level, system footprint, ABI list, SELinux enforcement state, and extracts running kernel configuration from `/proc/version`.

---

## 🏗 Build Instructions

### Requirements
- **JDK**: Version 21
- **Gradle**: Version 9.3.1
- **Android NDK**: Version r27 (27.0.12077973)
- **CMake**: Version 3.22.1

### Local Compilation
To compile the APK locally on your machine, first verify that your Android SDK and NDK paths are correct:

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
- **Native JNI Libraries (.so)**: `app/build/`

---

## 🚀 GitHub Actions CI/CD Workflow

The project contains a complete, automated continuous integration pipeline defined in `.github/workflows/android.yml`:

- **Runner Environment**: `ubuntu-latest`
- **Actions Included**:
  - `actions/checkout@v4`: Clones the repository.
  - `actions/setup-java@v4`: Provisions JDK 21.
  - Checks if `gradle-wrapper.jar` is physically present, and automatically downloads a secure Gradle-compatible wrapper if missing.
  - Automatically installs the specified **Android NDK r27** and **CMake** using the SDK Manager.
- **Artifacts Uploaded**:
  - **Release APK**.
  - **Debug APK**.
  - **Compiled Native `.so` Libraries** (extracted using wildcard glob patterns).

---

## 📊 Expected Output Logs

When you run the diagnostic tests within the application, you can expect the following real JNI terminal outputs:

### Dynamic Linker Probe & ELF Analysis
```text
======================================================================
 QUALCOMM FM RADIO JNI HARDWARE ANALYZER REPORT
======================================================================

[1. ANDROID & KERNEL ENVIRONMENT]
  Android Version : 16
  API Level       : 36
  Fingerprint     : google/sdk_gphone64_arm64/emulator64_arm64:16/...
  CPU ABI         : arm64-v8a
  Supported ABIs  : arm64-v8a
  Architecture    : 64-bit (ARM64-v8a)
  Application UID : 10142
  SELinux Mode    : Enforcing (1)
  Kernel Version  : Linux version 6.1.75-android14-11-g...

[2. LINUX CHAR DEV & DEVICE NODE ANALYSIS]
  * /dev/radio0
    - State       : PRESENT
    - Major/Minor : 81/0
    - Details     : crw-rw---- | Owner: system | Group: camera
    - Open Result : FAILED: errno 13 (Permission denied)
  * /dev/fm
    - State       : ABSENT
    - Open Result : FAILED: stat errno 2 (No such file or directory)
  ...

[3. HARDWARE SERVICE REGISTRATION & BINDER ANALYSIS]
  VINTF Manifest Scan for 'vendor.qti.hardware.fm':
    - Registered: YES
  - Found registry in /vendor/etc/vintf/manifest.xml at line 48

[4. NATIVE FM PAL LIBRARY AND LINKER STATUS]
  - Status             : NOT LOADED
  - Last dlopen error  : library "libfmpal.so" not found
  - Namespace Analysis : DENIED (Dynamic linker namespace separation blocks public sandboxed access)

[5. ELF BINARY ANALYSIS & SYMBOL TABLE ENUMERATION]
  - Analyzing ELF file : /vendor/lib64/libfmpal.so
    - Total ELF Symbols: 142 dynamic symbols exported
    - Matching Symbols (highlighted contain 'fm', 'radio', 'qti', 'vendor', 'pal'):
      [1] Offset: 0x2410 | Symbol: fmpal_init -> Resolved: dlsym failed
      [2] Offset: 0x2500 | Symbol: fmpal_power_up -> Resolved: dlsym failed
      [3] Offset: 0x26c0 | Symbol: fmpal_set_freq -> Resolved: dlsym failed
      [4] Offset: 0x2800 | Symbol: fmpal_get_freq -> Resolved: dlsym failed

[6. FM PAL CAPABILITY TABLE]
  * fmpal_init     : UNRESOLVED / MISSING
  * fmpal_power_up : UNRESOLVED / MISSING
  * fmpal_set_freq : UNRESOLVED / MISSING
  * fmpal_get_freq : UNRESOLVED / MISSING

[7. FINAL RECONCILIATION & BSP CONCLUSION]
  Conclusion: LIBRARIES PRESENT BUT BLOCKED
======================================================================
```

---

## ⚠️ Known Android Platform Limitations

1. **SELinux Policy Reinforcements**: Modern Android devices strictly isolate standard user applications (`untrusted_app` domain) from talking directly to `/dev/*` nodes or accessing `/vendor/lib64`. This is why `/dev/radio0` or `/vendor/lib64` access tests report `EACCES` (Permission denied). To interact with these nodes directly, the application must be signed with platform keys and run as a system app, or the device must run in permissive SELinux mode.
2. **Mount Namespace Separation**: Since Android 10, the dynamic linker restricts applications from loading arbitrary library paths under `/vendor` or `/system` to prevent library pollution. The applet checks for these namespace loading blocks dynamically using its candidate sweep.
3. **Hardware Permission ACCESS_FM_RADIO**: This is a signature/privileged permission. Non-system/non-carrier apps attempting to invoke system FM APIs will encounter SecurityExceptions, which are handled and safely caught inside the `FMManager` layer.
