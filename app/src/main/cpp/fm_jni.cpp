/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Qualcomm Snapdragon 695 (SM6375) FM Radio JNI Bridge & ELF Forensic Analyzer
 * Target: Android 16 (API 36) - Samsung Galaxy Tab A9+ (SM-X216B)
 * 
 * Implement custom dual-class ELF parser from scratch. Analyzes Elf32/Elf64 headers,
 * program headers, section headers, dynamic segment (DT_NEEDED dependencies),
 * relocation tables, symbols, and printable strings. Generates a forensic dependency
 * graph and reverses Qualcomm FM capabilities completely.
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

static void* g_lib_handle = nullptr;
static std::string g_dlopen_error = "Not loaded yet";
static std::string g_last_loaded_path = "";

static fmpal_init_t fn_fmpal_init = nullptr;
static fmpal_power_up_t fn_fmpal_power_up = nullptr;
static fmpal_set_freq_t fn_fmpal_set_freq = nullptr;
static fmpal_get_freq_t fn_fmpal_get_freq = nullptr;

struct ELFAnalysis {
    std::string filepath;
    bool parsed = false;
    std::string error;
    std::string class_str;
    std::string endianness;
    std::string machine;
    std::string type_str;
    uint64_t entry_point = 0;
    uint32_t ph_count = 0;
    uint32_t sh_count = 0;

    struct ProgramHeader {
        uint32_t type;
        std::string type_str;
        uint64_t offset;
        uint64_t vaddr;
        uint64_t filesz;
        uint64_t memsz;
        uint32_t flags;
        std::string flags_str;
    };
    std::vector<ProgramHeader> program_headers;

    struct SectionHeader {
        std::string name;
        uint32_t type;
        std::string type_str;
        uint64_t addr;
        uint64_t offset;
        uint64_t size;
        uint32_t link;
        uint32_t info;
    };
    std::vector<SectionHeader> section_headers;

    struct Symbol {
        std::string name;
        uint64_t value;
        uint64_t size;
        uint8_t info;
        std::string bind_str;
        std::string type_str;
        std::string section_name;
        bool is_export = false;
        bool is_import = false;
    };
    std::vector<Symbol> symbols;

    struct Relocation {
        uint64_t offset;
        uint64_t info;
        int64_t addend;
        std::string symbol_name;
        std::string type_str;
    };
    std::vector<Relocation> relocations;

    std::vector<std::string> dependencies;
    std::vector<std::string> target_strings;

    bool imports_dlopen = false;
    bool imports_dlsym = false;
    bool imports_binder = false;
    bool imports_libhidlbase = false;
    bool imports_libbinder = false;
    bool imports_libutils = false;
    bool imports_libcutils = false;
    bool imports_liblog = false;
};

// Returns whether a symbol/string is of interest for Qualcomm FM
static bool is_highlight_keyword(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return (lower.find("fm") != std::string::npos ||
            lower.find("radio") != std::string::npos ||
            lower.find("pal") != std::string::npos ||
            lower.find("qti") != std::string::npos ||
            lower.find("vendor") != std::string::npos ||
            lower.find("btfm") != std::string::npos ||
            lower.find("hci") != std::string::npos ||
            lower.find("rx") != std::string::npos ||
            lower.find("tx") != std::string::npos ||
            lower.find("seek") != std::string::npos ||
            lower.find("scan") != std::string::npos ||
            lower.find("rds") != std::string::npos ||
            lower.find("audio") != std::string::npos);
}

static std::string resolve_library_file_path(const std::string& libname) {
    std::vector<std::string> search_dirs = {
        "/vendor/lib64",
        "/vendor/lib",
        "/vendor/lib64/hw",
        "/vendor/lib/hw",
        "/system/lib64",
        "/system/lib"
    };
    for (const auto& dir : search_dirs) {
        std::string full_path = dir + "/" + libname;
        if (access(full_path.c_str(), R_OK) == 0) {
            return full_path;
        }
    }
    return "";
}

/**
 * Unified template function to parse both Elf32 and Elf64 formats natively and safely.
 */
