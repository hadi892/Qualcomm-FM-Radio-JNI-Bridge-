/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Qualcomm Snapdragon 695 (SM6375) FM Radio JNI Bridge
 * Targeted for Android 16 (API 36) - Samsung Galaxy Tab A9+ (SM-X216B)
 * 
 * This JNI implementation runs actual dlopen(), dlsym(), device node open() checks,
 * and reports the real system state, dlerror(), errno, and permissions.
 */

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <string>
#include <sstream>
#include <vector>

#define LOG_TAG "QualcommFM_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Function pointer typedefs matching Qualcomm's FM Platform Abstraction Layer (PAL)
typedef int (*fmpal_init_t)(void);
typedef int (*fmpal_power_up_t)(int);
typedef int (*fmpal_set_freq_t)(int);
typedef int (*fmpal_get_freq_t)(int*);

// Global state holding library handle and function pointers
static void* g_lib_handle = nullptr;
static std::string g_dlopen_error = "Not loaded yet";
static std::string g_last_loaded_path = "";

static fmpal_init_t fn_fmpal_init = nullptr;
static fmpal_power_up_t fn_fmpal_power_up = nullptr;
static fmpal_set_freq_t fn_fmpal_set_freq = nullptr;
static fmpal_get_freq_t fn_fmpal_get_freq = nullptr;

static std::string g_err_fmpal_init = "Not resolved";
static std::string g_err_fmpal_power_up = "Not resolved";
static std::string g_err_fmpal_set_freq = "Not resolved";
static std::string g_err_fmpal_get_freq = "Not resolved";

// Standard device nodes to probe for SELinux and access permission diagnostics
static const char* FM_DEV_NODES[] = {
    "/dev/radio0",
    "/dev/fm",
    "/dev/fmradio",
    "/dev/fm_radio"
};

/**
 * Recursively search a directory up to a max depth of 4 for candidate library names.
 */
static void search_directory(const std::string& path, int depth, const std::vector<std::string>& candidates, std::vector<std::string>& found_paths, std::stringstream& log) {
    if (depth > 4) return;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string full_path = path;
        if (full_path.back() != '/') {
            full_path += "/";
        }
        full_path += name;

        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                search_directory(full_path, depth + 1, candidates, found_paths, log);
            } else if (S_ISREG(st.st_mode)) {
                for (const auto& cand : candidates) {
                    if (name == cand) {
                        found_paths.push_back(full_path);
                        log << "   Found Candidate file: " << full_path << " (" << st.st_size << " bytes)\n";
                    }
                }
            }
        }
    }
    closedir(dir);
}

/**
 * JNI: loadNativeLibrary()
 * Attempts dlopen on candidate Qualcomm libraries and resolves symbols.
 * Returns detailed logs of the discovery and loading process.
 */
extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_loadNativeLibrary(JNIEnv* env, jclass clazz) {
    std::stringstream log;
    log << "[JNI Loader] Starting Qualcomm FM Library Discovery Engine...\n";

    if (g_lib_handle != nullptr) {
        log << "SUCCESS: Library already loaded from " << g_last_loaded_path << "\n";
        log << "Handle: " << g_lib_handle << "\n";
        return env->NewStringUTF(log.str().c_str());
    }

    // Step 1: Create a list of candidate libraries
    std::vector<std::string> candidates = {
        "libfmpal.so",
        "vendor.qti.hardware.fm@1.0.so",
        "vendor.qti.hardware.fm@1.0-impl.so",
        "libqcomfm_jni.so",
        "libfmjni.so"
    };

    bool loaded = false;

    // Step 2: Attempt loading by SONAME first
    log << "\n[STEP 1] Attempting SONAME (Linker Search Path) Load...\n";
    for (const auto& cand : candidates) {
        log << "-> dlopen(\"" << cand << "\", RTLD_NOW | RTLD_GLOBAL)...\n";
        errno = 0;
        void* handle = dlopen(cand.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (handle != nullptr) {
            g_lib_handle = handle;
            g_last_loaded_path = cand;
            g_dlopen_error = "";
            log << "   SUCCESS: Loaded via SONAME " << cand << "\n";
            log << "   Handle: " << handle << "\n";
            loaded = true;
            break;
        } else {
            const char* err = dlerror();
            std::string err_str = err ? err : "None";
            log << "   FAILED: dlerror(): " << err_str << " | errno: " << errno << " (" << strerror(errno) << ")\n";
        }
    }

    // Step 3 & 4: Deep Directory Discovery
    if (!loaded) {
        log << "\n[STEP 2] SONAME loading failed. Running Deep Directory Discovery...\n";
        std::vector<std::string> search_dirs = {
            "/vendor/lib64",
            "/vendor/lib",
            "/vendor/lib64/hw",
            "/vendor/lib/hw",
            "/system/lib64",
            "/system/lib",
            "/apex"
        };

        std::vector<std::string> found_paths;
        for (const auto& dir : search_dirs) {
            log << "-> Scanning directory: " << dir << "\n";
            struct stat dir_st;
            if (stat(dir.c_str(), &dir_st) != 0) {
                log << "   Directory stat failed (errno: " << errno << " - " << strerror(errno) << ")\n";
                if (errno == EACCES) {
                    log << "   [SELinux] Access blocked by security policies.\n";
                }
                continue;
            }
            if (access(dir.c_str(), R_OK) != 0) {
                log << "   Directory is not readable (access R_OK failed)\n";
                continue;
            }

            search_directory(dir, 1, candidates, found_paths, log);
        }

        if (found_paths.empty()) {
            log << "\nCRITICAL: No matching Qualcomm FM library files found in any scanned path.\n";
            g_dlopen_error = "No library files found";
        } else {
            log << "\n[STEP 3] Attempting to load discovered candidate libraries...\n";
            for (const auto& path : found_paths) {
                log << "-> dlopen(\"" << path << "\", RTLD_NOW | RTLD_GLOBAL)...\n";
                errno = 0;
                void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
                if (handle != nullptr) {
                    g_lib_handle = handle;
                    g_last_loaded_path = path;
                    g_dlopen_error = "";
                    log << "   SUCCESS: Loaded library at " << path << "\n";
                    log << "   Handle: " << handle << "\n";
                    loaded = true;
                    break;
                } else {
                    const char* err = dlerror();
                    std::string err_str = err ? err : "None";
                    log << "   FAILED: dlerror(): " << err_str << "\n";
                    log << "           errno: " << errno << " (" << strerror(errno) << ")\n";
                    g_dlopen_error = err_str;
                    
                    if (err_str.find("namespace") != std::string::npos || err_str.find("not accessible") != std::string::npos) {
                        log << "           [Linker Namespace Restriction Detected]\n";
                    }
                }
            }
        }
    }

    if (!loaded) {
        log << "\nFM PAL STATUS: LOAD FAILED. Hardware libraries inaccessible from standard sandbox.\n";
        return env->NewStringUTF(log.str().c_str());
    }

    // Step 8: Resolve symbols using dlsym()
    log << "\n[STEP 4] Resolving FM PAL Symbols...\n";
    
    // Resolve fmpal_init
    log << "-> dlsym(handle, \"fmpal_init\")...\n";
    fn_fmpal_init = (fmpal_init_t)dlsym(g_lib_handle, "fmpal_init");
    if (fn_fmpal_init != nullptr) {
        g_err_fmpal_init = "";
        log << "   ✓ fmpal_init resolved at " << fn_fmpal_init << "\n";
    } else {
        const char* err = dlerror();
        g_err_fmpal_init = err ? err : "Symbol fmpal_init not found";
        log << "   ✗ FAILED: " << g_err_fmpal_init << "\n";
    }

    // Resolve fmpal_power_up
    log << "-> dlsym(handle, \"fmpal_power_up\")...\n";
    fn_fmpal_power_up = (fmpal_power_up_t)dlsym(g_lib_handle, "fmpal_power_up");
    if (fn_fmpal_power_up != nullptr) {
        g_err_fmpal_power_up = "";
        log << "   ✓ fmpal_power_up resolved at " << fn_fmpal_power_up << "\n";
    } else {
        const char* err = dlerror();
        g_err_fmpal_power_up = err ? err : "Symbol fmpal_power_up not found";
        log << "   ✗ FAILED: " << g_err_fmpal_power_up << "\n";
    }

    // Resolve fmpal_set_freq
    log << "-> dlsym(handle, \"fmpal_set_freq\")...\n";
    fn_fmpal_set_freq = (fmpal_set_freq_t)dlsym(g_lib_handle, "fmpal_set_freq");
    if (fn_fmpal_set_freq != nullptr) {
        g_err_fmpal_set_freq = "";
        log << "   ✓ fmpal_set_freq resolved at " << fn_fmpal_set_freq << "\n";
    } else {
        const char* err = dlerror();
        g_err_fmpal_set_freq = err ? err : "Symbol fmpal_set_freq not found";
        log << "   ✗ FAILED: " << g_err_fmpal_set_freq << "\n";
    }

    // Resolve fmpal_get_freq
    log << "-> dlsym(handle, \"fmpal_get_freq\")...\n";
    fn_fmpal_get_freq = (fmpal_get_freq_t)dlsym(g_lib_handle, "fmpal_get_freq");
    if (fn_fmpal_get_freq != nullptr) {
        g_err_fmpal_get_freq = "";
        log << "   ✓ fmpal_get_freq resolved at " << fn_fmpal_get_freq << "\n";
    } else {
        const char* err = dlerror();
        g_err_fmpal_get_freq = err ? err : "Symbol fmpal_get_freq not found";
        log << "   ✗ FAILED: " << g_err_fmpal_get_freq << "\n";
    }

    if (fn_fmpal_init && fn_fmpal_power_up && fn_fmpal_set_freq && fn_fmpal_get_freq) {
        log << "\nFM PAL READY\n";
    } else {
        log << "\nFM PAL PARTIALLY RUNNABLE (Some symbols missing)\n";
    }

    return env->NewStringUTF(log.str().c_str());
}

/**
 * JNI: initFm()
 * Invokes fmpal_init() and reports result.
 */
extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_initFm(JNIEnv* env, jclass clazz) {
    if (g_lib_handle == nullptr) {
        return env->NewStringUTF("ERROR: Native library libfmpal.so not loaded. Call Load Native Library first.");
    }
    if (fn_fmpal_init == nullptr) {
        std::stringstream ss;
        ss << "ERROR: fmpal_init symbol is not resolved. dlerror: " << g_err_fmpal_init;
        return env->NewStringUTF(ss.str().c_str());
    }

    errno = 0;
    int rc = fn_fmpal_init();
    if (rc >= 0) {
        std::stringstream ss;
        ss << "SUCCESS: fmpal_init() returned " << rc;
        return env->NewStringUTF(ss.str().c_str());
    } else {
        std::stringstream ss;
        ss << "FAILED: fmpal_init() returned " << rc << " (errno: " << errno << " - " << strerror(errno) << ")";
        return env->NewStringUTF(ss.str().c_str());
    }
}

/**
 * JNI: setPower(boolean power)
 * Invokes fmpal_power_up(1) or fmpal_power_up(0) or power down counterpart.
 */
extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_setPower(JNIEnv* env, jclass clazz, jboolean power) {
    if (g_lib_handle == nullptr) {
        return env->NewStringUTF("ERROR: Native library libfmpal.so not loaded.");
    }
    if (fn_fmpal_power_up == nullptr) {
        std::stringstream ss;
        ss << "ERROR: fmpal_power_up symbol is not resolved. dlerror: " << g_err_fmpal_power_up;
        return env->NewStringUTF(ss.str().c_str());
    }

    errno = 0;
    int rc = fn_fmpal_power_up(power ? 1 : 0);
    if (rc >= 0) {
        std::stringstream ss;
        ss << "SUCCESS: fmpal_power_up(" << (power ? "1" : "0") << ") returned " << rc;
        return env->NewStringUTF(ss.str().c_str());
    } else {
        std::stringstream ss;
        ss << "FAILED: fmpal_power_up(" << (power ? "1" : "0") << ") returned " << rc << " (errno: " << errno << " - " << strerror(errno) << ")";
        return env->NewStringUTF(ss.str().c_str());
    }
}

/**
 * JNI: setFrequency(float frequencyMHz)
 * Invokes fmpal_set_freq(freqKHz).
 */
extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_setFrequency(JNIEnv* env, jclass clazz, jfloat frequencyMHz) {
    if (g_lib_handle == nullptr) {
        return env->NewStringUTF("ERROR: Native library libfmpal.so not loaded.");
    }
    if (fn_fmpal_set_freq == nullptr) {
        std::stringstream ss;
        ss << "ERROR: fmpal_set_freq symbol is not resolved. dlerror: " << g_err_fmpal_set_freq;
        return env->NewStringUTF(ss.str().c_str());
    }

    int freqKHz = static_cast<int>(frequencyMHz * 1000.0f);
    errno = 0;
    int rc = fn_fmpal_set_freq(freqKHz);
    if (rc >= 0) {
        std::stringstream ss;
        ss << "SUCCESS: fmpal_set_freq(" << freqKHz << " KHz) returned " << rc;
        return env->NewStringUTF(ss.str().c_str());
    } else {
        std::stringstream ss;
        ss << "FAILED: fmpal_set_freq(" << freqKHz << " KHz) returned " << rc << " (errno: " << errno << " - " << strerror(errno) << ")";
        return env->NewStringUTF(ss.str().c_str());
    }
}

/**
 * JNI: getCurrentFrequency()
 * Invokes fmpal_get_freq(&freqKHz).
 */
extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_getCurrentFrequency(JNIEnv* env, jclass clazz) {
    if (g_lib_handle == nullptr) {
        return env->NewStringUTF("ERROR: Native library libfmpal.so not loaded.");
    }
    if (fn_fmpal_get_freq == nullptr) {
        std::stringstream ss;
        ss << "ERROR: fmpal_get_freq symbol is not resolved. dlerror: " << g_err_fmpal_get_freq;
        return env->NewStringUTF(ss.str().c_str());
    }

    int freqKHz = 0;
    errno = 0;
    int rc = fn_fmpal_get_freq(&freqKHz);
    if (rc >= 0) {
        std::stringstream ss;
        ss << "SUCCESS: Frequency: " << (static_cast<float>(freqKHz) / 1000.0f) << " MHz (raw: " << freqKHz << " KHz)";
        return env->NewStringUTF(ss.str().c_str());
    } else {
        std::stringstream ss;
        ss << "FAILED: fmpal_get_freq() returned " << rc << " (errno: " << errno << " - " << strerror(errno) << ")";
        return env->NewStringUTF(ss.str().c_str());
    }
}

/**
 * JNI: getDiagnosticReport()
 * Runs full dynamic loading, SELinux, permissions, and HIDL probes, returning a comprehensive system report.
 */
extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_getDiagnosticReport(JNIEnv* env, jclass clazz) {
    std::stringstream report;

    report << "====================================================\n";
    report << " QUALCOMM FM RADIO JNI DIAGNOSTIC REPORT\n";
    report << "====================================================\n\n";

    // Step 9 Requirement: Detailed status header
    report << "[1. JNI & RUNTIME ENVIRONMENT]\n";
    report << "JNI Loaded: YES\n";
    report << "Native Loader OK: YES\n";
    #if defined(__aarch64__)
    report << "Architecture: ARM64 (64-bit)\n";
    #elif defined(__arm__)
    report << "Architecture: ARM32 (32-bit)\n";
    #else
    report << "Architecture: Non-ARM (" << __VERSION__ << ")\n";
    #endif
    report << "Android API Level: API 36 (Android 16)\n";
    report << "Target Device: Samsung Galaxy Tab A9+ (SM-X216B)\n";
    report << "Qualcomm Snapdragon Platform: SM6375 (Snapdragon 695 5G)\n\n";

    // 2. Library Discovery & Loader status (Step 9 format)
    report << "[2. NATIVE FM PAL LIBRARY STATUS]\n";
    report << "Searching...\n";
    
    std::vector<std::string> candidates = {
        "libfmpal.so",
        "vendor.qti.hardware.fm@1.0.so",
        "vendor.qti.hardware.fm@1.0-impl.so",
        "libqcomfm_jni.so",
        "libfmjni.so"
    };
    std::vector<std::string> search_dirs = {
        "/vendor/lib64",
        "/vendor/lib",
        "/vendor/lib64/hw",
        "/vendor/lib/hw",
        "/system/lib64",
        "/system/lib",
        "/apex"
    };

    std::vector<std::string> discovered;
    std::stringstream search_log;
    for (const auto& dir : search_dirs) {
        search_directory(dir, 1, candidates, discovered, search_log);
    }

    if (!discovered.empty()) {
        report << "Found:\n";
        for (const auto& path : discovered) {
            report << "  " << path << "\n";
        }
    } else {
        report << "Found: None (No library file visible in directory scans)\n";
    }

    report << "Loading...\n";
    if (g_lib_handle != nullptr) {
        report << "SUCCESS\n";
        report << "Handle: " << g_lib_handle << "\n";
        report << "Active Library Path: " << g_last_loaded_path << "\n\n";
        
        report << "Searching Symbols...\n";
        report << (fn_fmpal_init ? "  ✓ fmpal_init" : "  ✗ fmpal_init (Symbol missing)") << "\n";
        report << (fn_fmpal_power_up ? "  ✓ fmpal_power_up" : "  ✗ fmpal_power_up (Symbol missing)") << "\n";
        report << (fn_fmpal_set_freq ? "  ✓ fmpal_set_freq" : "  ✗ fmpal_set_freq (Symbol missing)") << "\n";
        report << (fn_fmpal_get_freq ? "  ✓ fmpal_get_freq" : "  ✗ fmpal_get_freq (Symbol missing)") << "\n\n";

        if (fn_fmpal_init && fn_fmpal_power_up && fn_fmpal_set_freq && fn_fmpal_get_freq) {
            report << "FM PAL READY\n";
        } else {
            report << "FM PAL PARTIALLY RUNNABLE\n";
        }
    } else {
        report << "FAILED\n";
        report << "Reason:\n";
        report << "  dlerror(): " << (g_dlopen_error.empty() ? "None recorded" : g_dlopen_error) << "\n";
        report << "  errno: " << errno << "\n";
        report << "  strerror(): " << strerror(errno) << "\n\n";

        // Assess Namespace restrictions (Step 6)
        report << "Namespace:\n";
        if (g_dlopen_error.find("namespace") != std::string::npos || g_dlopen_error.find("not accessible") != std::string::npos) {
            report << "  Denied (Modern Android Mount Namespace limits public app linking to vendor libraries)\n";
        } else {
            report << "  Allowed (No namespace error captured)\n";
        }
        
        // Assess SELinux restrictions (Step 7)
        report << "SELinux:\n";
        if (errno == EACCES) {
            report << "  Denied (SELinux active protection rules restricted library load or directories read)\n";
        } else {
            report << "  Indeterminate (Standard file permission or missing file state)\n";
        }
    }
    report << "\n";

    // 3. Linux Kernel Device Node Diagnostics
    report << "[3. LINUX CHAR DEV & SELINUX STATUS]\n";
    for (const char* node : FM_DEV_NODES) {
        errno = 0;
        int fd = open(node, O_RDWR);
        if (fd >= 0) {
            report << "  - " << node << ": ACCESSIBLE (O_RDWR open success)\n";
            close(fd);
        } else {
            int err = errno;
            report << "  - " << node << ": FAILED (errno: " << err << " - " << strerror(err) << ")\n";
            if (err == EACCES) {
                report << "    [SELinux] Access denied (untrusted_app SELinux domain is prevented from O_RDWR access)\n";
            } else if (err == ENOENT) {
                report << "    [Driver] Device node does not exist on this system kernel variant.\n";
            }
        }
    }
    report << "\n";

    // 4. HIDL Interface Stack Diagnostics
    report << "[4. HARDWARE INTERFACE STACK PROBES]\n";
    const char* impl_path = "/vendor/lib64/hw/vendor.qti.hardware.fm@1.0-impl.so";
    struct stat st;
    if (stat(impl_path, &st) == 0) {
        report << "  - File vendor.qti.hardware.fm@1.0-impl.so: PRESENT (size: " << st.st_size << " bytes)\n";
    } else {
        report << "  - File vendor.qti.hardware.fm@1.0-impl.so: ABSENT (errno: " << errno << " - " << strerror(errno) << ")\n";
    }

    errno = 0;
    void* hidl_handle = dlopen("vendor.qti.hardware.fm@1.0.so", RTLD_NOW | RTLD_GLOBAL);
    if (hidl_handle != nullptr) {
        report << "  - vendor.qti.hardware.fm@1.0.so dlopen(): SUCCESS\n";
        dlclose(hidl_handle);
    } else {
        const char* err = dlerror();
        report << "  - vendor.qti.hardware.fm@1.0.so dlopen(): FAILED (" << (err ? err : "Unknown linker restriction") << ")\n";
    }

    report << "\n====================================================";
    return env->NewStringUTF(report.str().c_str());
}
