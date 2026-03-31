/*
 * Boxedwine Android - Android ELF Dynamic Linker
 *
 * Loads ARMv7 ELF shared libraries (.so) from an APK into emulated memory,
 * resolves symbols, performs relocations, and calls JNI_OnLoad.
 *
 * Supports:
 *   - ELF32 shared objects (ET_DYN) targeting ARM (EM_ARM = 40)
 *   - REL relocations: R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT,
 *                      R_ARM_RELATIVE, R_ARM_CALL, R_ARM_JUMP24
 *   - Symbol resolution across loaded libraries + a JNI shim table
 *
 * Based on ideas from:
 *   referenceCode/apkenv/linker/linker.c  (Android Bionic linker)
 *   referenceCode/Bridge/BridgeLib/linker.cpp
 */

#ifndef __ANDROID_LINKER_H__
#define __ANDROID_LINKER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of simultaneously loaded libraries */
#define ANDROID_LINKER_MAX_LIBS 64

/* -------------------------------------------------------------------------
 * ELF32 types (matching ARM ABI)
 * ---------------------------------------------------------------------- */
typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

#pragma pack(push, 1)
typedef struct {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

typedef struct {
    Elf32_Word d_tag;
    Elf32_Word d_val; /* also d_ptr */
} Elf32_Dyn;
#pragma pack(pop)

/* ELF constants */
#define ET_DYN      3
#define EM_ARM      40
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_REL     9
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_PLTREL   20
#define DT_JMPREL   23
#define DT_INIT     12
#define DT_FINI     13
#define DT_SONAME   14

#define ELF32_R_SYM(i)   ((i) >> 8)
#define ELF32_R_TYPE(i)  ((i) & 0xff)
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)

/* ARM relocation types */
#define R_ARM_NONE      0
#define R_ARM_ABS32     2
#define R_ARM_CALL      28
#define R_ARM_JUMP24    29
#define R_ARM_THM_CALL  10
#define R_ARM_GLOB_DAT  21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE  23

/* -------------------------------------------------------------------------
 * Loaded library descriptor
 * ---------------------------------------------------------------------- */
typedef struct LoadedLib {
    char      name[256];

    /* Load base in emulated address space */
    uint32_t  load_base;
    uint32_t  load_size;

    /* Pointer into emulated memory at load_base */
    uint8_t  *mem;

    /* Dynamic section parsed fields */
    Elf32_Sym  *symtab;
    const char *strtab;
    uint32_t    symtab_count;

    Elf32_Rel  *rel;
    uint32_t    rel_count;
    Elf32_Rel  *plt_rel;
    uint32_t    plt_rel_count;

    /* Entry points in emulated VA */
    uint32_t   init_va;   /* DT_INIT  */
    uint32_t   fini_va;   /* DT_FINI  */
    uint32_t   entry_va;  /* e_entry  */

    /* Whether JNI_OnLoad has been called */
    bool       jni_loaded;

    /* Source ELF bytes (kept alive for symbol lookup) */
    uint8_t   *elf_data;
    size_t     elf_size;
} LoadedLib;

/* -------------------------------------------------------------------------
 * Symbol override entry (JNI / bionic shim)
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *name;
    uint32_t    emulated_va; /* Virtual address of the stub in emulated space */
} SymbolOverride;

/* -------------------------------------------------------------------------
 * Linker context
 * ---------------------------------------------------------------------- */
typedef struct {
    LoadedLib    libs[ANDROID_LINKER_MAX_LIBS];
    unsigned     lib_count;

    /* Base address for the next library to be mapped */
    uint32_t     next_load_base;

    /* Emulated memory: flat byte array */
    uint8_t     *mem;
    uint32_t     mem_size;

    /* Symbol override table (for bionic / JNI stubs) */
    SymbolOverride *overrides;
    unsigned        override_count;
    unsigned        override_capacity;
} LinkerContext;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * Initialise a linker context.
 * @param ctx       Context to initialise.
 * @param mem       Pointer to the start of emulated address space.
 * @param mem_size  Total size of emulated address space in bytes.
 * @param load_base Virtual address where libraries begin to be mapped.
 */
void android_linker_init(LinkerContext *ctx, uint8_t *mem, uint32_t mem_size, uint32_t load_base);

/**
 * Register a symbol override (e.g. a bionic/JNI stub).
 */
void android_linker_add_override(LinkerContext *ctx, const char *name, uint32_t emulated_va);

/**
 * Load an ELF shared library image into emulated memory.
 * @param ctx      Linker context.
 * @param name     Library name (e.g. "libmain.so").
 * @param elf_data ELF image bytes.
 * @param elf_size Size in bytes.
 * @return Index into ctx->libs[], or -1 on failure.
 */
int android_linker_load(LinkerContext *ctx, const char *name, const uint8_t *elf_data, size_t elf_size);

/**
 * Resolve all pending relocations in all loaded libraries.
 */
bool android_linker_relocate_all(LinkerContext *ctx);

/**
 * Look up an exported symbol by name across all loaded libraries.
 * @return Virtual address of the symbol, or 0 if not found.
 */
uint32_t android_linker_lookup(const LinkerContext *ctx, const char *name);

/**
 * Free all resources held by the linker context.
 */
void android_linker_destroy(LinkerContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID_LINKER_H__ */
