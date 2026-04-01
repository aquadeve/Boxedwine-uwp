/*
 * Boxedwine Android - Android ELF Dynamic Linker Implementation
 *
 * Loads ARMv7 (ELF32) and AArch64 (ELF64) .so files into a flat emulated
 * memory space, performs load-time relocations, and resolves inter-library symbols.
 *
 * Based on:
 *   Android Bionic linker (referenceCode/apkenv/linker/linker.c)
 *   ARM ELF ABI specification
 */

#include "android_linker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Debug logging macro — active in debug builds */
#if defined(_DEBUG) || !defined(NDEBUG)
#define LNK_LOG_DEBUG(fmt, ...) fprintf(stderr, "[LNK DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define LNK_LOG_DEBUG(fmt, ...) ((void)0)
#endif

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static inline uint32_t align_up(uint32_t v, uint32_t align) {
    return (v + align - 1) & ~(align - 1);
}

static inline const Elf32_Ehdr *elf_hdr(const uint8_t *data) {
    return (const Elf32_Ehdr*)data;
}

static inline const Elf64_Ehdr *elf64_hdr(const uint8_t *data) {
    return (const Elf64_Ehdr*)data;
}

static inline const Elf32_Phdr *elf_phdr(const uint8_t *data, unsigned idx) {
    const Elf32_Ehdr *e = elf_hdr(data);
    return (const Elf32_Phdr*)(data + e->e_phoff + idx * e->e_phentsize);
}

static inline const Elf64_Phdr *elf64_phdr(const uint8_t *data, unsigned idx) {
    const Elf64_Ehdr *e = elf64_hdr(data);
    return (const Elf64_Phdr*)(data + e->e_phoff + idx * e->e_phentsize);
}

static inline const Elf32_Shdr *elf_shdr(const uint8_t *data, unsigned idx) {
    const Elf32_Ehdr *e = elf_hdr(data);
    return (const Elf32_Shdr*)(data + e->e_shoff + idx * e->e_shentsize);
}

/* Check whether an ELF image is 64-bit */
static inline bool is_elf64(const uint8_t *data) {
    return data[4] == ELFCLASS64;
}

/* -------------------------------------------------------------------------
 * android_linker_init
 * ---------------------------------------------------------------------- */

void android_linker_init(LinkerContext *ctx, uint8_t *mem, uint32_t mem_size, uint32_t load_base) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->mem            = mem;
    ctx->mem_size       = mem_size;
    ctx->next_load_base = load_base;
}

/* -------------------------------------------------------------------------
 * android_linker_add_override
 * ---------------------------------------------------------------------- */

void android_linker_add_override(LinkerContext *ctx, const char *name, uint32_t va) {
    if (ctx->override_count >= ctx->override_capacity) {
        unsigned new_cap = ctx->override_capacity ? ctx->override_capacity * 2 : 64;
        ctx->overrides = (SymbolOverride*)realloc(ctx->overrides, new_cap * sizeof(SymbolOverride));
        ctx->override_capacity = new_cap;
    }
    ctx->overrides[ctx->override_count].name        = name;
    ctx->overrides[ctx->override_count].emulated_va = va;
    ctx->override_count++;
}

/* -------------------------------------------------------------------------
 * android_linker_load
 * ---------------------------------------------------------------------- */

