#include "core/ce.h"
#include "core/machine.h"
#include "core/mips.h"
#include "core/optimiser.h"
#include "util/file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE      (16u << 20)
#define PAGE_SIZE     0x1000u
#define ROM_PAGES     256
#define KSEG0         0x80000000u
#define AREA_MASK     0x1FFFFFFFu
#define RETURN_VA     0x80004000u
#define ALIAS_VA      0x80100000u
#define STACK_TOP     0x80E00000u
#define DATA_VA       0x80800000u
#define DATA_SIZE     0x10000u
#define SOURCE_VA     (DATA_VA + 0x100u)
#define OTHER_VA      (DATA_VA + 0x2000u)
#define TABLE_VA      (DATA_VA + 0x4000u)
#define TARGET_VA     (DATA_VA + 0x8000u)
#define MODULE_VA     (DATA_VA + 0xC000u)
#define EXPORTS_RVA   0xE000u
#define FUNCTIONS_RVA 0xD000u
#define ORDINALS_RVA  0xD400u
#define NAME_LIST_RVA 0xD800u
#define NAMES_RVA     0x1000u
#define NAME_SLOT     16u
#define CODE_BYTES    0x400u
#define BOOT_SECONDS  20u
#define RUN_CYCLES    5000000u
#define TRIALS        4000
#define J_RETURN      (0x08000000u | ((RETURN_VA & 0x0FFFFFFFu) >> 2))

typedef enum { KIND_OTHER, KIND_ZERO, KIND_MOVE, KIND_WIDEN, KIND_RANGE, KIND_EXPORT } kind_t;

static uint8_t *ram;
static machine_t *machine;
static uint32_t rom_page_pa[ROM_PAGES];
static uint8_t *rom_page[ROM_PAGES];
static int rom_page_count;
static mips_cpu_t guest, native;

static uint8_t *rom_page_at(uint32_t pa) {
    uint32_t base = pa & ~(PAGE_SIZE - 1);
    for (int i = 0; i < rom_page_count; i++) {
        if (rom_page_pa[i] == base) return rom_page[i];
    }
    if (rom_page_count == ROM_PAGES) return NULL;
    uint8_t *page = malloc(PAGE_SIZE);
    if (!page || !machine_read_physical(machine, base, page, PAGE_SIZE)) {
        free(page);
        return NULL;
    }
    rom_page_pa[rom_page_count] = base;
    rom_page[rom_page_count++] = page;
    return page;
}

static bool bus_read(void *context, uint32_t pa, int size, uint32_t *value) {
    (void)context;
    uint8_t *page = rom_page_at(pa);
    if (!page) return false;
    *value = 0;
    for (int i = 0; i < size; i++) *value |= (uint32_t)page[(pa & (PAGE_SIZE - 1)) + (uint32_t)i] << (8 * i);
    return true;
}

static bool bus_write(void *context, uint32_t pa, int size, uint32_t value) {
    (void)context;
    (void)pa;
    (void)size;
    (void)value;
    return false;
}

static uint8_t *bus_fetch_page(void *context, uint32_t pa) {
    (void)context;
    return pa < RAM_SIZE ? ram + pa : rom_page_at(pa);
}

static uint8_t *memory_map(void *context, uint32_t va, bool write) {
    (void)context;
    (void)write;
    if (va < KSEG0) return NULL;
    uint32_t pa = va & AREA_MASK;
    return pa < RAM_SIZE ? ram + pa : NULL;
}

static const uint8_t *rom_at(void *context, uint32_t pa, uint32_t length) {
    static uint8_t buffer[CODE_BYTES];
    (void)context;
    return length <= sizeof buffer && machine_read_physical(machine, pa, buffer, length) ? buffer : NULL;
}

static void returned(void *context, uint32_t pc) {
    (void)context;
    (void)pc;
    guest.yield = true;
}

static void start(mips_cpu_t *cpu, uint32_t va) {
    memset(cpu, 0, sizeof *cpu);
    cpu->bus = (mips_bus_t){ NULL, bus_read, bus_write, bus_fetch_page, ram, RAM_SIZE - 1, RAM_SIZE };
    cpu->speed = 1;
    mips_reset(cpu, va);
    cpu->on_watch = returned;
    cpu->watch[0] = RETURN_VA;
    cpu->watch_count = 1;
    cpu->gpr[29] = STACK_TOP;
    cpu->gpr[31] = RETURN_VA;
}

static uint32_t random_word(void) {
    return (uint32_t)rand() << 16 ^ (uint32_t)rand();
}

static void put16(uint32_t va, uint32_t value) {
    ram[(va & AREA_MASK)] = (uint8_t)value;
    ram[(va & AREA_MASK) + 1] = (uint8_t)(value >> 8);
}

static void put32(uint32_t va, uint32_t value) {
    put16(va, value);
    put16(va + 2, value >> 16);
}