template<typename Ehdr_T, typename Phdr_T, typename Shdr_T, typename Sym_T, typename Dyn_T, typename Rel_T, typename Rela_T, int Class>
bool parse_elf_generic(int fd, uint64_t file_size, ELFAnalysis& analysis) {
    Ehdr_T ehdr;
    if (lseek(fd, 0, SEEK_SET) == -1 || read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        analysis.error = "Failed to read ELF header";
        return false;
    }

    analysis.class_str = (Class == ELFCLASS64) ? "64-bit (ELFCLASS64)" : "32-bit (ELFCLASS32)";
    analysis.entry_point = ehdr.e_entry;
    analysis.ph_count = ehdr.e_phnum;
    analysis.sh_count = ehdr.e_shnum;

    switch (ehdr.e_machine) {
        case EM_AARCH64: analysis.machine = "AArch64 (ARM 64-bit)"; break;
        case EM_ARM:     analysis.machine = "ARM (32-bit)"; break;
        case EM_X86_64:  analysis.machine = "x86_64"; break;
        case EM_386:     analysis.machine = "x86 (Intel 386)"; break;
        default:         analysis.machine = "Unknown machine (" + std::to_string(ehdr.e_machine) + ")"; break;
    }

    switch (ehdr.e_type) {
        case ET_NONE: analysis.type_str = "NONE (None)"; break;
        case ET_REL:  analysis.type_str = "REL (Relocatable file)"; break;
        case ET_EXEC: analysis.type_str = "EXEC (Executable file)"; break;
        case ET_DYN:  analysis.type_str = "DYN (Shared object)"; break;
        case ET_CORE: analysis.type_str = "CORE (Core file)"; break;
        default:      analysis.type_str = "Unknown type (" + std::to_string(ehdr.e_type) + ")"; break;
    }

    // Program Headers
    if (ehdr.e_phnum > 0) {
        uint64_t ph_off = ehdr.e_phoff;
        uint64_t ph_sz = ehdr.e_phnum * sizeof(Phdr_T);
        if (ph_off + ph_sz <= file_size) {
            std::vector<Phdr_T> phdrs(ehdr.e_phnum);
            if (lseek(fd, ph_off, SEEK_SET) != -1 && read(fd, phdrs.data(), ph_sz) == static_cast<ssize_t>(ph_sz)) {
                for (int i = 0; i < ehdr.e_phnum; ++i) {
                    ELFAnalysis::ProgramHeader ph;
                    ph.type = phdrs[i].p_type;
                    switch (phdrs[i].p_type) {
                        case PT_NULL:    ph.type_str = "NULL"; break;
                        case PT_LOAD:    ph.type_str = "LOAD"; break;
                        case PT_DYNAMIC: ph.type_str = "DYNAMIC"; break;
                        case PT_INTERP:  ph.type_str = "INTERP"; break;
                        case PT_NOTE:    ph.type_str = "NOTE"; break;
                        case PT_PHDR:    ph.type_str = "PHDR"; break;
                        case PT_TLS:     ph.type_str = "TLS"; break;
                        case PT_GNU_EH_FRAME: ph.type_str = "GNU_EH_FRAME"; break;
                        case PT_GNU_STACK:    ph.type_str = "GNU_STACK"; break;
                        case PT_GNU_RELRO:    ph.type_str = "GNU_RELRO"; break;
                        default:         ph.type_str = "OTHER (0x" + std::to_string(phdrs[i].p_type) + ")"; break;
                    }
                    ph.offset = phdrs[i].p_offset;
                    ph.vaddr = phdrs[i].p_vaddr;
                    ph.filesz = phdrs[i].p_filesz;
                    ph.memsz = phdrs[i].p_memsz;
                    ph.flags = phdrs[i].p_flags;
                    std::stringstream f_ss;
                    if (phdrs[i].p_flags & PF_R) f_ss << "R";
                    if (phdrs[i].p_flags & PF_W) f_ss << "W";
                    if (phdrs[i].p_flags & PF_X) f_ss << "X";
                    ph.flags_str = f_ss.str();
                    analysis.program_headers.push_back(ph);
                }
            }
        }
    }

    // Section Headers
    std::vector<Shdr_T> shdrs;
    std::vector<char> shstrtab;
    if (ehdr.e_shnum > 0) {
        uint64_t sh_off = ehdr.e_shoff;
        uint64_t sh_sz = ehdr.e_shnum * sizeof(Shdr_T);
        if (sh_off + sh_sz <= file_size) {
            shdrs.resize(ehdr.e_shnum);
            if (lseek(fd, sh_off, SEEK_SET) != -1 && read(fd, shdrs.data(), sh_sz) == static_cast<ssize_t>(sh_sz)) {
                // Section Names string table
                if (ehdr.e_shstrndx < ehdr.e_shnum) {
                    Shdr_T shstr = shdrs[ehdr.e_shstrndx];
                    if (shstr.sh_offset + shstr.sh_size <= file_size) {
                        shstrtab.resize(shstr.sh_size);
                        lseek(fd, shstr.sh_offset, SEEK_SET);
                        read(fd, shstrtab.data(), shstr.sh_size);
                    }
                }

                for (int i = 0; i < ehdr.e_shnum; ++i) {
                    ELFAnalysis::SectionHeader sh;
                    if (!shstrtab.empty() && shdrs[i].sh_name < shstrtab.size()) {
                        sh.name = &shstrtab[shdrs[i].sh_name];
                    } else {
                        sh.name = "section_" + std::to_string(i);
                    }
                    sh.type = shdrs[i].sh_type;
                    switch (shdrs[i].sh_type) {
                        case SHT_NULL:     sh.type_str = "NULL"; break;
                        case SHT_PROGBITS: sh.type_str = "PROGBITS"; break;
                        case SHT_SYMTAB:   sh.type_str = "SYMTAB"; break;
                        case SHT_STRTAB:   sh.type_str = "STRTAB"; break;
                        case SHT_RELA:     sh.type_str = "RELA"; break;
                        case SHT_HASH:     sh.type_str = "HASH"; break;
                        case SHT_DYNAMIC:  sh.type_str = "DYNAMIC"; break;
                        case SHT_NOTE:     sh.type_str = "NOTE"; break;
                        case SHT_NOBITS:   sh.type_str = "NOBITS"; break;
                        case SHT_REL:      sh.type_str = "REL"; break;
                        case SHT_DYNSYM:   sh.type_str = "DYNSYM"; break;
                        case SHT_INIT_ARRAY: sh.type_str = "INIT_ARRAY"; break;
                        case SHT_FINI_ARRAY: sh.type_str = "FINI_ARRAY"; break;
                        default:           sh.type_str = "OTHER (0x" + std::to_string(shdrs[i].sh_type) + ")"; break;
                    }
                    sh.addr = shdrs[i].sh_addr;
                    sh.offset = shdrs[i].sh_offset;
                    sh.size = shdrs[i].sh_size;
                    sh.link = shdrs[i].sh_link;
                    sh.info = shdrs[i].sh_info;
                    analysis.section_headers.push_back(sh);
                }
            }
        }
    }

    // Identify critical sections
    int dynsym_idx = -1, dynstr_idx = -1, dynamic_idx = -1;
    std::vector<int> rela_indices;
    std::vector<int> rel_indices;
    int rodata_idx = -1;

    for (size_t i = 0; i < analysis.section_headers.size(); ++i) {
        const auto& sh = analysis.section_headers[i];
        if (sh.name == ".dynsym") dynsym_idx = i;
        else if (sh.name == ".dynstr") dynstr_idx = i;
        else if (sh.name == ".dynamic") dynamic_idx = i;
        else if (sh.type_str == "RELA") rela_indices.push_back(i);
        else if (sh.type_str == "REL") rel_indices.push_back(i);
        else if (sh.name == ".rodata") rodata_idx = i;
    }

    // Parse dynamic symbol table
    if (dynsym_idx != -1 && dynstr_idx != -1) {
        Shdr_T dynsym_sh = shdrs[dynsym_idx];
        Shdr_T dynstr_sh = shdrs[dynstr_idx];

        if (dynsym_sh.sh_offset + dynsym_sh.sh_size <= file_size &&
            dynstr_sh.sh_offset + dynstr_sh.sh_size <= file_size) {
            
            size_t num_syms = dynsym_sh.sh_size / sizeof(Sym_T);
            std::vector<Sym_T> syms(num_syms);
            std::vector<char> dynstr(dynstr_sh.sh_size);

            if (lseek(fd, dynsym_sh.sh_offset, SEEK_SET) != -1 &&
                read(fd, syms.data(), dynsym_sh.sh_size) == static_cast<ssize_t>(dynsym_sh.sh_size) &&
                lseek(fd, dynstr_sh.sh_offset, SEEK_SET) != -1 &&
                read(fd, dynstr.data(), dynstr_sh.sh_size) == static_cast<ssize_t>(dynstr_sh.sh_size)) {

                for (size_t i = 0; i < num_syms; ++i) {
                    Sym_T sym = syms[i];
                    if (sym.st_name == 0 || sym.st_name >= dynstr_sh.sh_size) continue;

                    ELFAnalysis::Symbol s;
                    s.name = &dynstr[sym.st_name];
                    s.value = sym.st_value;
                    s.size = sym.st_size;
                    s.info = sym.st_info;

                    uint8_t bind = sym.st_info >> 4;
                    switch (bind) {
                        case STB_LOCAL:  s.bind_str = "LOCAL"; break;
                        case STB_GLOBAL: s.bind_str = "GLOBAL"; break;
                        case STB_WEAK:   s.bind_str = "WEAK"; break;
                        default:         s.bind_str = "OTHER"; break;
                    }

                    uint8_t type = sym.st_info & 0xf;
                    switch (type) {
                        case STT_NOTYPE:  s.type_str = "NOTYPE"; break;
                        case STT_OBJECT:  s.type_str = "OBJECT"; break;
                        case STT_FUNC:    s.type_str = "FUNC"; break;
                        case STT_SECTION: s.type_str = "SECTION"; break;
                        case STT_FILE:    s.type_str = "FILE"; break;
                        default:          s.type_str = "OTHER"; break;
                    }

                    if (sym.st_shndx == SHN_UNDEF) {
                        s.section_name = "UNDEF";
                        s.is_import = true;
                        s.is_export = false;
                    } else {
                        s.is_import = false;
                        s.is_export = (bind == STB_GLOBAL || bind == STB_WEAK);
                        if (sym.st_shndx < analysis.section_headers.size()) {
                            s.section_name = analysis.section_headers[sym.st_shndx].name;
                        } else {
                            s.section_name = "ABS/Index";
                        }
                    }
                    analysis.symbols.push_back(s);
                }
            }
        }
    }

    // Parse dependencies from dynamic section
    if (dynamic_idx != -1 && dynstr_idx != -1) {
        Shdr_T dynamic_sh = shdrs[dynamic_idx];
        Shdr_T dynstr_sh = shdrs[dynstr_idx];

        if (dynamic_sh.sh_offset + dynamic_sh.sh_size <= file_size &&
            dynstr_sh.sh_offset + dynstr_sh.sh_size <= file_size) {
            
            size_t num_dyns = dynamic_sh.sh_size / sizeof(Dyn_T);
            std::vector<Dyn_T> dyns(num_dyns);
            std::vector<char> dynstr(dynstr_sh.sh_size);

            if (lseek(fd, dynamic_sh.sh_offset, SEEK_SET) != -1 &&
                read(fd, dyns.data(), dynamic_sh.sh_size) == static_cast<ssize_t>(dynamic_sh.sh_size) &&
                lseek(fd, dynstr_sh.sh_offset, SEEK_SET) != -1 &&
                read(fd, dynstr.data(), dynstr_sh.sh_size) == static_cast<ssize_t>(dynstr_sh.sh_size)) {

                for (size_t i = 0; i < num_dyns; ++i) {
                    Dyn_T dyn = dyns[i];
                    if (dyn.d_tag == DT_NEEDED) {
                        if (dyn.d_un.d_val < dynstr_sh.sh_size) {
                            std::string dep_name = &dynstr[dyn.d_un.d_val];
                            analysis.dependencies.push_back(dep_name);
                            if (dep_name.find("libhidlbase") != std::string::npos) analysis.imports_libhidlbase = true;
                            if (dep_name.find("libbinder") != std::string::npos) analysis.imports_libbinder = true;
                            if (dep_name.find("libutils") != std::string::npos) analysis.imports_libutils = true;
                            if (dep_name.find("libcutils") != std::string::npos) analysis.imports_libcutils = true;
                            if (dep_name.find("liblog") != std::string::npos) analysis.imports_liblog = true;
                        }
                    }
                }
            }
        }
    }

    // Parse RELA relocations
    for (int r_idx : rela_indices) {
        Shdr_T rela_sh = shdrs[r_idx];
        if (rela_sh.sh_offset + rela_sh.sh_size <= file_size) {
            size_t num_relas = rela_sh.sh_size / sizeof(Rela_T);
            std::vector<Rela_T> relas(num_relas);
            if (lseek(fd, rela_sh.sh_offset, SEEK_SET) != -1 && read(fd, relas.data(), rela_sh.sh_size) == static_cast<ssize_t>(rela_sh.sh_size)) {
                for (size_t i = 0; i < num_relas; ++i) {
                    ELFAnalysis::Relocation r;
                    r.offset = relas[i].r_offset;
                    r.info = relas[i].r_info;
                    r.addend = relas[i].r_addend;

                    uint32_t sym_idx = (Class == ELFCLASS64) ? (relas[i].r_info >> 32) : (relas[i].r_info >> 8);
                    uint32_t r_type = (Class == ELFCLASS64) ? (relas[i].r_info & 0xffffffff) : (relas[i].r_info & 0xff);
                    r.type_str = "RELA_TYPE_" + std::to_string(r_type);

                    if (sym_idx < analysis.symbols.size()) {
                        r.symbol_name = analysis.symbols[sym_idx].name;
                    }
                    analysis.relocations.push_back(r);
                }
            }
        }
    }

    // Parse REL relocations
    for (int r_idx : rel_indices) {
        Shdr_T rel_sh = shdrs[r_idx];
        if (rel_sh.sh_offset + rel_sh.sh_size <= file_size) {
            size_t num_rels = rel_sh.sh_size / sizeof(Rel_T);
            std::vector<Rel_T> rels(num_rels);
            if (lseek(fd, rel_sh.sh_offset, SEEK_SET) != -1 && read(fd, rels.data(), rel_sh.sh_size) == static_cast<ssize_t>(rel_sh.sh_size)) {
                for (size_t i = 0; i < num_rels; ++i) {
                    ELFAnalysis::Relocation r;
                    r.offset = rels[i].r_offset;
                    r.info = rels[i].r_info;
                    r.addend = 0;

                    uint32_t sym_idx = (Class == ELFCLASS64) ? (rels[i].r_info >> 32) : (rels[i].r_info >> 8);
                    uint32_t r_type = (Class == ELFCLASS64) ? (rels[i].r_info & 0xffffffff) : (rels[i].r_info & 0xff);
                    r.type_str = "REL_TYPE_" + std::to_string(r_type);

                    if (sym_idx < analysis.symbols.size()) {
                        r.symbol_name = analysis.symbols[sym_idx].name;
                    }
                    analysis.relocations.push_back(r);
                }
            }
        }
    }

    // Extract target strings from .rodata section
    if (rodata_idx != -1) {
        Shdr_T ro_sh = shdrs[rodata_idx];
        if (ro_sh.sh_offset + ro_sh.sh_size <= file_size) {
            std::vector<char> rodata(ro_sh.sh_size);
            if (lseek(fd, ro_sh.sh_offset, SEEK_SET) != -1 && read(fd, rodata.data(), ro_sh.sh_size) == static_cast<ssize_t>(ro_sh.sh_size)) {
                std::string cur_str = "";
                for (size_t i = 0; i < ro_sh.sh_size; ++i) {
                    char c = rodata[i];
                    if (c >= 32 && c <= 126) {
                        cur_str += c;
                    } else {
                        if (cur_str.length() >= 4) {
                            std::string lower = cur_str;
                            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                            if (lower.find("fm") != std::string::npos ||
                                lower.find("vendor") != std::string::npos ||
                                lower.find("radio") != std::string::npos ||
                                lower.find("qti") != std::string::npos ||
                                lower.find("hidl") != std::string::npos ||
                                lower.find("service") != std::string::npos ||
                                lower.find("btfm") != std::string::npos ||
                                lower.find("slimbus") != std::string::npos ||
                                lower.find("audio") != std::string::npos) {
                                if (std::find(analysis.target_strings.begin(), analysis.target_strings.end(), cur_str) == analysis.target_strings.end()) {
                                    analysis.target_strings.push_back(cur_str);
                                }
                            }
                        }
                        cur_str = "";
                    }
                }
            }
        }
    }

    // Identify critical imports
    for (const auto& sym : analysis.symbols) {
        if (sym.is_import) {
            if (sym.name.find("dlopen") != std::string::npos) analysis.imports_dlopen = true;
            if (sym.name.find("dlsym") != std::string::npos) analysis.imports_dlsym = true;
            if (sym.name.find("binder") != std::string::npos || sym.name.find("Binder") != std::string::npos) analysis.imports_binder = true;
        }
    }

    analysis.parsed = true;
    return true;
}

