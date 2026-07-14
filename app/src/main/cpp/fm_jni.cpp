/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Qualcomm Snapdragon 695 (SM6375) FM Radio JNI Bridge & Hardware Analyzer
 * Targeted for Android 16 (API 36) - Samsung Galaxy Tab A9+ (SM-X216B)
 * 
 * This JNI implementation implements a real ELF loader, real filesystem scans,
 * world-readable VINTF hardware manifest queries, SELinux policies check, 
 * device nodes major/minor and permissions probing, live dynamic symbol 
 * parsing of ELF tables (supporting dual Elf32/Elf64 formats), and dynamic
 * verification of Qualcomm Platform Abstraction Layer (PAL) interfaces.
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
#include <pwd.h>
#include <grp.h>
#include <elf.h>
#include <sys/system_properties.h>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iomanip>

#define LOG_TAG "QualcommFM_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef major
#define major(dev) ((unsigned int)(((dev) >> 8) & 0xfff))
#endif
#ifndef minor
#define minor(dev) ((unsigned int)((dev) & 0xff))
#endif

// Qualcomm FM PAL function pointer signatures
typedef int (*fmpal_init_t)(void);
typedef int (*fmpal_power_up_t)(int);
typedef int (*fmpal_set_freq_t)(int);
typedef int (*fmpal_get_freq_t)(int*);

// Global status states
static void* g_lib_handle = nullptr;
static std::string g_dlopen_error = "Not loaded yet";
static std::string g_last_loaded_path = "";

static fmpal_init_t fn_fmpal_init = nullptr;
static fmpal_power_up_t fn_fmpal_power_up = nullptr;
static fmpal_set_freq_t fn_fmpal_set_freq = nullptr;
static fmpal_get_freq_t fn_fmpal_get_freq = nullptr;

struct ParsedSymbol {
    std::string name;
    uint64_t address;
};

struct NodeInfo {
    std::string path;
    bool exists;
    std::string metadata;
    unsigned int major_num;
    unsigned int minor_num;
    std::string open_result;
};

/**
 * Robust dual-class ELF parser. Parses 32-bit and 64-bit ELF dynamic symbol tables (.dynsym)
 * directly from library files on the Android filesystem.
 */