static void random_string(uint32_t va, int length, int alphabet) {
    for (int i = 0; i < length; i++) ram[(va & AREA_MASK) + (uint32_t)i] = (uint8_t)(1 + rand() % alphabet);
    ram[(va & AREA_MASK) + (uint32_t)length] = 0;
}

static kind_t kind_of(native_fn run) {
    if (run == native_zero) return KIND_ZERO;
    if (run == native_memmove) return KIND_MOVE;
    if (run == native_widen) return KIND_WIDEN;
    if (run == native_range_lookup16) return KIND_RANGE;
    if (run == native_export_lookup) return KIND_EXPORT;
    return KIND_OTHER;
}

static const char *kind_name(kind_t kind) {
    static const char *names[] = { "other", "zero", "memmove", "widen", "range", "export" };
    return names[kind];
}

static void prepare(kind_t kind, mips_cpu_t *cpu, int trial) {
    memset(ram + (DATA_VA & AREA_MASK), 0xA5, DATA_SIZE);
    switch (kind) {
    case KIND_ZERO:
        cpu->gpr[4] = TARGET_VA + (uint32_t)(rand() % 8) * 4;
        cpu->gpr[5] = (uint32_t)(1 + rand() % 64) * 32;
        break;
    case KIND_MOVE: {
        uint32_t length = (uint32_t)(rand() % 600), base = TARGET_VA;
        for (uint32_t i = 0; i < 2048; i++) ram[(base & AREA_MASK) + i] = (uint8_t)random_word();
        cpu->gpr[4] = base + (uint32_t)(rand() % 700);
        cpu->gpr[5] = trial % 3 ? base + (uint32_t)(rand() % 700) : SOURCE_VA + (uint32_t)(rand() % 64);
        cpu->gpr[6] = length;
        break;
    }
    case KIND_WIDEN:
        random_string(SOURCE_VA + (uint32_t)(trial % 3), rand() % 50, 255);
        cpu->gpr[4] = TARGET_VA;
        cpu->gpr[5] = SOURCE_VA + (uint32_t)(trial % 3);
        cpu->gpr[6] = (uint32_t)(rand() % 64 - 4);
        break;
    case KIND_RANGE: {
        int count = rand() % 40;
        uint32_t next = random_word() % 200;
        for (int i = 0; i < count; i++) {
            uint32_t first = next + random_word() % 50, last = first + random_word() % 30;
            put16(TABLE_VA + (uint32_t)i * 6, first);
            put16(TABLE_VA + (uint32_t)i * 6 + 2, last);
            put16(TABLE_VA + (uint32_t)i * 6 + 4, random_word());
            next = last + 1;
        }
        cpu->gpr[4] = TABLE_VA;
        cpu->gpr[5] = (uint32_t)count;
        cpu->gpr[6] = trial % 5 ? random_word() % (next + 20) : random_word();
        break;
    }
    case KIND_EXPORT: {
        int count = rand() % 60, functions = count + rand() % 4 - 2, chosen = rand() % (count + 2);
        put32(MODULE_VA + 80, DATA_VA);
        put32(MODULE_VA + 124, trial % 17 ? EXPORTS_RVA : 0);
        put32(MODULE_VA + 128, 40);
        put32(DATA_VA + EXPORTS_RVA + 20, (uint32_t)(functions < 0 ? 0 : functions));
        put32(DATA_VA + EXPORTS_RVA + 24, (uint32_t)count);
        put32(DATA_VA + EXPORTS_RVA + 28, FUNCTIONS_RVA);
        put32(DATA_VA + EXPORTS_RVA + 32, NAME_LIST_RVA);
        put32(DATA_VA + EXPORTS_RVA + 36, ORDINALS_RVA);
        for (int i = 0; i < count; i++) {
            uint32_t name = NAMES_RVA + (uint32_t)i * NAME_SLOT;
            random_string(DATA_VA + name, 1 + rand() % (int)(NAME_SLOT - 2), 3);
            put32(DATA_VA + NAME_LIST_RVA + (uint32_t)i * 4, name);
            put16(DATA_VA + ORDINALS_RVA + (uint32_t)i * 2, (uint32_t)(rand() % 64));
        }
        for (int i = 0; i < 64; i++) put32(DATA_VA + FUNCTIONS_RVA + (uint32_t)i * 4, 0x100u + random_word() % 0xE00u);
        if (chosen < count) memcpy(ram + (SOURCE_VA & AREA_MASK), ram + ((DATA_VA + NAMES_RVA) & AREA_MASK) + (uint32_t)chosen * NAME_SLOT, NAME_SLOT);
        else random_string(SOURCE_VA, 1 + rand() % 12, 3);
        cpu->gpr[4] = MODULE_VA;
        cpu->gpr[5] = SOURCE_VA;
        break;
    }
    default:
        break;
    }
}

static bool returned_to_caller(const mips_cpu_t *cpu) {
    return cpu->pc == RETURN_VA || cpu->pc == RETURN_VA + 4;
}

