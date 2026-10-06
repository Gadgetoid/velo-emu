#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MIPS_TLB_ENTRIES 32

enum {
    MIPS_EXC_INT  = 0,
    MIPS_EXC_MOD  = 1,
    MIPS_EXC_TLBL = 2,
    MIPS_EXC_TLBS = 3,
    MIPS_EXC_ADEL = 4,
    MIPS_EXC_ADES = 5,
    MIPS_EXC_IBE  = 6,
    MIPS_EXC_DBE  = 7,
    MIPS_EXC_SYS  = 8,
    MIPS_EXC_BP   = 9,
    MIPS_EXC_RI   = 10,
    MIPS_EXC_CPU  = 11,
    MIPS_EXC_OV   = 12,
};

enum {
    CP0_INDEX    = 0,
    CP0_RANDOM   = 1,
    CP0_ENTRYLO  = 2,
    CP0_CONFIG   = 3,
    CP0_CONTEXT  = 4,
    CP0_BADVADDR = 8,
    CP0_ENTRYHI  = 10,
    CP0_STATUS   = 12,
    CP0_CAUSE    = 13,
    CP0_EPC      = 14,
    CP0_PRID     = 15,
};

typedef struct {
    uint32_t vpn;
    uint32_t pid;
    uint32_t pfn;
    bool     global;
    bool     valid;
    bool     dirty;
    bool     noncache;
} mips_tlb_entry_t;

typedef struct mips_cpu mips_cpu_t;

typedef struct {
    void    *context;
    bool   (*read)(void *context, uint32_t pa, int size, uint32_t *value);
    bool   (*write)(void *context, uint32_t pa, int size, uint32_t value);
    uint8_t *(*fetch_page)(void *context, uint32_t pa);
    uint8_t  *dram;
    uint32_t  dram_mask;
    uint32_t  dram_end;
} mips_bus_t;

#define MIPS_WATCH_MAX 8
#define MIPS_PAGE_CACHE 64
#define MIPS_FETCH_CACHE 32

typedef struct {
    uint32_t tag;
    uint32_t pfn;
    bool     dirty;
} mips_page_cache_t;

typedef struct {
    uint32_t tag;
    uint8_t *page;
} mips_fetch_cache_t;
#define MIPS_SLOT_SIZE 0x02000000u

typedef struct {
    void    *context;
    bool   (*before)(void *context, uint32_t pc);
    bool   (*access)(void *context, uint32_t va, int size, bool write);
    void   (*exception)(void *context, uint32_t code, uint32_t pc, bool user);
    uint32_t filter[128];
    uint32_t pc;
    bool     every;
    bool     data;
    bool     stop;
    bool     undo;
} mips_debug_t;

struct mips_cpu {
    uint32_t gpr[32];
    uint32_t hi, lo;
    uint32_t pc;
    uint32_t next_pc;
    bool     in_delay_slot;
    bool     next_in_delay_slot;
    uint32_t current_pc;
    bool     current_in_delay_slot;
    uint32_t epoch;
    uint64_t run_until, run_base_cycles;
    uint32_t run_base_count, run_base_budget, run_stash, run_window_epoch;
    uint32_t cp0[32];
    uint32_t external_ip;
    mips_tlb_entry_t tlb[MIPS_TLB_ENTRIES];
    uint32_t random_state;
    uint64_t cycles;
    uint32_t speed;
    uint32_t speed_count;
    uint64_t exceptions[16];
    bool     fault;
    bool     yield;
    mips_bus_t bus;
    mips_fetch_cache_t fetch_cache[MIPS_FETCH_CACHE];
    mips_page_cache_t page_cache[MIPS_PAGE_CACHE];
    uint32_t watch[MIPS_WATCH_MAX];
    int      watch_count;
    uint32_t watch_filter[128];
    void   (*on_watch)(void *context, uint32_t pc);
    mips_debug_t *debug;
    bool   (*on_break)(void *context, uint32_t code);
};

void mips_reset(mips_cpu_t *cpu, uint32_t entry);
void mips_set_external_ip(mips_cpu_t *cpu, uint32_t ip_bits);
void mips_run(mips_cpu_t *cpu, uint64_t until_cycle);
bool mips_translate(mips_cpu_t *cpu, uint32_t va, bool write, uint32_t *pa);
void mips_raise_tlb_miss(mips_cpu_t *cpu, uint32_t va);
void mips_raise_tlb_store_miss(mips_cpu_t *cpu, uint32_t va);
void mips_flush_translations(mips_cpu_t *cpu);
void mips_debug_filter_add(mips_debug_t *debug, uint32_t va);
bool mips_user_mode(const mips_cpu_t *cpu);
uint32_t mips_asid(const mips_cpu_t *cpu);
