/*
 * elf_writer.c — ELF64 relocatable object file writer
 *
 * Writes a valid ELF64 .o file that can be linked with:
 *   gcc foo.o -o foo
 *   ld -o foo foo.o -lc --dynamic-linker /lib64/ld-linux-x86-64.so.2
 *
 * Sections emitted:
 *   [0]  NULL
 *   [1]  .text       — machine code
 *   [2]  .rodata     — string literals and other read-only data
 *   [3]  .data       — initialized global variables
 *   [4]  .bss        — zero-initialized globals (SHT_NOBITS)
 *   [5]  .symtab     — symbol table
 *   [6]  .strtab     — string table (symbol names)
 *   [7]  .shstrtab   — section name string table
 *   [8]  .rela.text  — relocations for .text
 */

#include "../include/elf.h"
#include "../include/codegen.h"
#include "../include/x86_encode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* =========================================================
 * String table builder
 * ========================================================= */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} StrTab;

static void strtab_init(StrTab *st) {
    st->cap  = 256;
    st->data = malloc(st->cap);
    st->size = 0;
    /* First byte is always NUL (index 0 = empty string) */
    st->data[st->size++] = '\0';
}

static void strtab_free(StrTab *st) {
    free(st->data);
    st->data = NULL;
    st->size = st->cap = 0;
}

static size_t strtab_add(StrTab *st, const char *s) {
    size_t len = s ? strlen(s) : 0;
    if (st->size + len + 1 > st->cap) {
        while (st->cap < st->size + len + 1) st->cap *= 2;
        st->data = realloc(st->data, st->cap);
    }
    size_t off = st->size;
    if (s) memcpy(st->data + st->size, s, len);
    st->data[st->size + len] = '\0';
    st->size += len + 1;
    return off;
}

/* =========================================================
 * ELF writer
 * ========================================================= */

/* Section indices */
#define SEC_NULL      0
#define SEC_TEXT      1
#define SEC_RODATA    2
#define SEC_DATA      3
#define SEC_BSS       4
#define SEC_SYMTAB    5
#define SEC_STRTAB    6
#define SEC_SHSTRTAB  7
#define SEC_RELA_TEXT 8
#define NSECTIONS     9

static void write_u8(FILE *f, uint8_t v)  { fwrite(&v, 1, 1, f); }
static void write_u16(FILE *f, uint16_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v>>8)};
    fwrite(b, 1, 2, f);
}
static void write_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24)};
    fwrite(b, 1, 4, f);
}
static void write_u64(FILE *f, uint64_t v) {
    write_u32(f, (uint32_t)v);
    write_u32(f, (uint32_t)(v>>32));
}
static void write_i64(FILE *f, int64_t v) { write_u64(f, (uint64_t)v); }

static void write_elf64_ehdr(FILE *f, Elf64_Ehdr *h) {
    fwrite(h->e_ident, 1, EI_NIDENT, f);
    write_u16(f, h->e_type);
    write_u16(f, h->e_machine);
    write_u32(f, h->e_version);
    write_u64(f, h->e_entry);
    write_u64(f, h->e_phoff);
    write_u64(f, h->e_shoff);
    write_u32(f, h->e_flags);
    write_u16(f, h->e_ehsize);
    write_u16(f, h->e_phentsize);
    write_u16(f, h->e_phnum);
    write_u16(f, h->e_shentsize);
    write_u16(f, h->e_shnum);
    write_u16(f, h->e_shstrndx);
}

static void write_elf64_shdr(FILE *f, Elf64_Shdr *s) {
    write_u32(f, s->sh_name);
    write_u32(f, s->sh_type);
    write_u64(f, s->sh_flags);
    write_u64(f, s->sh_addr);
    write_u64(f, s->sh_offset);
    write_u64(f, s->sh_size);
    write_u32(f, s->sh_link);
    write_u32(f, s->sh_info);
    write_u64(f, s->sh_addralign);
    write_u64(f, s->sh_entsize);
}