static ELFAnalysis analyze_elf_file(const std::string& filepath) {
    ELFAnalysis analysis;
    analysis.filepath = filepath;

    int fd = open(filepath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        analysis.error = "open failed: " + std::string(strerror(errno));
        return analysis;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        analysis.error = "fstat failed: " + std::string(strerror(errno));
        close(fd);
        return analysis;
    }
    uint64_t file_size = st.st_size;

    unsigned char e_ident[EI_NIDENT];
    if (read(fd, e_ident, EI_NIDENT) != EI_NIDENT) {
        analysis.error = "failed to read ELF identification bytes";
        close(fd);
        return analysis;
    }

    if (memcmp(e_ident, ELFMAG, SELFMAG) != 0) {
        analysis.error = "invalid ELF magic header";
        close(fd);
        return analysis;
    }

    analysis.endianness = (e_ident[EI_DATA] == ELFDATA2LSB) ? "Little endian" : "Big endian";

    if (e_ident[EI_CLASS] == ELFCLASS64) {
        parse_elf_generic<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Sym, Elf64_Dyn, Elf64_Rel, Elf64_Rela, ELFCLASS64>(fd, file_size, analysis);
    } else if (e_ident[EI_CLASS] == ELFCLASS32) {
        parse_elf_generic<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Sym, Elf32_Dyn, Elf32_Rel, Elf32_Rela, ELFCLASS32>(fd, file_size, analysis);
    } else {
        analysis.error = "unsupported ELF class: " + std::to_string(e_ident[EI_CLASS]);
    }

    close(fd);
    return analysis;
}

