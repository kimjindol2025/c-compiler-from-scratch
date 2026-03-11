#pragma once
#include <stdint.h>

/* =========================================================
 * ELF64 structures and constants for x86-64
 * ========================================================= */

/* ELF identification */
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define EI_OSABI    7
#define EI_NIDENT   16

#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

#define ELFCLASS64  2
#define ELFDATA2LSB 1   /* little-endian */
#define EV_CURRENT  1
#define ELFOSABI_NONE 0

/* e_type */
#define ET_NONE  0
#define ET_REL   1   /* relocatable */
#define ET_EXEC  2   /* executable  */
#define ET_DYN   3   /* shared obj  */
#define ET_CORE  4

/* e_machine */
#define EM_X86_64 62

/* section types */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_NOTE        7
#define SHT_NOBITS      8   /* .bss */
#define SHT_REL         9
#define SHT_DYNSYM      11

/* section flags */
#define SHF_WRITE       (1 << 0)
#define SHF_ALLOC       (1 << 1)
#define SHF_EXECINSTR   (1 << 2)
#define SHF_MERGE       (1 << 4)
#define SHF_STRINGS     (1 << 5)

/* special section indices */
#define SHN_UNDEF   0
#define SHN_ABS     0xfff1
#define SHN_COMMON  0xfff2

/* symbol binding (high nibble of st_info) */
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2

/* symbol type (low nibble of st_info) */
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4

#define ELF64_ST_BIND(i)    ((i) >> 4)
#define ELF64_ST_TYPE(i)    ((i) & 0xf)
#define ELF64_ST_INFO(b,t)  (((b)<<4)+((t)&0xf))

/* symbol visibility */
#define STV_DEFAULT 0

/* relocation types for x86-64 */
#define R_X86_64_NONE       0
#define R_X86_64_64         1   /* 64-bit absolute */
#define R_X86_64_PC32       2   /* 32-bit PC-relative */
#define R_X86_64_GOT32      3
#define R_X86_64_PLT32      4   /* 32-bit PC-relative to PLT */
#define R_X86_64_COPY       5
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_32         10  /* 32-bit zero-extended absolute */
#define R_X86_64_32S        11  /* 32-bit sign-extended absolute */
#define R_X86_64_PC64       24

#define ELF64_R_SYM(i)     ((i) >> 32)
#define ELF64_R_TYPE(i)    ((i) & 0xffffffffUL)
#define ELF64_R_INFO(s,t)  (((uint64_t)(s) << 32) | (uint64_t)(t))

/* program header types */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_PHDR     6

/* program header flags */
#define PF_X (1 << 0)  /* execute */
#define PF_W (1 << 1)  /* write   */
#define PF_R (1 << 2)  /* read    */

/* ---- ELF64 header ---- */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;    /* program header table offset */
    uint64_t e_shoff;    /* section header table offset */
    uint32_t e_flags;
    uint16_t e_ehsize;   /* size of this header */
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx; /* index of .shstrtab */
} Elf64_Ehdr;

/* ---- Section header ---- */
typedef struct {
    uint32_t sh_name;       /* offset into .shstrtab */
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;     /* file offset */
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

/* ---- Program header ---- */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* ---- Symbol table entry ---- */
typedef struct {
    uint32_t st_name;   /* offset into .strtab */
    uint8_t  st_info;   /* STB_* << 4 | STT_* */
    uint8_t  st_other;  /* STV_* */
    uint16_t st_shndx;  /* section index */
    uint64_t st_value;  /* offset within section */
    uint64_t st_size;
} Elf64_Sym;

/* ---- Relocation with addend ---- */
typedef struct {
    uint64_t r_offset;  /* offset in section where reloc applies */
    uint64_t r_info;    /* ELF64_R_INFO(sym_idx, R_X86_64_*) */
    int64_t  r_addend;
} Elf64_Rela;