int android_linker_load(LinkerContext *ctx, const char *name,
                        const uint8_t *elf_data, size_t elf_size) {
    LNK_LOG_DEBUG("load: '%s' (%zu bytes), lib_count=%u", name, elf_size, ctx->lib_count);
    if (ctx->lib_count >= ANDROID_LINKER_MAX_LIBS) {
        LNK_LOG_DEBUG("load: FAILED — max libs (%d) reached", ANDROID_LINKER_MAX_LIBS);
        return -1;
    }
    if (elf_size < 16) {
        LNK_LOG_DEBUG("load: FAILED — elf_size %zu too small", elf_size);
        return -1;
    }

    /* Validate ELF magic */
    if (elf_data[0] != 0x7F || elf_data[1] != 'E' ||
        elf_data[2] != 'L'  || elf_data[3] != 'F') {
        fprintf(stderr, "android_linker: %s: bad ELF magic\n", name);
        LNK_LOG_DEBUG("load: FAILED — bad ELF magic (0x%02X%02X%02X%02X)",
                      elf_data[0], elf_data[1], elf_data[2], elf_data[3]);
        return -1;
    }

    bool elf64 = is_elf64(elf_data);
    LNK_LOG_DEBUG("load: '%s' is %s ELF", name, elf64 ? "ELF64" : "ELF32");

    /* Validate machine type */
    if (elf64) {
        if (elf_size < sizeof(Elf64_Ehdr)) return -1;
        const Elf64_Ehdr *ehdr = elf64_hdr(elf_data);
        if (ehdr->e_machine != EM_AARCH64) {
            fprintf(stderr, "android_linker: %s: not an AArch64 ELF64 (e_machine=%u)\n", name, ehdr->e_machine);
            return -1;
        }
    } else {
        if (elf_size < sizeof(Elf32_Ehdr)) return -1;
        const Elf32_Ehdr *ehdr = elf_hdr(elf_data);
        if (ehdr->e_machine != EM_ARM) {
            fprintf(stderr, "android_linker: %s: not an ARM ELF32 (e_machine=%u)\n", name, ehdr->e_machine);
            return -1;
        }
    }

    /* Calculate total virtual size by inspecting PT_LOAD segments */
    uint32_t min_va = UINT32_MAX, max_va = 0;
    if (elf64) {
        const Elf64_Ehdr *ehdr = elf64_hdr(elf_data);
        for (unsigned i = 0; i < ehdr->e_phnum; i++) {
            const Elf64_Phdr *ph = elf64_phdr(elf_data, i);
            if (ph->p_type != PT_LOAD) continue;
            uint32_t va_lo = (uint32_t)ph->p_vaddr;
            uint32_t seg_end = va_lo + (uint32_t)ph->p_memsz;
            if (va_lo < min_va) min_va = va_lo;
            if (seg_end > max_va) max_va = seg_end;
        }
    } else {
        const Elf32_Ehdr *ehdr = elf_hdr(elf_data);
        for (unsigned i = 0; i < ehdr->e_phnum; i++) {
            const Elf32_Phdr *ph = elf_phdr(elf_data, i);
            if (ph->p_type != PT_LOAD) continue;
            uint32_t seg_end = ph->p_vaddr + ph->p_memsz;
            if (ph->p_vaddr < min_va) min_va = ph->p_vaddr;
            if (seg_end > max_va) max_va = seg_end;
        }
    }
    if (min_va == UINT32_MAX) min_va = 0;
    uint32_t total_size = align_up(max_va - min_va, 4096);

    /* Allocate virtual address range */
    uint32_t load_base = ctx->next_load_base;
    if (load_base + total_size > ctx->mem_size) {
        fprintf(stderr, "android_linker: out of emulated memory for %s\n", name);
        return -1;
    }
    ctx->next_load_base = align_up(load_base + total_size, 4096);

    /* Zero the region */
    uint8_t *seg_mem = ctx->mem + load_base;
    memset(seg_mem, 0, total_size);

    /* Copy PT_LOAD segments */
    if (elf64) {
        const Elf64_Ehdr *ehdr = elf64_hdr(elf_data);
        for (unsigned i = 0; i < ehdr->e_phnum; i++) {
            const Elf64_Phdr *ph = elf64_phdr(elf_data, i);
            if (ph->p_type != PT_LOAD) continue;
            uint32_t dest_off = (uint32_t)ph->p_vaddr - min_va;
            if (ph->p_offset + ph->p_filesz > elf_size) {
                fprintf(stderr, "android_linker: %s: segment out of bounds\n", name);
                return -1;
            }
            memcpy(seg_mem + dest_off, elf_data + ph->p_offset, (size_t)ph->p_filesz);
        }
    } else {
        const Elf32_Ehdr *ehdr = elf_hdr(elf_data);
        for (unsigned i = 0; i < ehdr->e_phnum; i++) {
            const Elf32_Phdr *ph = elf_phdr(elf_data, i);
            if (ph->p_type != PT_LOAD) continue;
            uint32_t dest_off = ph->p_vaddr - min_va;
            if (ph->p_offset + ph->p_filesz > elf_size) {
                fprintf(stderr, "android_linker: %s: segment out of bounds\n", name);
                return -1;
            }
            memcpy(seg_mem + dest_off, elf_data + ph->p_offset, ph->p_filesz);
        }
    }

    /* Fill in LoadedLib */
    LoadedLib *lib = &ctx->libs[ctx->lib_count];
    memset(lib, 0, sizeof(*lib));
    snprintf(lib->name, sizeof(lib->name), "%s", name);
    lib->load_base = load_base;
    lib->load_size = total_size;
    lib->mem       = seg_mem;
    lib->is_elf64  = elf64;
    lib->elf_data  = (uint8_t*)malloc(elf_size); /* keep a copy for reloc */
    if (!lib->elf_data) return -1;
    memcpy(lib->elf_data, elf_data, elf_size);
    lib->elf_size = elf_size;

    /* Parse PT_DYNAMIC segment */
    if (elf64) {
        const Elf64_Ehdr *ehdr = elf64_hdr(elf_data);
        for (unsigned i = 0; i < ehdr->e_phnum; i++) {
            const Elf64_Phdr *ph = elf64_phdr(elf_data, i);
            if (ph->p_type != PT_DYNAMIC) continue;

            const Elf64_Dyn *dyn = (const Elf64_Dyn*)(elf_data + ph->p_offset);
            uint64_t strtab_off = 0, symtab_off = 0;
            uint64_t rela_off = 0, rela_sz = 0, plt_off = 0, plt_sz = 0;
            uint32_t sym_count = 0;

            for (; dyn->d_tag != DT_NULL; dyn++) {
                switch ((uint32_t)dyn->d_tag) {
                    case DT_STRTAB:   strtab_off = dyn->d_val - min_va; break;
                    case DT_SYMTAB:   symtab_off = dyn->d_val - min_va; break;
                    case DT_RELA:     rela_off   = dyn->d_val - min_va; break;
                    case DT_RELASZ:   rela_sz    = dyn->d_val; break;
                    case DT_JMPREL:   plt_off    = dyn->d_val - min_va; break;
                    case DT_PLTRELSZ: plt_sz     = dyn->d_val; break;
                    case DT_HASH: {
                        const uint32_t *hash = (const uint32_t*)(seg_mem + (uint32_t)(dyn->d_val - min_va));
                        sym_count = hash[1];
                        break;
                    }
                    case DT_INIT: lib->init_va = load_base + (uint32_t)(dyn->d_val - min_va); break;
                    case DT_FINI: lib->fini_va = load_base + (uint32_t)(dyn->d_val - min_va); break;
                }
            }

            if (strtab_off) lib->strtab = (const char*)(seg_mem + (uint32_t)strtab_off);
            if (symtab_off) {
                lib->symtab64       = (Elf64_Sym*)(seg_mem + (uint32_t)symtab_off);
                lib->symtab64_count = sym_count;
            }
            if (rela_off && rela_sz) {
                lib->rela       = (Elf64_Rela*)(seg_mem + (uint32_t)rela_off);
                lib->rela_count = (uint32_t)(rela_sz / sizeof(Elf64_Rela));
            }
            if (plt_off && plt_sz) {
                lib->plt_rela       = (Elf64_Rela*)(seg_mem + (uint32_t)plt_off);
                lib->plt_rela_count = (uint32_t)(plt_sz / sizeof(Elf64_Rela));
            }
            break;
        }

        if (ehdr->e_entry) lib->entry_va = load_base + (uint32_t)(ehdr->e_entry - min_va);
    } else {
        const Elf32_Ehdr *ehdr = elf_hdr(elf_data);
        for (unsigned i = 0; i < ehdr->e_phnum; i++) {
            const Elf32_Phdr *ph = elf_phdr(elf_data, i);
            if (ph->p_type != PT_DYNAMIC) continue;

            const Elf32_Dyn *dyn = (const Elf32_Dyn*)(elf_data + ph->p_offset);
            uint32_t strtab_off = 0, symtab_off = 0;
            uint32_t rel_off = 0, rel_sz = 0, plt_off = 0, plt_sz = 0;
            uint32_t sym_count = 0;

            for (; dyn->d_tag != DT_NULL; dyn++) {
                switch (dyn->d_tag) {
                    case DT_STRTAB:  strtab_off = dyn->d_val - min_va; break;
                    case DT_SYMTAB:  symtab_off = dyn->d_val - min_va; break;
                    case DT_REL:     rel_off    = dyn->d_val - min_va; break;
                    case DT_RELSZ:   rel_sz     = dyn->d_val; break;
                    case DT_JMPREL:  plt_off    = dyn->d_val - min_va; break;
                    case DT_PLTRELSZ:plt_sz     = dyn->d_val; break;
                    case DT_HASH: {
                        const uint32_t *hash = (const uint32_t*)(seg_mem + (dyn->d_val - min_va));
                        sym_count = hash[1];
                        break;
                    }
                    case DT_INIT: lib->init_va = load_base + (dyn->d_val - min_va); break;
                    case DT_FINI: lib->fini_va = load_base + (dyn->d_val - min_va); break;
                }
            }

            if (strtab_off) lib->strtab = (const char*)(seg_mem + strtab_off);
            if (symtab_off) {
                lib->symtab       = (Elf32_Sym*)(seg_mem + symtab_off);
                lib->symtab_count = sym_count;
            }
            if (rel_off && rel_sz) {
                lib->rel       = (Elf32_Rel*)(seg_mem + rel_off);
                lib->rel_count = rel_sz / sizeof(Elf32_Rel);
            }
            if (plt_off && plt_sz) {
                lib->plt_rel       = (Elf32_Rel*)(seg_mem + plt_off);
                lib->plt_rel_count = plt_sz / sizeof(Elf32_Rel);
            }
            break;
        }

        if (ehdr->e_entry) lib->entry_va = load_base + (ehdr->e_entry - min_va);
    }

    LNK_LOG_DEBUG("load: '%s' loaded at VA 0x%08X, size=0x%X, entry=0x%08X",
                  name, load_base, total_size, lib->entry_va);
    return (int)ctx->lib_count++;
}