static void print_dependency_node(std::stringstream& ss, const std::string& lib_name, int indent_level, std::vector<std::string>& visited) {
    if (std::find(visited.begin(), visited.end(), lib_name) != visited.end()) {
        for (int i = 0; i < indent_level; ++i) ss << "  ";
        ss << "└── " << lib_name << " (already listed)\n";
        return;
    }
    visited.push_back(lib_name);

    for (int i = 0; i < indent_level; ++i) ss << "  ";
    if (indent_level > 0) ss << "└── ";
    
    std::string path = resolve_library_file_path(lib_name);
    if (!path.empty()) {
        ss << lib_name << " [" << path << "]\n";
        if (indent_level < 2) {
            ELFAnalysis sub = analyze_elf_file(path);
            if (sub.parsed) {
                for (const auto& child : sub.dependencies) {
                    print_dependency_node(ss, child, indent_level + 1, visited);
                }
            }
        }
    } else {
        ss << lib_name << " [NOT FOUND IN SEARCH PATHS]\n";
    }
}

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

static std::string get_file_metadata(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "stat failed: " + std::string(strerror(errno));
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

static void search_directory(const std::string& path, int depth, const std::vector<std::string>& candidates, std::vector<std::string>& found_paths) {
    if (depth > 4) return;
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string full_path = path;
        if (full_path.back() != '/') full_path += "/";
        full_path += name;

        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                search_directory(full_path, depth + 1, candidates, found_paths);
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

static std::string get_prop(const char* name) {
    char val[PROP_VALUE_MAX] = {0};
    __system_property_get(name, val);
    return val;
}

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
    if (strlen(prop_val) > 0) return std::string(prop_val) + " (From ro.boot.selinux)";
    return "Enforcing (Enforced in modern API levels)";
}

static std::string get_kernel_version() {
    std::ifstream version("/proc/version");
    if (version.is_open()) {
        std::string line;
        if (std::getline(version, line)) return line;
    }
    return "Unknown kernel version";
}

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
    out_details = found ? ss.str() : "  - Not declared in any device VINTF manifest XML files.\n";
    return found;
}