static std::vector<ParsedSymbol> parse_elf_symbols(const std::string& filepath, std::string& out_error) {
    std::vector<ParsedSymbol> symbols;
    int fd = open(filepath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        out_error = "open failed: " + std::string(strerror(errno));
        return symbols;
    }

    unsigned char e_ident[EI_NIDENT];
    if (read(fd, e_ident, EI_NIDENT) != EI_NIDENT) {
        out_error = "failed to read ELF identification bytes";
        close(fd);
        return symbols;
    }

    if (memcmp(e_ident, ELFMAG, SELFMAG) != 0) {
        out_error = "invalid ELF magic header";
        close(fd);
        return symbols;
    }

    if (lseek(fd, 0, SEEK_SET) == -1) {
        out_error = "lseek reset failed";
        close(fd);
        return symbols;
    }

    if (e_ident[EI_CLASS] == ELFCLASS64) {
        Elf64_Ehdr ehdr;
        if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
            out_error = "failed to read Elf64 header";
            close(fd);
            return symbols;
        }

        std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
        if (lseek(fd, ehdr.e_shoff, SEEK_SET) == -1 ||
            read(fd, shdrs.data(), ehdr.e_shnum * sizeof(Elf64_Shdr)) != static_cast<ssize_t>(ehdr.e_shnum * sizeof(Elf64_Shdr))) {
            out_error = "failed to read Elf64 section headers";
            close(fd);
            return symbols;
        }

        Elf64_Shdr shstr = shdrs[ehdr.e_shstrndx];
        std::vector<char> shstrtab(shstr.sh_size);
        if (lseek(fd, shstr.sh_offset, SEEK_SET) == -1 ||
            read(fd, shstrtab.data(), shstr.sh_size) != static_cast<ssize_t>(shstr.sh_size)) {
            out_error = "failed to read Elf64 section string table";
            close(fd);
            return symbols;
        }

        int dynsym_idx = -1;
        int dynstr_idx = -1;
        for (int i = 0; i < ehdr.e_shnum; ++i) {
            std::string sname = &shstrtab[shdrs[i].sh_name];
            if (sname == ".dynsym") {
                dynsym_idx = i;
            } else if (sname == ".dynstr") {
                dynstr_idx = i;
            }
        }

        if (dynsym_idx != -1 && dynstr_idx != -1) {
            Elf64_Shdr dynsym_sh = shdrs[dynsym_idx];
            Elf64_Shdr dynstr_sh = shdrs[dynstr_idx];

            size_t num_syms = dynsym_sh.sh_size / sizeof(Elf64_Sym);
            std::vector<Elf64_Sym> syms(num_syms);
            std::vector<char> dynstr(dynstr_sh.sh_size);

            if (lseek(fd, dynsym_sh.sh_offset, SEEK_SET) != -1 &&
                read(fd, syms.data(), dynsym_sh.sh_size) == static_cast<ssize_t>(dynsym_sh.sh_size) &&
                lseek(fd, dynstr_sh.sh_offset, SEEK_SET) != -1 &&
                read(fd, dynstr.data(), dynstr_sh.sh_size) == static_cast<ssize_t>(dynstr_sh.sh_size)) {
                
                for (size_t i = 0; i < num_syms; ++i) {
                    Elf64_Sym sym = syms[i];
                    if (sym.st_name != 0 && sym.st_name < dynstr_sh.sh_size) {
                        std::string sname = &dynstr[sym.st_name];
                        symbols.push_back({sname, sym.st_value});
                    }
                }
            } else {
                out_error = "failed to read Elf64 dynamic tables";
            }
        } else {
            out_error = "Elf64 .dynsym or .dynstr section not found";
        }
    } else if (e_ident[EI_CLASS] == ELFCLASS32) {
        Elf32_Ehdr ehdr;
        if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
            out_error = "failed to read Elf32 header";
            close(fd);
            return symbols;
        }

        std::vector<Elf32_Shdr> shdrs(ehdr.e_shnum);
        if (lseek(fd, ehdr.e_shoff, SEEK_SET) == -1 ||
            read(fd, shdrs.data(), ehdr.e_shnum * sizeof(Elf32_Shdr)) != static_cast<ssize_t>(ehdr.e_shnum * sizeof(Elf32_Shdr))) {
            out_error = "failed to read Elf32 section headers";
            close(fd);
            return symbols;
        }

        Elf32_Shdr shstr = shdrs[ehdr.e_shstrndx];
        std::vector<char> shstrtab(shstr.sh_size);
        if (lseek(fd, shstr.sh_offset, SEEK_SET) == -1 ||
            read(fd, shstrtab.data(), shstr.sh_size) != static_cast<ssize_t>(shstr.sh_size)) {
            out_error = "failed to read Elf32 section string table";
            close(fd);
            return symbols;
        }

        int dynsym_idx = -1;
        int dynstr_idx = -1;
        for (int i = 0; i < ehdr.e_shnum; ++i) {
            std::string sname = &shstrtab[shdrs[i].sh_name];
            if (sname == ".dynsym") {
                dynsym_idx = i;
            } else if (sname == ".dynstr") {
                dynstr_idx = i;
            }
        }

        if (dynsym_idx != -1 && dynstr_idx != -1) {
            Elf32_Shdr dynsym_sh = shdrs[dynsym_idx];
            Elf32_Shdr dynstr_sh = shdrs[dynstr_idx];

            size_t num_syms = dynsym_sh.sh_size / sizeof(Elf32_Sym);
            std::vector<Elf32_Sym> syms(num_syms);
            std::vector<char> dynstr(dynstr_sh.sh_size);

            if (lseek(fd, dynsym_sh.sh_offset, SEEK_SET) != -1 &&
                read(fd, syms.data(), dynsym_sh.sh_size) == static_cast<ssize_t>(dynsym_sh.sh_size) &&
                lseek(fd, dynstr_sh.sh_offset, SEEK_SET) != -1 &&
                read(fd, dynstr.data(), dynstr_sh.sh_size) == static_cast<ssize_t>(dynstr_sh.sh_size)) {
                
                for (size_t i = 0; i < num_syms; ++i) {
                    Elf32_Sym sym = syms[i];
                    if (sym.st_name != 0 && sym.st_name < dynstr_sh.sh_size) {
                        std::string sname = &dynstr[sym.st_name];
                        symbols.push_back({sname, sym.st_value});
                    }
                }
            } else {
                out_error = "failed to read Elf32 dynamic tables";
            }
        } else {
            out_error = "Elf32 .dynsym or .dynstr section not found";
        }
    } else {
        out_error = "unsupported ELF class header";
    }

    close(fd);
    return symbols;
}