/* -------------------------------------------------------------------------
 * Symbol lookup
 * ---------------------------------------------------------------------- */

uint32_t android_linker_lookup(const LinkerContext *ctx, const char *name) {
    /* Check overrides first */
    for (unsigned i = 0; i < ctx->override_count; i++) {
        if (strcmp(ctx->overrides[i].name, name) == 0)
            return ctx->overrides[i].emulated_va;
    }

    /* Search all loaded libraries */
    for (unsigned li = 0; li < ctx->lib_count; li++) {
        const LoadedLib *lib = &ctx->libs[li];
        if (!lib->strtab) continue;

        if (lib->is_elf64) {
            if (!lib->symtab64) continue;
            for (unsigned si = 0; si < lib->symtab64_count; si++) {
                const Elf64_Sym *sym = &lib->symtab64[si];
                if (ELF64_ST_BIND(sym->st_info) == 0) continue; /* STB_LOCAL */
                if (sym->st_shndx == 0) continue; /* SHN_UNDEF */
                const char *sym_name = lib->strtab + sym->st_name;
                if (strcmp(sym_name, name) == 0) {
                    return lib->load_base + (uint32_t)sym->st_value;
                }
            }
        } else {
            if (!lib->symtab) continue;
            for (unsigned si = 0; si < lib->symtab_count; si++) {
                const Elf32_Sym *sym = &lib->symtab[si];
                if (ELF32_ST_BIND(sym->st_info) == 0) continue;
                if (sym->st_shndx == 0) continue;
                const char *sym_name = lib->strtab + sym->st_name;
                if (strcmp(sym_name, name) == 0) {
                    return lib->load_base + sym->st_value;
                }
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * android_linker_relocate_all
 * ---------------------------------------------------------------------- */

static bool apply_relocations(LinkerContext *ctx, LoadedLib *lib,
                               Elf32_Rel *rel, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t offset = rel[i].r_offset;
        uint32_t type   = ELF32_R_TYPE(rel[i].r_info);
        uint32_t sym_idx= ELF32_R_SYM (rel[i].r_info);

        /* Compute the target VA in the library */
        uint32_t target_va;
        {
            const Elf32_Ehdr *e = elf_hdr(lib->elf_data);
            uint32_t min_va = UINT32_MAX;
            for (unsigned pi = 0; pi < e->e_phnum; pi++) {
                const Elf32_Phdr *ph = elf_phdr(lib->elf_data, pi);
                if (ph->p_type == PT_LOAD && ph->p_vaddr < min_va)
                    min_va = ph->p_vaddr;
            }
            if (min_va == UINT32_MAX) min_va = 0;
            target_va = lib->load_base + (offset - min_va);
        }

        /* Get symbol address if needed */
        uint32_t sym_va = 0;
        if (sym_idx && lib->symtab && lib->strtab) {
            const Elf32_Sym *sym = &lib->symtab[sym_idx];
            const char *sym_name = lib->strtab + sym->st_name;
            if (sym->st_shndx != 0) {
                sym_va = lib->load_base + sym->st_value;
            } else {
                sym_va = android_linker_lookup(ctx, sym_name);
                if (!sym_va && type != R_ARM_RELATIVE) {
                    fprintf(stderr, "android_linker: unresolved symbol '%s'\n", sym_name);
                }
            }
        }

        uint8_t *patch = ctx->mem + target_va;
        uint32_t word;
        memcpy(&word, patch, 4);

        switch (type) {
            case R_ARM_ABS32:
                word += sym_va;
                break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT:
                word = sym_va;
                break;
            case R_ARM_RELATIVE:
                word += lib->load_base;
                break;
            case R_ARM_CALL:
            case R_ARM_JUMP24: {
                int32_t branch_off = (int32_t)(sym_va - (target_va + 8));
                word = (word & 0xFF000000u) | ((branch_off >> 2) & 0x00FFFFFFu);
                break;
            }
            case R_ARM_NONE:
                break;
            default:
                fprintf(stderr, "android_linker: unhandled reloc type %u\n", type);
                break;
        }

        memcpy(patch, &word, 4);
    }
    return true;
}

static bool apply_relocations64(LinkerContext *ctx, LoadedLib *lib,
                                 Elf64_Rela *rela, uint32_t count) {
    const Elf64_Ehdr *e = elf64_hdr(lib->elf_data);
    uint64_t min_va64 = UINT64_MAX;
    for (unsigned pi = 0; pi < e->e_phnum; pi++) {
        const Elf64_Phdr *ph = elf64_phdr(lib->elf_data, pi);
        if (ph->p_type == PT_LOAD && ph->p_vaddr < min_va64)
            min_va64 = ph->p_vaddr;
    }
    if (min_va64 == UINT64_MAX) min_va64 = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t offset = rela[i].r_offset;
        uint32_t type    = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        int64_t  addend  = rela[i].r_addend;

        uint32_t target_va = lib->load_base + (uint32_t)(offset - min_va64);

        /* Get symbol address if needed */
        uint32_t sym_va = 0;
        if (sym_idx && lib->symtab64 && lib->strtab) {
            const Elf64_Sym *sym = &lib->symtab64[sym_idx];
            const char *sym_name = lib->strtab + sym->st_name;
            if (sym->st_shndx != 0) {
                sym_va = lib->load_base + (uint32_t)sym->st_value;
            } else {
                sym_va = android_linker_lookup(ctx, sym_name);
                if (!sym_va && type != R_AARCH64_RELATIVE) {
                    fprintf(stderr, "android_linker: unresolved symbol '%s'\n", sym_name);
                }
            }
        }

        uint8_t *patch = ctx->mem + target_va;

        switch (type) {
            case R_AARCH64_ABS64: {
                uint64_t val;
                memcpy(&val, patch, 8);
                val = sym_va + addend;
                memcpy(patch, &val, 8);
                break;
            }
            case R_AARCH64_ABS32: {
                uint32_t val;
                memcpy(&val, patch, 4);
                val = sym_va + (uint32_t)addend;
                memcpy(patch, &val, 4);
                break;
            }
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT: {
                uint64_t val = sym_va + addend;
                memcpy(patch, &val, 8);
                break;
            }
            case R_AARCH64_RELATIVE: {
                uint64_t val = lib->load_base + addend;
                memcpy(patch, &val, 8);
                break;
            }
            case R_AARCH64_NONE:
                break;
            default:
                fprintf(stderr, "android_linker: unhandled AArch64 reloc type %u\n", type);
                break;
        }
    }
    return true;
}

bool android_linker_relocate_all(LinkerContext *ctx) {
    bool ok = true;
    LNK_LOG_DEBUG("relocate_all: processing %u libraries", ctx->lib_count);
    for (unsigned i = 0; i < ctx->lib_count; i++) {
        LoadedLib *lib = &ctx->libs[i];
        LNK_LOG_DEBUG("relocate_all: lib[%u] '%s' (elf64=%d)", i, lib->name, lib->is_elf64);

        if (lib->is_elf64) {
            if (lib->rela && lib->rela_count)
                ok = apply_relocations64(ctx, lib, lib->rela, lib->rela_count) && ok;
            if (lib->plt_rela && lib->plt_rela_count)
                ok = apply_relocations64(ctx, lib, lib->plt_rela, lib->plt_rela_count) && ok;
        } else {
            if (lib->rel && lib->rel_count)
                ok = apply_relocations(ctx, lib, lib->rel, lib->rel_count) && ok;
            if (lib->plt_rel && lib->plt_rel_count)
                ok = apply_relocations(ctx, lib, lib->plt_rel, lib->plt_rel_count) && ok;
        }
    }
    LNK_LOG_DEBUG("relocate_all: finished, result=%s", ok ? "OK" : "FAILED");
    return ok;
}

/* -------------------------------------------------------------------------
 * android_linker_destroy
 * ---------------------------------------------------------------------- */

void android_linker_destroy(LinkerContext *ctx) {
    for (unsigned i = 0; i < ctx->lib_count; i++) {
        free(ctx->libs[i].elf_data);
    }
    free(ctx->overrides);
    memset(ctx, 0, sizeof(*ctx));
}