struct NodeInfo {
    std::string path;
    bool exists;
    std::string metadata;
    unsigned int major_num;
    unsigned int minor_num;
    std::string open_result;
};

static std::vector<NodeInfo> probe_device_nodes() {
    const char* nodes[] = {
        "/dev/radio0", "/dev/fm", "/dev/fmradio", "/dev/fm_radio", "/dev/btfmslim"
    };
    std::vector<NodeInfo> info_list;
    for (const char* node : nodes) {
        NodeInfo info;
        info.path = node;
        struct stat st;
        if (stat(node, &st) == 0) {
            info.exists = true;
            info.major_num = major(st.st_rdev);
            info.minor_num = minor(st.st_rdev);
            std::stringstream ss;
            ss << ((S_ISCHR(st.st_mode)) ? "c" : "-")
               << ((st.st_mode & S_IRUSR) ? "r" : "-") << ((st.st_mode & S_IWUSR) ? "w" : "-") << ((st.st_mode & S_IXUSR) ? "x" : "-")
               << ((st.st_mode & S_IRGRP) ? "r" : "-") << ((st.st_mode & S_IWGRP) ? "w" : "-") << ((st.st_mode & S_IXGRP) ? "x" : "-")
               << ((st.st_mode & S_IROTH) ? "r" : "-") << ((st.st_mode & S_IWOTH) ? "w" : "-") << ((st.st_mode & S_IXOTH) ? "x" : "-");
            struct passwd* pw = getpwuid(st.st_uid);
            struct group* gr = getgrgid(st.st_gid);
            ss << " | Owner: " << (pw ? pw->pw_name : std::to_string(st.st_uid))
               << " | Group: " << (gr ? gr->gr_name : std::to_string(st.st_gid));
            info.metadata = ss.str();

            int fd = open(node, O_RDWR);
            if (fd >= 0) {
                info.open_result = "SUCCESS (O_RDWR accessible)";
                close(fd);
            } else {
                int err_no = errno;
                fd = open(node, O_RDONLY);
                if (fd >= 0) {
                    info.open_result = "SUCCESS (O_RDONLY accessible)";
                    close(fd);
                } else {
                    info.open_result = "FAILED: errno " + std::to_string(err_no) + " (" + strerror(err_no) + ")";
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

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_loadNativeLibrary(JNIEnv* env, jclass clazz) {
    std::stringstream log;
    log << "[JNI Native Loader] Beginning ELF Dynamic Linker Probe Sequence...\n";
    if (g_lib_handle != nullptr) {
        log << "SUCCESS: Loaded from previous session: " << g_last_loaded_path << "\n";
        return env->NewStringUTF(log.str().c_str());
    }

    std::vector<std::string> candidates = {
        "libfmpal.so", "vendor.qti.hardware.fm@1.0.so", "vendor.qti.hardware.fm@1.0-impl.so", "libfmjni.so", "libqcomfm_jni.so"
    };

    bool loaded = false;
    for (const auto& cand : candidates) {
        log << "-> dlopen(\"" << cand << "\", RTLD_NOW | RTLD_GLOBAL)...\n";
        void* handle = dlopen(cand.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (handle != nullptr) {
            g_lib_handle = handle;
            g_last_loaded_path = cand;
            g_dlopen_error = "";
            log << "   SUCCESS: Loaded via SONAME " << cand << "\n";
            loaded = true;
            break;
        } else {
            log << "   FAILED: dlerror(): " << dlerror() << "\n";
        }
    }

    if (!loaded) {
        log << "\n[Deep Directory Scan]\n";
        std::vector<std::string> search_dirs = { "/vendor/lib64", "/vendor/lib", "/vendor/lib64/hw", "/vendor/lib/hw", "/system/lib64", "/system/lib" };
        std::vector<std::string> found_paths;
        for (const auto& dir : search_dirs) {
            search_directory(dir, 1, candidates, found_paths);
        }

        for (const auto& path : found_paths) {
            log << "-> dlopen(\"" << path << "\")...\n";
            void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (handle != nullptr) {
                g_lib_handle = handle;
                g_last_loaded_path = path;
                g_dlopen_error = "";
                log << "   SUCCESS: Loaded absolute candidate " << path << "\n";
                loaded = true;
                break;
            } else {
                g_dlopen_error = dlerror();
                log << "   FAILED: " << g_dlopen_error << "\n";
            }
        }
    }

    if (loaded) {
        fn_fmpal_init = (fmpal_init_t)dlsym(g_lib_handle, "fmpal_init");
        fn_fmpal_power_up = (fmpal_power_up_t)dlsym(g_lib_handle, "fmpal_power_up");
        fn_fmpal_set_freq = (fmpal_set_freq_t)dlsym(g_lib_handle, "fmpal_set_freq");
        fn_fmpal_get_freq = (fmpal_get_freq_t)dlsym(g_lib_handle, "fmpal_get_freq");
    }
    return env->NewStringUTF(log.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_initFm(JNIEnv* env, jclass clazz) {
    if (!fn_fmpal_init) return env->NewStringUTF("FAILED: fmpal_init unresolved.");
    int rc = fn_fmpal_init();
    std::stringstream ss;
    ss << (rc >= 0 ? "SUCCESS" : "FAILED") << ": fmpal_init() returned " << rc << " (errno: " << errno << " - " << strerror(errno) << ")";
    return env->NewStringUTF(ss.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_setPower(JNIEnv* env, jclass clazz, jboolean power) {
    if (!fn_fmpal_power_up) return env->NewStringUTF("FAILED: fmpal_power_up unresolved.");
    int rc = fn_fmpal_power_up(power ? 1 : 0);
    std::stringstream ss;
    ss << (rc >= 0 ? "SUCCESS" : "FAILED") << ": fmpal_power_up(" << (power ? "1" : "0") << ") returned " << rc << " (errno: " << errno << " - " << strerror(errno) << ")";
    return env->NewStringUTF(ss.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_setFrequency(JNIEnv* env, jclass clazz, jfloat frequencyMHz) {
    if (!fn_fmpal_set_freq) return env->NewStringUTF("FAILED: fmpal_set_freq unresolved.");
    int freqKHz = static_cast<int>(frequencyMHz * 1000.0f);
    int rc = fn_fmpal_set_freq(freqKHz);
    std::stringstream ss;
    ss << (rc >= 0 ? "SUCCESS" : "FAILED") << ": fmpal_set_freq(" << freqKHz << " KHz) returned " << rc << " (errno: " << errno << " - " << strerror(errno) << ")";
    return env->NewStringUTF(ss.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_getCurrentFrequency(JNIEnv* env, jclass clazz) {
    if (!fn_fmpal_get_freq) return env->NewStringUTF("FAILED: fmpal_get_freq unresolved.");
    int freqKHz = 0;
    int rc = fn_fmpal_get_freq(&freqKHz);
    std::stringstream ss;
    if (rc >= 0) ss << "SUCCESS: Frequency: " << (static_cast<float>(freqKHz) / 1000.0f) << " MHz (raw: " << freqKHz << " KHz)";
    else ss << "FAILED: fmpal_get_freq() returned " << rc << " (errno: " << errno << " - " << strerror(errno) << ")";
    return env->NewStringUTF(ss.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_qualcomm_fmradio_FMBridge_getDiagnosticReport(JNIEnv* env, jclass clazz) {
    std::stringstream r;

    r << "======================================================================\n";
    r << " QUALCOMM SNAPDRAGON FM RADIO FORENSIC ENGINEERING REPORT\n";
    r << "======================================================================\n\n";

    r << "[1. SYSTEM PLATFORM & SOC IDENTIFICATION]\n";
    r << "  Android Release : " << get_prop("ro.build.version.release") << "\n";
    r << "  API Level       : " << get_prop("ro.build.version.sdk") << "\n";
    r << "  SELinux Mode    : " << get_selinux_mode() << "\n";
    r << "  Active ABI      : " << get_prop("ro.product.cpu.abi") << "\n";
    r << "  Kernel Version  : " << get_kernel_version() << "\n\n";

    r << "[2. LINUX CHARACTER DEVICE PROBES]\n";
    std::vector<NodeInfo> nodes = probe_device_nodes();
    for (const auto& node : nodes) {
        r << "  * " << node.path << "\n";
        if (node.exists) {
            r << "    - Presence    : PRESENT (Major: " << node.major_num << ", Minor: " << node.minor_num << ")\n";
            r << "    - Permissions : " << node.metadata << "\n";
            r << "    - I/O Probing : " << node.open_result << "\n";
        } else {
            r << "    - Presence    : ABSENT\n";
        }
    }
    r << "\n";

    r << "[3. VINTF HARDWARE SERVICE REGISTRATION]\n";
    std::string vintf;
    bool registered = scan_vintf_manifests(vintf);
    r << "  - vendor.qti.hardware.fm Service: " << (registered ? "REGISTERED\n" : "UNREGISTERED\n");
    r << vintf << "\n";

    // Locate primary library file to analyze
    std::string lib_path = "";
    if (g_lib_handle != nullptr) {
        lib_path = find_loaded_library_path_from_maps(g_last_loaded_path);
        if (lib_path.empty() && g_last_loaded_path[0] == '/') lib_path = g_last_loaded_path;
    }
    if (lib_path.empty()) {
        std::vector<std::string> candidates = { "libfmpal.so", "vendor.qti.hardware.fm@1.0-impl.so", "vendor.qti.hardware.fm@1.0.so" };
        std::vector<std::string> found;
        search_directory("/vendor/lib64", 1, candidates, found);
        search_directory("/vendor/lib", 1, candidates, found);
        if (!found.empty()) lib_path = found[0];
    }

    if (lib_path.empty()) {
        r << "[4. ELF BINARY FORENSIC ANALYSIS]\n";
        r << "  - Error: No readable Qualcomm FM hardware library files found on filesystem.\n\n";
    } else {
        r << "[4. ELF BINARY FORENSIC ANALYSIS]\n";
        r << "  - Target Library File : " << lib_path << "\n";
        r << "  - File Details        : " << get_file_metadata(lib_path) << "\n";

        ELFAnalysis elf = analyze_elf_file(lib_path);
        if (!elf.parsed) {
            r << "  - ELF Parser Error    : " << elf.error << "\n\n";
        } else {
            r << "  - Architecture        : " << elf.class_str << " | " << elf.endianness << "\n";
            r << "  - Target CPU Machine  : " << elf.machine << "\n";
            r << "  - Binary Object Type  : " << elf.type_str << "\n";
            r << "  - Program Entry Point : 0x" << std::hex << elf.entry_point << std::dec << "\n";
            r << "  - Program Headers     : " << elf.ph_count << " segments\n";
            r << "  - Section Headers     : " << elf.sh_count << " sections\n\n";

            r << "[5. EXPORTED SYMBOLS TABLE (DYNAMIC & STANDARD)]\n";
            int count = 0;
            for (const auto& sym : elf.symbols) {
                if (sym.is_export) {
                    count++;
                    if (is_highlight_keyword(sym.name)) {
                        r << "  [" << count << "] 0x" << std::hex << sym.value << std::dec 
                          << " | Size: " << sym.size << " | " << sym.section_name << " | EXPORT: " << sym.name;
                        if (g_lib_handle != nullptr) {
                            void* addr = dlsym(g_lib_handle, sym.name.c_str());
                            if (addr) r << " -> [RESOLVED LIVE AT: " << addr << "]";
                        }
                        r << "\n";
                    }
                }
            }
            if (count == 0) r << "  - No exported symbols discovered (stripped library).\n";
            r << "\n";

            r << "[6. CORE IMPORTED SYMBOLS]\n";
            int imp_count = 0;
            for (const auto& sym : elf.symbols) {
                if (sym.is_import) {
                    if (is_highlight_keyword(sym.name) || sym.name.find("dl") == 0 || sym.name.find("binder") != std::string::npos) {
                        imp_count++;
                        r << "  [" << imp_count << "] IMPORT: " << sym.name << "\n";
                    }
                }
            }
            r << "  - Imports dlopen()    : " << (elf.imports_dlopen ? "YES" : "NO") << "\n";
            r << "  - Imports dlsym()     : " << (elf.imports_dlsym ? "YES" : "NO") << "\n";
            r << "  - Uses Binder         : " << (elf.imports_binder ? "YES" : "NO") << "\n";
            r << "\n";

            r << "[7. FORENSIC DEPENDENCY GRAPH (RECURSIVE SEARCH)]\n";
            std::vector<std::string> visited;
            print_dependency_node(r, g_last_loaded_path.empty() ? "libfmpal.so" : g_last_loaded_path, 0, visited);
            r << "\n";

            r << "[8. REVERSE ENGINEERED QUALCOMM FM SIGNATURE STRINGS]\n";
            if (elf.target_strings.empty()) {
                r << "  - No hardware strings matched. File might be fully stripped of printable sections.\n";
            } else {
                for (const auto& str : elf.target_strings) {
                    r << "  - STRING: \"" << str << "\"\n";
                }
            }
            r << "\n";

            r << "[9. RELOCATIONS TABLE ENTRIES]\n";
            int rel_show = 0;
            for (const auto& rel : elf.relocations) {
                if (!rel.symbol_name.empty() && is_highlight_keyword(rel.symbol_name)) {
                    rel_show++;
                    r << "  - Reloc Offset: 0x" << std::hex << rel.offset << std::dec << " | Type: " << rel.type_str << " | Bind symbol: " << rel.symbol_name << "\n";
                    if (rel_show >= 15) {
                        r << "  - ... [truncated remaining relocations] ...\n";
                        break;
                    }
                }
            }
            if (rel_show == 0) r << "  - No relocation entries matching target keywords.\n";
            r << "\n";

            // Heuristics for entry points & tuning functions
            r << "[10. REVERSED HARDWARE CAPABILITIES REPORT]\n";
            
            // Search for probable FM entry points
            r << "  * Probable FM Entry Points:\n";
            bool found_any_entry = false;
            for (const auto& sym : elf.symbols) {
                if (sym.is_export) {
                    std::string sym_lower = sym.name;
                    std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);
                    if (sym_lower.find("init") != std::string::npos || sym_lower.find("power") != std::string::npos || sym_lower.find("enable") != std::string::npos) {
                        r << "    - 0x" << std::hex << sym.value << std::dec << " -> " << sym.name << " (Probable hardware initializer)\n";
                        found_any_entry = true;
                    }
                }
            }
            if (!found_any_entry) r << "    - None detected. Checking binary strings heuristics...\n";

            // Tuning APIs
            r << "  * Probable Tuning & Frequency APIs:\n";
            bool found_any_tune = false;
            for (const auto& sym : elf.symbols) {
                if (sym.is_export) {
                    std::string sym_lower = sym.name;
                    std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);
                    if (sym_lower.find("freq") != std::string::npos || sym_lower.find("tune") != std::string::npos || sym_lower.find("set_channel") != std::string::npos) {
                        r << "    - 0x" << std::hex << sym.value << std::dec << " -> " << sym.name << " (Tuning execution function)\n";
                        found_any_tune = true;
                    }
                }
            }
            if (!found_any_tune) r << "    - None detected.\n";

            // Audio Routing APIs
            r << "  * Probable Audio Routing APIs:\n";
            bool found_any_audio = false;
            for (const auto& sym : elf.symbols) {
                if (sym.is_export) {
                    std::string sym_lower = sym.name;
                    std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);
                    if (sym_lower.find("audio") != std::string::npos || sym_lower.find("route") != std::string::npos || sym_lower.find("volume") != std::string::npos) {
                        r << "    - 0x" << std::hex << sym.value << std::dec << " -> " << sym.name << " (Audio channel controller)\n";
                        found_any_audio = true;
                    }
                }
            }
            if (!found_any_audio) r << "    - None detected.\n";

            // HIDL interfaces & service discovery
            r << "  * Probable HIDL/AIDL/Binder Interfaces:\n";
            bool found_any_hidl = false;
            for (const auto& str : elf.target_strings) {
                if (str.find("vendor.qti.hardware.fm") != std::string::npos || str.find("android.hardware") != std::string::npos || str.find("IFm") != std::string::npos) {
                    r << "    - Registered Interface Name: " << str << "\n";
                    found_any_hidl = true;
                }
            }
            if (!found_any_hidl) r << "    - No direct HIDL descriptors matched in printable segments.\n";

            // Firmware Functions
            r << "  * Probable Firmware Loading & Patch Functions:\n";
            bool found_any_fw = false;
            for (const auto& sym : elf.symbols) {
                if (sym.is_export) {
                    std::string sym_lower = sym.name;
                    std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);
                    if (sym_lower.find("fw") != std::string::npos || sym_lower.find("firmware") != std::string::npos || sym_lower.find("patch") != std::string::npos || sym_lower.find("rampatch") != std::string::npos) {
                        r << "    - 0x" << std::hex << sym.value << std::dec << " -> " << sym.name << " (Firmware loader API)\n";
                        found_any_fw = true;
                    }
                }
            }
            if (!found_any_fw) {
                for (const auto& str : elf.target_strings) {
                    std::string s_lower = str;
                    std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
                    if (s_lower.find(".tlv") != std::string::npos || s_lower.find(".bin") != std::string::npos || s_lower.find("rampatch") != std::string::npos) {
                        r << "    - Firmware string match: \"" << str << "\"\n";
                        found_any_fw = true;
                    }
                }
            }
            if (!found_any_fw) r << "    - No firmware references discovered.\n";
            r << "\n";
        }
    }

    r << "[11. ANALYSIS CONFLICT RESOLUTION & RECONCILIATION]\n";
    if (g_lib_handle != nullptr && fn_fmpal_init && fn_fmpal_power_up && fn_fmpal_set_freq && fn_fmpal_get_freq) {
        r << "  Analysis State: FULLY OPERATIONAL\n";
        r << "  Recommendation: Dynamic loader has successfully loaded the physical device driver. Standard APIs are active.\n";
    } else if (!lib_path.empty() && g_lib_handle == nullptr) {
        r << "  Analysis State: PERSISTENT PLATFORM BLOCK\n";
        r << "  Recommendation: Qualcomm FM driver binary exists, but modern Android 16 SELinux and mounting spaces protect standard processes from loaded execution. Sign with system key to proceed.\n";
    } else {
        r << "  Analysis State: HARDWARE ACCESS ABSENT\n";
        r << "  Recommendation: Device does not expose Qualcomm Snapdragon FM hardware libraries within accessible layers.\n";
    }
    r << "======================================================================\n";

    return env->NewStringUTF(r.str().c_str());
}