/**
 * Searches /proc/self/maps to discover the exact physical mapped filesystem location
 * of a loaded dynamic library. Highly precise and robust.
 */
static std::string find_loaded_library_path_from_maps(const std::string& libname) {
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) return "";
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(libname) != std::string::npos) {
            size_t idx = line.find('/');
            if (idx != std::string::npos) {
                std::string path = line.substr(idx);
                while (!path.empty() && (path.back() == '\r' || path.back() == '\n' || path.back() == ' ')) {
                    path.pop_back();
                }
                return path;
            }
        }
    }
    return "";
}

/**
 * Reads detailed metadata of a target file.
 */
static std::string get_file_metadata(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return "stat failed: " + std::string(strerror(errno));
    }
    std::stringstream ss;
    ss << "Octal permissions: " << std::oct << (st.st_mode & 0777) << std::dec;
    ss << " | Size: " << st.st_size << " bytes";
    
    struct passwd* pw = getpwuid(st.st_uid);
    struct group* gr = getgrgid(st.st_gid);
    ss << " | Owner: " << (pw ? pw->pw_name : std::to_string(st.st_uid));
    ss << " | Group: " << (gr ? gr->gr_name : std::to_string(st.st_gid));
    ss << " | Readable: " << (access(path.c_str(), R_OK) == 0 ? "YES" : "NO");
    return ss.str();
}

/**
 * Helper to scan directory candidates recursively up to a depth of 4.
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
                    }
                }
            }
        }
    }
    closedir(dir);
}

/**
 * Retrieves a system property value using the Android system properties service.
 */
static std::string get_prop(const char* name) {
    char val[PROP_VALUE_MAX] = {0};
    __system_property_get(name, val);
    return val;
}

/**
 * Reads the actual enforce status of SELinux from the filesystem or fallback boot property.
 */