static void write_elf64_sym(FILE *f, Elf64_Sym *s) {
    write_u32(f, s->st_name);
    write_u8(f, s->st_info);
    write_u8(f, s->st_other);
    write_u16(f, s->st_shndx);
    write_u64(f, s->st_value);
    write_u64(f, s->st_size);
}

static void write_elf64_rela(FILE *f, Elf64_Rela *r) {
    write_u64(f, r->r_offset);
    write_u64(f, r->r_info);
    write_i64(f, r->r_addend);
}

static void pad_to(FILE *f, long cur, long target) {
    while (cur < target) { fputc(0, f); cur++; }
}

void codegen_emit_elf(CodeGen *cg, const char *outfile) {
    FILE *f = fopen(outfile, "wb");
    if (!f) {
        fprintf(stderr, "cannot open '%s': %s\n", outfile, strerror(errno));
        return;
    }

    /* ---- Build symbol table ---- */
    /* ELF symbol table: local symbols first, then global */

    /* Build string table */
    StrTab strtab;
    strtab_init(&strtab);

    /* Count symbols */
    int nsyms = 1; /* index 0 = STN_UNDEF */
    for (SymEntry *s = cg->syms; s; s = s->next) nsyms++;

    /* Allocate ELF symbol array */
    Elf64_Sym *syms = calloc(nsyms, sizeof(Elf64_Sym));
    /* Index 0: undefined */
    syms[0].st_name  = 0;
    syms[0].st_info  = 0;
    syms[0].st_other = STV_DEFAULT;
    syms[0].st_shndx = SHN_UNDEF;
    syms[0].st_value = 0;
    syms[0].st_size  = 0;

    /* Build symbol index map and fill syms array */
    /* First: local symbols */
    int sym_idx = 1;
    int first_global = 1; /* index of first global symbol */

    /* Two-pass: locals first */
    for (SymEntry *s = cg->syms; s; s = s->next) {
        if (s->is_global) continue;
        syms[sym_idx].st_name  = (uint32_t)strtab_add(&strtab, s->name);
        syms[sym_idx].st_other = STV_DEFAULT;
        syms[sym_idx].st_value = (uint64_t)s->offset;
        syms[sym_idx].st_size  = (uint64_t)s->size;

        uint16_t shndx;
        uint8_t  stt;
        if (s->kind == CGSYM_FUNC) {
            shndx = SEC_TEXT;
            stt   = STT_FUNC;
        } else if (s->kind == CGSYM_RODATA) {
            shndx = SEC_RODATA;
            stt   = STT_OBJECT;
        } else {
            shndx = SEC_DATA;
            stt   = STT_OBJECT;
        }
        syms[sym_idx].st_shndx = shndx;
        syms[sym_idx].st_info  = ELF64_ST_INFO(STB_LOCAL, stt);
        sym_idx++;
    }

    first_global = sym_idx;

    /* Globals */
    for (SymEntry *s = cg->syms; s; s = s->next) {
        if (!s->is_global) continue;
        syms[sym_idx].st_name  = (uint32_t)strtab_add(&strtab, s->name);
        syms[sym_idx].st_other = STV_DEFAULT;
        syms[sym_idx].st_value = (uint64_t)s->offset;
        syms[sym_idx].st_size  = (uint64_t)s->size;

        uint16_t shndx;
        uint8_t  stt;
        if (s->kind == CGSYM_EXTERN) {
            shndx = SHN_UNDEF;
            stt   = STT_NOTYPE;
        } else if (s->kind == CGSYM_FUNC) {
            shndx = SEC_TEXT;
            stt   = STT_FUNC;
        } else if (s->kind == CGSYM_RODATA) {
            shndx = SEC_RODATA;
            stt   = STT_OBJECT;
        } else {
            shndx = SEC_DATA;
            stt   = STT_OBJECT;
        }
        syms[sym_idx].st_shndx = shndx;
        syms[sym_idx].st_info  = ELF64_ST_INFO(STB_GLOBAL, stt);
        sym_idx++;
    }

    int total_syms = sym_idx;

    /* ---- Build relocation table ---- */
    /* Count relocations */
    int nrelas = 0;
    for (Reloc *r = cg->relocs; r; r = r->next) nrelas++;

    Elf64_Rela *relas = calloc(nrelas, sizeof(Elf64_Rela));
    int ri = 0;

    /* Build a name → sym_idx map for reloc targets */
    /* Simple linear search (OK for small programs) */
    for (Reloc *r = cg->relocs; r; r = r->next, ri++) {
        /* Find symbol index */
        int sidx = 0;
        int scan = 1;
        for (SymEntry *s = cg->syms; s; s = s->next, scan++) {
            if (strcmp(s->name, r->sym_name) == 0) { sidx = scan; break; }
        }
        /* Also check backward order (syms is prepended) */
        if (sidx == 0) {
            /* Symbol not found: add as undefined */
            sidx = 0; /* will link as STN_UNDEF */
        }

        relas[ri].r_offset = (uint64_t)r->offset;
        relas[ri].r_info   = ELF64_R_INFO((uint32_t)sidx, (uint32_t)r->type);
        relas[ri].r_addend = r->addend;
    }

    /* ---- Build section name string table ---- */
    StrTab shstrtab;
    strtab_init(&shstrtab);

    uint32_t sh_null_name     = (uint32_t)strtab_add(&shstrtab, "");
    uint32_t sh_text_name     = (uint32_t)strtab_add(&shstrtab, ".text");
    uint32_t sh_rodata_name   = (uint32_t)strtab_add(&shstrtab, ".rodata");
    uint32_t sh_data_name     = (uint32_t)strtab_add(&shstrtab, ".data");
    uint32_t sh_bss_name      = (uint32_t)strtab_add(&shstrtab, ".bss");
    uint32_t sh_symtab_name   = (uint32_t)strtab_add(&shstrtab, ".symtab");
    uint32_t sh_strtab_name   = (uint32_t)strtab_add(&shstrtab, ".strtab");
    uint32_t sh_shstrtab_name = (uint32_t)strtab_add(&shstrtab, ".shstrtab");
    uint32_t sh_rela_text_name= (uint32_t)strtab_add(&shstrtab, ".rela.text");

    /* ---- Compute file layout ---- */
    /*
     * File layout:
     *   [0x00]  ELF header (64 bytes)
     *   [0x40]  .text
     *   [0x40 + text_size aligned to 16]  .rodata
     *   ... .data, .bss(no bytes), .symtab, .strtab, .shstrtab, .rela.text
     *   [end]   Section header table
     */

    size_t text_size   = enc_size(cg->enc);
    size_t rodata_size = cg->rodata_buf.size;
    size_t data_size   = cg->data_buf.size;
    size_t bss_size    = cg->bss_size;
    size_t symtab_size = (size_t)total_syms * sizeof(Elf64_Sym);
    size_t strtab_size = strtab.size;
    size_t shstrtab_size = shstrtab.size;
    size_t rela_size   = (size_t)nrelas * sizeof(Elf64_Rela);

    /* Align sections to 16 bytes */
#define ALIGN16(x) (((x) + 15) & ~(size_t)15)

    uint64_t off_text      = 0x40;
    uint64_t off_rodata    = off_text    + ALIGN16(text_size);
    uint64_t off_data      = off_rodata  + ALIGN16(rodata_size);
    uint64_t off_symtab    = off_data    + ALIGN16(data_size);
    uint64_t off_strtab    = off_symtab  + ALIGN16(symtab_size);
    uint64_t off_shstrtab  = off_strtab  + ALIGN16(strtab_size);
    uint64_t off_rela_text = off_shstrtab + ALIGN16(shstrtab_size);
    uint64_t off_shdrs     = off_rela_text + ALIGN16(rela_size);

    /* ---- Write ELF header ---- */
    Elf64_Ehdr ehdr = {0};
    ehdr.e_ident[EI_MAG0]    = ELFMAG0;
    ehdr.e_ident[EI_MAG1]    = ELFMAG1;
    ehdr.e_ident[EI_MAG2]    = ELFMAG2;
    ehdr.e_ident[EI_MAG3]    = ELFMAG3;
    ehdr.e_ident[EI_CLASS]   = ELFCLASS64;
    ehdr.e_ident[EI_DATA]    = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI]   = ELFOSABI_NONE;
    ehdr.e_type      = ET_REL;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_version   = EV_CURRENT;
    ehdr.e_entry     = 0;
    ehdr.e_phoff     = 0;   /* no program headers for .o file */
    ehdr.e_shoff     = off_shdrs;
    ehdr.e_flags     = 0;
    ehdr.e_ehsize    = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 0;
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum     = NSECTIONS;
    ehdr.e_shstrndx  = SEC_SHSTRTAB;

    write_elf64_ehdr(f, &ehdr);

    /* ---- Write sections ---- */

    /* .text */
    fseek(f, (long)off_text, SEEK_SET);
    fwrite(enc_bytes(cg->enc), 1, text_size, f);

    /* .rodata */
    fseek(f, (long)off_rodata, SEEK_SET);
    if (rodata_size > 0) fwrite(cg->rodata_buf.data, 1, rodata_size, f);

    /* .data */
    fseek(f, (long)off_data, SEEK_SET);
    if (data_size > 0) fwrite(cg->data_buf.data, 1, data_size, f);

    /* .symtab */
    fseek(f, (long)off_symtab, SEEK_SET);
    for (int i = 0; i < total_syms; i++) write_elf64_sym(f, &syms[i]);

    /* .strtab */
    fseek(f, (long)off_strtab, SEEK_SET);
    fwrite(strtab.data, 1, strtab_size, f);

    /* .shstrtab */
    fseek(f, (long)off_shstrtab, SEEK_SET);
    fwrite(shstrtab.data, 1, shstrtab_size, f);

    /* .rela.text */
    fseek(f, (long)off_rela_text, SEEK_SET);
    for (int i = 0; i < nrelas; i++) write_elf64_rela(f, &relas[i]);

    /* ---- Write section headers ---- */
    fseek(f, (long)off_shdrs, SEEK_SET);

    Elf64_Shdr shdrs[NSECTIONS] = {0};

    /* [0] NULL */
    shdrs[SEC_NULL].sh_name = sh_null_name;
    shdrs[SEC_NULL].sh_type = SHT_NULL;

    /* [1] .text */
    shdrs[SEC_TEXT].sh_name      = sh_text_name;
    shdrs[SEC_TEXT].sh_type      = SHT_PROGBITS;
    shdrs[SEC_TEXT].sh_flags     = SHF_ALLOC | SHF_EXECINSTR;
    shdrs[SEC_TEXT].sh_addr      = 0;
    shdrs[SEC_TEXT].sh_offset    = off_text;
    shdrs[SEC_TEXT].sh_size      = text_size;
    shdrs[SEC_TEXT].sh_link      = 0;
    shdrs[SEC_TEXT].sh_info      = 0;
    shdrs[SEC_TEXT].sh_addralign = 16;
    shdrs[SEC_TEXT].sh_entsize   = 0;

    /* [2] .rodata */
    shdrs[SEC_RODATA].sh_name      = sh_rodata_name;
    shdrs[SEC_RODATA].sh_type      = SHT_PROGBITS;
    shdrs[SEC_RODATA].sh_flags     = SHF_ALLOC;
    shdrs[SEC_RODATA].sh_addr      = 0;
    shdrs[SEC_RODATA].sh_offset    = off_rodata;
    shdrs[SEC_RODATA].sh_size      = rodata_size;
    shdrs[SEC_RODATA].sh_addralign = 1;
    shdrs[SEC_RODATA].sh_entsize   = 0;

    /* [3] .data */
    shdrs[SEC_DATA].sh_name      = sh_data_name;
    shdrs[SEC_DATA].sh_type      = SHT_PROGBITS;
    shdrs[SEC_DATA].sh_flags     = SHF_ALLOC | SHF_WRITE;
    shdrs[SEC_DATA].sh_addr      = 0;
    shdrs[SEC_DATA].sh_offset    = off_data;
    shdrs[SEC_DATA].sh_size      = data_size;
    shdrs[SEC_DATA].sh_addralign = 8;
    shdrs[SEC_DATA].sh_entsize   = 0;

    /* [4] .bss */
    shdrs[SEC_BSS].sh_name      = sh_bss_name;
    shdrs[SEC_BSS].sh_type      = SHT_NOBITS;
    shdrs[SEC_BSS].sh_flags     = SHF_ALLOC | SHF_WRITE;
    shdrs[SEC_BSS].sh_addr      = 0;
    shdrs[SEC_BSS].sh_offset    = off_data + data_size;  /* no bytes in file */
    shdrs[SEC_BSS].sh_size      = bss_size;
    shdrs[SEC_BSS].sh_addralign = 8;
    shdrs[SEC_BSS].sh_entsize   = 0;

    /* [5] .symtab */
    shdrs[SEC_SYMTAB].sh_name      = sh_symtab_name;
    shdrs[SEC_SYMTAB].sh_type      = SHT_SYMTAB;
    shdrs[SEC_SYMTAB].sh_flags     = 0;
    shdrs[SEC_SYMTAB].sh_addr      = 0;
    shdrs[SEC_SYMTAB].sh_offset    = off_symtab;
    shdrs[SEC_SYMTAB].sh_size      = symtab_size;
    shdrs[SEC_SYMTAB].sh_link      = SEC_STRTAB;   /* associated string table */
    shdrs[SEC_SYMTAB].sh_info      = (uint32_t)first_global;  /* first global sym index */
    shdrs[SEC_SYMTAB].sh_addralign = 8;
    shdrs[SEC_SYMTAB].sh_entsize   = sizeof(Elf64_Sym);

    /* [6] .strtab */
    shdrs[SEC_STRTAB].sh_name      = sh_strtab_name;
    shdrs[SEC_STRTAB].sh_type      = SHT_STRTAB;
    shdrs[SEC_STRTAB].sh_flags     = 0;
    shdrs[SEC_STRTAB].sh_addr      = 0;
    shdrs[SEC_STRTAB].sh_offset    = off_strtab;
    shdrs[SEC_STRTAB].sh_size      = strtab_size;
    shdrs[SEC_STRTAB].sh_addralign = 1;
    shdrs[SEC_STRTAB].sh_entsize   = 0;

    /* [7] .shstrtab */
    shdrs[SEC_SHSTRTAB].sh_name      = sh_shstrtab_name;
    shdrs[SEC_SHSTRTAB].sh_type      = SHT_STRTAB;
    shdrs[SEC_SHSTRTAB].sh_flags     = 0;
    shdrs[SEC_SHSTRTAB].sh_addr      = 0;
    shdrs[SEC_SHSTRTAB].sh_offset    = off_shstrtab;
    shdrs[SEC_SHSTRTAB].sh_size      = shstrtab_size;
    shdrs[SEC_SHSTRTAB].sh_addralign = 1;
    shdrs[SEC_SHSTRTAB].sh_entsize   = 0;

    /* [8] .rela.text */
    shdrs[SEC_RELA_TEXT].sh_name      = sh_rela_text_name;
    shdrs[SEC_RELA_TEXT].sh_type      = SHT_RELA;
    shdrs[SEC_RELA_TEXT].sh_flags     = 0x40ULL; /* SHF_INFO_LINK */
    shdrs[SEC_RELA_TEXT].sh_addr      = 0;
    shdrs[SEC_RELA_TEXT].sh_offset    = off_rela_text;
    shdrs[SEC_RELA_TEXT].sh_size      = rela_size;
    shdrs[SEC_RELA_TEXT].sh_link      = SEC_SYMTAB;  /* associated symbol table */
    shdrs[SEC_RELA_TEXT].sh_info      = SEC_TEXT;     /* section being relocated */
    shdrs[SEC_RELA_TEXT].sh_addralign = 8;
    shdrs[SEC_RELA_TEXT].sh_entsize   = sizeof(Elf64_Rela);

    for (int i = 0; i < NSECTIONS; i++) write_elf64_shdr(f, &shdrs[i]);

    fclose(f);

    /* Cleanup */
    free(syms);
    free(relas);
    strtab_free(&strtab);
    strtab_free(&shstrtab);
}