static bool same_result(const mips_cpu_t *expected, const mips_cpu_t *actual, const uint8_t *expected_data) {
    if (!returned_to_caller(expected) || !returned_to_caller(actual) || expected->gpr[2] != actual->gpr[2]) return false;
    for (int i = 16; i < 24; i++) {
        if (expected->gpr[i] != actual->gpr[i]) return false;
    }
    if (expected->gpr[29] != actual->gpr[29] || expected->gpr[30] != actual->gpr[30]) return false;
    return memcmp(expected_data, ram + (DATA_VA & AREA_MASK), DATA_SIZE) == 0;
}

static void run_until_return(mips_cpu_t *cpu) {
    guest = *cpu;
    mips_run(&guest, RUN_CYCLES);
    *cpu = guest;
}

static uint32_t guest_entry(const optimiser_hook_t *hook) {
    return hook->state == OPTIMISER_UNCHECKED ? ALIAS_VA + (hook->va & 0xFFFu) : hook->va;
}

static bool load_module_code(optimiser_hook_t *hook) {
    ce_t ce;
    uint8_t code[CODE_BYTES];
    ce_init(&ce, machine);
    if (!ce_read(&ce, hook->va, CE_CURRENT, code, sizeof code)) return false;
    for (uint32_t i = 0; i < hook->words; i++) {
        uint32_t word = (uint32_t)code[i * 4] | (uint32_t)code[i * 4 + 1] << 8 | (uint32_t)code[i * 4 + 2] << 16 | (uint32_t)code[i * 4 + 3] << 24;
        if (word != hook->code[i]) return false;
    }
    memcpy(ram + (guest_entry(hook) & AREA_MASK), code, sizeof code);
    return true;
}

static int check_hook(optimiser_t *optimiser, int index) {
    optimiser_hook_t *hook = &optimiser->hooks[index];
    kind_t kind = kind_of(hook->run);
    if (kind == KIND_OTHER) return 0;
    bool aliased = hook->state == OPTIMISER_UNCHECKED;
    if (aliased && !load_module_code(hook)) {
        printf("%-8s %08X: code not found in memory after %u s\n", kind_name(kind), hook->va, BOOT_SECONDS);
        return 1;
    }
    uint32_t entry = guest_entry(hook);
    if (aliased) hook->state = OPTIMISER_MATCHED;
    static uint8_t expected_data[DATA_SIZE], data_before[DATA_SIZE];
    int failures = 0, declined = 0;
    for (int trial = 0; trial < TRIALS; trial++) {
        srand((unsigned)(trial * 7919 + index));
        mips_cpu_t cpu;
        start(&cpu, entry);
        cpu.gpr[2] = random_word();
        prepare(kind, &cpu, trial);
        native = cpu;
        memcpy(data_before, ram + (DATA_VA & AREA_MASK), DATA_SIZE);
        run_until_return(&cpu);
        memcpy(expected_data, ram + (DATA_VA & AREA_MASK), DATA_SIZE);
        memcpy(ram + (DATA_VA & AREA_MASK), data_before, DATA_SIZE);
        if (!optimiser_call(optimiser, &native, hook->va)) {
            declined++;
            continue;
        }
        if (!returned_to_caller(&native)) run_until_return(&native);
        if (!same_result(&cpu, &native, expected_data) && failures++ < 3) {
            printf("  %s trial %d: guest v0 %08X pc %08X, native v0 %08X pc %08X\n", kind_name(kind), trial, cpu.gpr[2], cpu.pc, native.gpr[2], native.pc);
        }
    }
    printf("%-8s %08X: %d trials, %d declined, %d differ\n", kind_name(kind), hook->va, TRIALS, declined, failures);
    return failures || declined == TRIALS;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: native-check ROM\n");
        return 2;
    }
    size_t rom_size;
    uint8_t *rom = file_read(argv[1], &rom_size);
    if (!rom) {
        fprintf(stderr, "native-check: cannot read %s\n", argv[1]);
        return 1;
    }
    char error[256];
    machine = machine_create(rom, rom_size, error, sizeof error);
    free(rom);
    if (!machine) {
        fprintf(stderr, "native-check: %s\n", error);
        return 1;
    }
    machine_run(machine, (uint64_t)BOOT_SECONDS * MACHINE_CLOCK_HZ);
    ram = calloc(1, RAM_SIZE);
    put32(RETURN_VA, J_RETURN);
    put32(RETURN_VA + 4, 0);
    optimiser_t optimiser;
    optimiser_init(&optimiser, rom_at, NULL, (native_memory_t){ NULL, memory_map });
    if (!optimiser.hook_count) {
        fprintf(stderr, "native-check: no optimisation profile matches %s\n", argv[1]);
        return 1;
    }
    printf("%s\n", optimiser.profile);
    int failed = 0;
    for (int i = 0; i < optimiser.hook_count; i++) failed |= check_hook(&optimiser, i);
    machine_destroy(machine);
    free(ram);
    return failed;
}