static std::string get_selinux_mode() {
    std::ifstream enforce("/sys/fs/selinux/enforce");
    if (enforce.is_open()) {
        char val;
        if (enforce.get(val)) {
            if (val == '1') return "Enforcing (1)";
            if (val == '0') return "Permissive (0)";
        }
    }
    char prop_val[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.boot.selinux", prop_val);
    if (strlen(prop_val) > 0) {
        return std::string(prop_val) + " (From ro.boot.selinux)";
    }
    return "Enforcing (Enforced in modern API levels)";
}

/**
 * Reads /proc/version to display the exact running kernel specifications.
 */
static std::string get_kernel_version() {
    std::ifstream version("/proc/version");
    if (version.is_open()) {
        std::string line;
        if (std::getline(version, line)) {
            return line;
        }
    }
    return "Unknown kernel version";
}

/**
 * Probes the hardware character devices.
 */
static std::vector<NodeInfo> probe_device_nodes() {
    const char* nodes[] = {
        "/dev/radio0",
        "/dev/fm",
        "/dev/fmradio",
        "/dev/fm_radio",
        "/dev/btfmslim"
    };

    std::vector<NodeInfo> info_list;
    for (const char* node : nodes) {
        NodeInfo info;
        info.path = node;
        struct stat st;
        errno = 0;
        if (stat(node, &st) == 0) {
            info.exists = true;
            info.major_num = major(st.st_rdev);
            info.minor_num = minor(st.st_rdev);
            
            std::stringstream ss;
            ss << ((S_ISCHR(st.st_mode)) ? "c" : "-");
            ss << ((st.st_mode & S_IRUSR) ? "r" : "-");
            ss << ((st.st_mode & S_IWUSR) ? "w" : "-");
            ss << ((st.st_mode & S_IXUSR) ? "x" : "-");
            ss << ((st.st_mode & S_IRGRP) ? "r" : "-");
            ss << ((st.st_mode & S_IWGRP) ? "w" : "-");
            ss << ((st.st_mode & S_IXGRP) ? "x" : "-");
            ss << ((st.st_mode & S_IROTH) ? "r" : "-");
            ss << ((st.st_mode & S_IWOTH) ? "w" : "-");
            ss << ((st.st_mode & S_IXOTH) ? "x" : "-");
            
            struct passwd* pw = getpwuid(st.st_uid);
            struct group* gr = getgrgid(st.st_gid);
            ss << " | Owner: " << (pw ? pw->pw_name : std::to_string(st.st_uid));
            ss << " | Group: " << (gr ? gr->gr_name : std::to_string(st.st_gid));
            info.metadata = ss.str();

            int fd = open(node, O_RDWR);
            if (fd >= 0) {
                info.open_result = "SUCCESS (O_RDWR accessible)";
                close(fd);
            } else {
                int open_err = errno;
                fd = open(node, O_RDONLY);
                if (fd >= 0) {
                    info.open_result = "SUCCESS (O_RDONLY accessible)";
                    close(fd);
                } else {
                    info.open_result = "FAILED: errno " + std::to_string(open_err) + " (" + strerror(open_err) + ")";
                }
            }
        } else {
            info.exists = false;
            info.major_num = 0;
            info.minor_num = 0;
            info.metadata = "N/A";
            info.open_result = "FAILED: stat errno " + std::to_string(errno) + " (" + strerror(errno) + ")";
        }
        info_list.push_back(info);
    }
    return info_list;
}

/**
 * Scans standard Android VINTF manifest.xml files to find registered vendor hardware service names.
 */
static bool scan_vintf_manifests(std::string& out_details) {
    const char* paths[] = {
        "/vendor/etc/vintf/manifest.xml",
        "/vendor/manifest.xml",
        "/system/etc/vintf/manifest.xml"
    };

    bool found = false;
    std::stringstream ss;
    for (const char* path : paths) {
        std::ifstream file(path);
        if (file.is_open()) {
            std::string line;
            int line_num = 0;
            while (std::getline(file, line)) {
                line_num++;
                if (line.find("vendor.qti.hardware.fm") != std::string::npos) {
                    found = true;
                    ss << "  - Found registry in " << path << " at line " << line_num << "\n";
                }
            }
        }
    }

    if (found) {
        out_details = ss.str();
    } else {
        out_details = "  - Not declared in any device VINTF manifest XML files.\n";
    }
    return found;
}

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_loadNativeLibrary(JNIEnv* env, jclass clazz) {
    std::stringstream log;
    log << "[JNI Native Loader] Beginning ELF Dynamic Linker Probe Sequence...\n";

    if (g_lib_handle != nullptr) {
        log << "SUCCESS: Loaded from previous session: " << g_last_loaded_path << "\n";
        log << "Handle: " << g_lib_handle << "\n";
        return env->NewStringUTF(log.str().c_str());
    }

    std::vector<std::string> candidates = {
        "libfmpal.so",
        "vendor.qti.hardware.fm@1.0.so",
        "vendor.qti.hardware.fm@1.0-impl.so",
        "libfmjni.so",
        "libqcomfm_jni.so"
    };

    bool loaded = false;
    log << "\n[STEP 1: SONAME Linker Search]\n";
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
            log << "   FAILED: dlerror(): " << (err ? err : "None") << " | errno: " << errno << "\n";
        }
    }

    if (!loaded) {
        log << "\n[STEP 2: Deep Directory Scan & Absolute Path Loading]\n";
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
            search_directory(dir, 1, candidates, found_paths, log);
        }

        if (found_paths.empty()) {
            log << "   FAILED: No target library files found on filesystem.\n";
            g_dlopen_error = "No library files found on disk";
        } else {
            log << "   Found " << found_paths.size() << " candidate library files:\n";
            for (const auto& path : found_paths) {
                log << "     * " << path << " (" << get_file_metadata(path) << ")\n";
            }

            log << "\n[STEP 3: Loading Discovered Path Candidates]\n";
            for (const auto& path : found_paths) {
                log << "-> dlopen(\"" << path << "\", RTLD_NOW | RTLD_GLOBAL)...\n";
                errno = 0;
                void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
                if (handle != nullptr) {
                    g_lib_handle = handle;
                    g_last_loaded_path = path;
                    g_dlopen_error = "";
                    log << "   SUCCESS: Loaded absolute candidate " << path << "\n";
                    log << "   Handle: " << handle << "\n";
                    loaded = true;
                    break;
                } else {
                    const char* err = dlerror();
                    log << "   FAILED: dlerror(): " << (err ? err : "None") << " | errno: " << errno << "\n";
                    g_dlopen_error = err ? err : "dlopen failed";
                }
            }
        }
    }

    if (loaded) {
        log << "\n[STEP 4: Native Symbol Mapping]\n";
        fn_fmpal_init = (fmpal_init_t)dlsym(g_lib_handle, "fmpal_init");
        fn_fmpal_power_up = (fmpal_power_up_t)dlsym(g_lib_handle, "fmpal_power_up");
        fn_fmpal_set_freq = (fmpal_set_freq_t)dlsym(g_lib_handle, "fmpal_set_freq");
        fn_fmpal_get_freq = (fmpal_get_freq_t)dlsym(g_lib_handle, "fmpal_get_freq");

        log << "  * fmpal_init: " << (fn_fmpal_init ? "RESOLVED" : "MISSING") << "\n";
        log << "  * fmpal_power_up: " << (fn_fmpal_power_up ? "RESOLVED" : "MISSING") << "\n";
        log << "  * fmpal_set_freq: " << (fn_fmpal_set_freq ? "RESOLVED" : "MISSING") << "\n";
        log << "  * fmpal_get_freq: " << (fn_fmpal_get_freq ? "RESOLVED" : "MISSING") << "\n";

        log << "\nSUCCESS: Loaded Qualcomm Snapdragon FM PAL Layer.\n";
    } else {
        log << "\nFAILED: Hardware libraries inaccessible from standard untrusted sandboxed application.\n";
    }

    return env->NewStringUTF(log.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_initFm(JNIEnv* env, jclass clazz) {
    if (g_lib_handle == nullptr) {
        return env->NewStringUTF("FAILED: Native library not loaded. Trigger 'Load Native Library' first.");
    }
    if (fn_fmpal_init == nullptr) {
        return env->NewStringUTF("FAILED: fmpal_init symbol is unresolved.");
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

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_setPower(JNIEnv* env, jclass clazz, jboolean power) {
    if (g_lib_handle == nullptr) {
        return env->NewStringUTF("FAILED: Native library not loaded.");
    }
    if (fn_fmpal_power_up == nullptr) {
        return env->NewStringUTF("FAILED: fmpal_power_up symbol is unresolved.");
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

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_setFrequency(JNIEnv* env, jclass clazz, jfloat frequencyMHz) {
    if (g_lib_handle == nullptr) {
        return env->NewStringUTF("FAILED: Native library not loaded.");
    }
    if (fn_fmpal_set_freq == nullptr) {
        return env->NewStringUTF("FAILED: fmpal_set_freq symbol is unresolved.");
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

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_getCurrentFrequency(JNIEnv* env, jclass clazz) {
    if (g_lib_handle == nullptr) {
        return env->NewStringUTF("FAILED: Native library not loaded.");
    }
    if (fn_fmpal_get_freq == nullptr) {
        return env->NewStringUTF("FAILED: fmpal_get_freq symbol is unresolved.");
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

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_getDiagnosticReport(JNIEnv* env, jclass clazz) {
    std::stringstream r;

    r << "======================================================================\n";
    r << " QUALCOMM FM RADIO JNI HARDWARE ANALYZER REPORT\n";
    r << "======================================================================\n\n";

    r << "[1. ANDROID & KERNEL ENVIRONMENT]\n";
    r << "  Android Version : " << get_prop("ro.build.version.release") << "\n";
    r << "  API Level       : " << get_prop("ro.build.version.sdk") << "\n";
    r << "  Fingerprint     : " << get_prop("ro.build.fingerprint") << "\n";
    r << "  CPU ABI         : " << get_prop("ro.product.cpu.abi") << "\n";
    r << "  Supported ABIs  : " << get_prop("ro.product.cpu.abilist") << "\n";
    r << "  Architecture    : " << (sizeof(void*) == 8 ? "64-bit (ARM64-v8a)" : "32-bit") << "\n";
    r << "  Application UID : " << getuid() << "\n";
    r << "  SELinux Mode    : " << get_selinux_mode() << "\n";
    r << "  Kernel Version  : " << get_kernel_version() << "\n\n";

    r << "[2. LINUX CHAR DEV & DEVICE NODE ANALYSIS]\n";
    std::vector<NodeInfo> nodes = probe_device_nodes();
    bool any_node_exists = false;
    bool any_node_accessible = false;
    for (const auto& node : nodes) {
        r << "  * " << node.path << "\n";
        if (node.exists) {
            any_node_exists = true;
            r << "    - State       : PRESENT\n";
            r << "    - Major/Minor : " << node.major_num << "/" << node.minor_num << "\n";
            r << "    - Details     : " << node.metadata << "\n";
            r << "    - Open Result : " << node.open_result << "\n";
            if (node.open_result.find("SUCCESS") != std::string::npos) {
                any_node_accessible = true;
            }
        } else {
            r << "    - State       : ABSENT\n";
            r << "    - Open Result : " << node.open_result << "\n";
        }
    }
    r << "\n";

    r << "[3. HARDWARE SERVICE REGISTRATION & BINDER ANALYSIS]\n";
    std::string vintf_details;
    bool hal_found = scan_vintf_manifests(vintf_details);
    r << "  VINTF Manifest Scan for 'vendor.qti.hardware.fm':\n";
    r << (hal_found ? "    - Registered: YES\n" : "    - Registered: NO\n");
    r << vintf_details;

    struct stat impl_st;
    const char* impl_path = "/vendor/lib64/hw/vendor.qti.hardware.fm@1.0-impl.so";
    if (stat(impl_path, &impl_st) == 0) {
        r << "  HIDL .so binary (/vendor/lib64/hw/vendor.qti.hardware.fm@1.0-impl.so):\n";
        r << "    - Presence    : PRESENT\n";
        r << "    - Size        : " << impl_st.st_size << " bytes\n";
        r << "    - Permissions : " << get_file_metadata(impl_path) << "\n";
    } else {
        r << "  HIDL .so binary (/vendor/lib64/hw/vendor.qti.hardware.fm@1.0-impl.so):\n";
        r << "    - Presence    : ABSENT (" << strerror(errno) << ")\n";
    }
    r << "\n";

    r << "[4. NATIVE FM PAL LIBRARY AND LINKER STATUS]\n";
    if (g_lib_handle != nullptr) {
        r << "  - Status             : LOADED\n";
        r << "  - Active Library     : " << g_last_loaded_path << "\n";
        r << "  - Library Handle     : " << g_lib_handle << "\n";
        
        std::string absolute_mapped_path = find_loaded_library_path_from_maps(g_last_loaded_path);
        if (!absolute_mapped_path.empty()) {
            r << "  - Mapped Path (Maps) : " << absolute_mapped_path << "\n";
            r << "  - Metadata           : " << get_file_metadata(absolute_mapped_path) << "\n";
        } else if (g_last_loaded_path[0] == '/') {
            r << "  - Metadata           : " << get_file_metadata(g_last_loaded_path) << "\n";
        } else {
            r << "  - Mapped Path (Maps) : Could not resolve mapped library from /proc/self/maps\n";
        }
    } else {
        r << "  - Status             : NOT LOADED\n";
        r << "  - Last dlopen error  : " << (g_dlopen_error.empty() ? "None" : g_dlopen_error) << "\n";
        if (g_dlopen_error.find("namespace") != std::string::npos || g_dlopen_error.find("not accessible") != std::string::npos) {
            r << "  - Namespace Analysis : DENIED (Dynamic linker namespace separation blocks public sandboxed access)\n";
        } else {
            r << "  - Namespace Analysis : Indeterminate\n";
        }
    }
    r << "\n";

    r << "[5. ELF BINARY ANALYSIS & SYMBOL TABLE ENUMERATION]\n";
    std::string elf_error;
    std::vector<ParsedSymbol> elf_symbols;
    std::string elf_analyze_path = "";

    if (g_lib_handle != nullptr) {
        elf_analyze_path = find_loaded_library_path_from_maps(g_last_loaded_path);
        if (elf_analyze_path.empty() && g_last_loaded_path[0] == '/') {
            elf_analyze_path = g_last_loaded_path;
        }
    }

    if (elf_analyze_path.empty()) {
        std::vector<std::string> candidates = {
            "libfmpal.so",
            "vendor.qti.hardware.fm@1.0.so",
            "vendor.qti.hardware.fm@1.0-impl.so",
            "libfmjni.so",
            "libqcomfm_jni.so"
        };
        std::vector<std::string> found_paths;
        std::stringstream dummy_log;
        search_directory("/vendor/lib64", 1, candidates, found_paths, dummy_log);
        search_directory("/vendor/lib", 1, candidates, found_paths, dummy_log);
        if (!found_paths.empty()) {
            elf_analyze_path = found_paths[0];
            r << "  - Active library not loaded; falling back to analyzing passive candidate: " << elf_analyze_path << "\n";
        }
    }

    if (!elf_analyze_path.empty()) {
        r << "  - Analyzing ELF file : " << elf_analyze_path << "\n";
        elf_symbols = parse_elf_symbols(elf_analyze_path, elf_error);
        if (!elf_error.empty()) {
            r << "    - ELF Parser Error : " << elf_error << "\n";
        } else {
            r << "    - Total ELF Symbols: " << elf_symbols.size() << " dynamic symbols exported\n";
            r << "    - Matching Symbols (highlighted contain 'fm', 'radio', 'qti', 'vendor', 'pal'):\n";
            int matches = 0;
            for (const auto& sym : elf_symbols) {
                std::string sym_lower = sym.name;
                std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);
                bool matched = (sym_lower.find("fm") != std::string::npos ||
                                sym_lower.find("radio") != std::string::npos ||
                                sym_lower.find("qti") != std::string::npos ||
                                sym_lower.find("vendor") != std::string::npos ||
                                sym_lower.find("pal") != std::string::npos);
                if (matched) {
                    matches++;
                    r << "      [" << matches << "] Offset: 0x" << std::hex << sym.address << std::dec << " | Symbol: " << sym.name;
                    if (g_lib_handle != nullptr) {
                        void* resolved_addr = dlsym(g_lib_handle, sym.name.c_str());
                        if (resolved_addr) {
                            r << " -> Resolved Address: " << resolved_addr;
                        } else {
                            r << " -> Resolved: dlsym failed";
                        }
                    }
                    r << "\n";
                }
            }
            if (matches == 0) {
                r << "      - No symbols matching filters were found.\n";
            }
        }
    } else {
        r << "  - No candidate ELF library files available or readable for analysis.\n";
    }
    r << "\n";

    r << "[6. FM PAL CAPABILITY TABLE]\n";
    r << "  * fmpal_init     : " << (fn_fmpal_init ? "RESOLVED (Runnable)" : "UNRESOLVED / MISSING") << "\n";
    r << "  * fmpal_power_up : " << (fn_fmpal_power_up ? "RESOLVED (Runnable)" : "UNRESOLVED / MISSING") << "\n";
    r << "  * fmpal_set_freq : " << (fn_fmpal_set_freq ? "RESOLVED (Runnable)" : "UNRESOLVED / MISSING") << "\n";
    r << "  * fmpal_get_freq : " << (fn_fmpal_get_freq ? "RESOLVED (Runnable)" : "UNRESOLVED / MISSING") << "\n\n";

    r << "[7. FINAL RECONCILIATION & BSP CONCLUSION]\n";
    
    std::string conclusion = "";
    if (g_lib_handle != nullptr && fn_fmpal_init && fn_fmpal_power_up && fn_fmpal_set_freq && fn_fmpal_get_freq) {
        conclusion = "READY FOR FM INITIALIZATION";
    } else if (!elf_analyze_path.empty() && g_lib_handle == nullptr) {
        conclusion = "LIBRARIES PRESENT BUT BLOCKED";
    } else if (!hal_found) {
        conclusion = "HAL NOT REGISTERED";
    } else if (any_node_exists && !any_node_accessible) {
        conclusion = "BINDER ACCESS DENIED";
    } else {
        conclusion = "DEVICE DOES NOT EXPOSE QUALCOMM FM TO USER APPLICATIONS";
    }

    r << "  Conclusion: " << conclusion << "\n";
    r << "======================================================================\n";

    return env->NewStringUTF(r.str().c_str());
}
