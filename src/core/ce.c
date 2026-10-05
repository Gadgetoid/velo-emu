#include "core/ce.h"

#include <string.h>
#include <strings.h>

#define KSEG0               0x80000000u
#define KSEG2               0xC0000000u
#define KSEG_PA_MASK        0x1FFFFFFFu
#define KDATA_PA            0x00001800u
#define KDATA_SECTIONS      0xC0u
#define PROCESS_SCAN_END    0x00100000u
#define BLOCK_PAGES         0x0Cu
#define BLOCK_RESERVED      1u
#define PAGE_VALID          (1u << 9)
#define PAGE_DIRTY          (1u << 10)
#define PAGE_FRAME          0xFFFFF000u
#define SLOT_BASE(process)  ((uint32_t)((process) + 1) * MIPS_SLOT_SIZE)

#define CE1_SIGNATURE       0x03AB15CDu
#define MODULE_SCAN_END     0x00080000u
#define CE1_MODULE_TYPE     0x10u
#define CE1_MODULE_MAGIC    0x4C444F4Du

static const uint32_t PROCESS_STRIDES[] = { 0x8C, 0x9C };

typedef struct {
    uint32_t vm_base;
    uint32_t name;
} process_layout_t;

static const process_layout_t LAYOUTS[] = {
    [1] = { 0x18, 0x30 },
    [2] = { 0x0C, 0x20 },
};

typedef struct {
    uint32_t self;
    uint32_t next;
    uint32_t name;
    uint32_t in_use;
    uint32_t base;
    uint32_t entry;
} module_layout_t;

static const module_layout_t MODULE_LAYOUTS[] = {
    [1] = { 0x04, 0x14, 0x18, 0x1C, 0x60, 0x6C },
    [2] = { 0x00, 0x04, 0x08, 0x0C, 0x50, 0x5C },
};

void ce_init(ce_t *ce, machine_t *machine) {
    memset(ce, 0, sizeof *ce);
    ce->machine = machine;
}

static bool physical_word(ce_t *ce, uint32_t pa, uint32_t *value) {
    uint8_t bytes[4];
    if (!machine_read_physical(ce->machine, pa, bytes, 4)) return false;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return true;
}

static bool kernel_word(ce_t *ce, uint32_t va, uint32_t *value) {
    if (va < KSEG0 || va >= KSEG2) return false;
    return physical_word(ce, va & KSEG_PA_MASK, value);
}

static bool slots_consistent(ce_t *ce, uint32_t vm_base_pa, uint32_t stride) {
    int used = 0;
    for (int process = 0; process < CE_PROCESS_MAX; process++) {
        uint32_t vm_base;
        if (!physical_word(ce, vm_base_pa + (uint32_t)process * stride, &vm_base)) return false;
        if (vm_base == SLOT_BASE(process)) used++;
        else if (vm_base || !process) return false;
    }
    return used >= 3;
}

static bool process_array_valid(ce_t *ce) {
    uint32_t first;
    return physical_word(ce, ce->process_array + LAYOUTS[ce->version].vm_base, &first) && first == SLOT_BASE(0);
}

static bool find_process_array(ce_t *ce) {
    for (uint32_t pa = 0; pa < PROCESS_SCAN_END; pa += 4) {
        uint32_t first, signature;
        if (!physical_word(ce, pa, &first) || first != SLOT_BASE(0)) continue;
        for (size_t i = 0; i < sizeof PROCESS_STRIDES / sizeof PROCESS_STRIDES[0]; i++) {
            if (!slots_consistent(ce, pa, PROCESS_STRIDES[i])) continue;
            ce->version = pa >= LAYOUTS[1].vm_base && physical_word(ce, pa - LAYOUTS[1].vm_base, &signature) && signature == CE1_SIGNATURE ? 1 : 2;
            ce->process_array = pa - LAYOUTS[ce->version].vm_base;
            ce->process_stride = PROCESS_STRIDES[i];
            return true;
        }
    }
    return false;
}

bool ce_ready(ce_t *ce) {
    if (ce->version && process_array_valid(ce)) return true;
    ce->version = 0;
    return find_process_array(ce);
}

int ce_current_process(ce_t *ce) {
    uint32_t current, section;
    if (physical_word(ce, KDATA_PA + KDATA_SECTIONS, &current) && current) {
        for (int slot = 1; slot <= CE_PROCESS_MAX; slot++) {
            if (physical_word(ce, KDATA_PA + KDATA_SECTIONS + (uint32_t)slot * 4, &section) && section == current) return slot - 1;
        }
    }
    return (int)mips_asid(machine_cpu(ce->machine));
}

static bool tlb_translate(ce_t *ce, uint32_t va, bool write, uint32_t *pa) {
    mips_cpu_t *cpu = machine_cpu(ce->machine);
    uint32_t asid = mips_asid(cpu);
    for (int i = 0; i < MIPS_TLB_ENTRIES; i++) {
        const mips_tlb_entry_t *entry = &cpu->tlb[i];
        if (entry->vpn != (va & PAGE_FRAME) || !entry->valid || (!entry->global && entry->pid != asid)) continue;
        if (write && !entry->dirty) return false;
        *pa = entry->pfn | (va & ~PAGE_FRAME);
        return true;
    }
    return false;
}

static bool section_translate(ce_t *ce, uint32_t va, bool write, uint32_t *pa) {
    uint32_t section, block, page;
    if (!physical_word(ce, KDATA_PA + KDATA_SECTIONS + (va >> 25) * 4, &section) || !section) return false;
    if (!kernel_word(ce, section + ((va >> 16) & 0x1FF) * 4, &block) || block <= BLOCK_RESERVED) return false;
    if (!kernel_word(ce, block + BLOCK_PAGES + ((va >> 12) & 15) * 4, &page) || !(page & PAGE_VALID)) return false;
    if (write && !(page & PAGE_DIRTY)) return false;
    *pa = (page & PAGE_FRAME) | (va & ~PAGE_FRAME);
    return true;
}

bool ce_translate(ce_t *ce, uint32_t va, int process, bool write, uint32_t *pa) {
    if (va >= KSEG0 && va < KSEG2) {
        *pa = va & KSEG_PA_MASK;
        return true;
    }
    if (va >= KSEG2) return tlb_translate(ce, va, write, pa);
    if (va < MIPS_SLOT_SIZE && process != CE_CURRENT && process != ce_current_process(ce)) va += SLOT_BASE(process);
    if (section_translate(ce, va, write, pa)) return true;
    return tlb_translate(ce, va, write, pa);
}

bool ce_read(ce_t *ce, uint32_t va, int process, uint8_t *data, uint32_t length) {
    while (length) {
        uint32_t pa, chunk = 0x1000 - (va & 0xFFF);
        if (chunk > length) chunk = length;
        if (!ce_translate(ce, va, process, false, &pa) || !machine_read_physical(ce->machine, pa, data, chunk)) return false;
        va += chunk;
        data += chunk;
        length -= chunk;
    }
    return true;
}

bool ce_write(ce_t *ce, uint32_t va, int process, const uint8_t *data, uint32_t length) {
    while (length) {
        uint32_t pa, chunk = 0x1000 - (va & 0xFFF);
        if (chunk > length) chunk = length;
        if (!ce_translate(ce, va, process, false, &pa) || !machine_write_physical(ce->machine, pa, data, chunk)) return false;
        va += chunk;
        data += chunk;
        length -= chunk;
    }
    return true;
}

bool ce_read_word(ce_t *ce, uint32_t va, int process, uint32_t *value) {
    uint8_t bytes[4];
    if (!ce_read(ce, va, process, bytes, 4)) return false;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return true;
}

static uint32_t process_entry(const ce_t *ce, int process) {
    return ce->process_array + (uint32_t)process * ce->process_stride;
}

bool ce_process_name(ce_t *ce, int process, char *name, size_t size) {
    name[0] = 0;
    if (process < 0 || process >= CE_PROCESS_MAX || !ce_ready(ce)) return false;
    uint32_t entry = process_entry(ce, process), vm_base, pointer;
    if (!physical_word(ce, entry + LAYOUTS[ce->version].vm_base, &vm_base) || vm_base != SLOT_BASE(process)) return false;
    if (!physical_word(ce, entry + LAYOUTS[ce->version].name, &pointer) || !pointer) return false;
    uint8_t text[CE_NAME_MAX * 2];
    uint32_t available = 0;
    while (available < sizeof text && ce_read(ce, pointer + available, process, text + available, 2)) available += 2;
    if (available < 2) return false;
    bool wide = text[1] == 0;
    size_t length = 0;
    for (uint32_t i = 0; i < available && length + 1 < size; i += wide ? 2 : 1) {
        uint8_t character = text[i];
        if (!character || (wide && text[i + 1])) break;
        name[length++] = (char)character;
    }
    name[length] = 0;
    return length > 0;
}

int ce_find_process(ce_t *ce, const char *name) {
    for (int process = 0; process < CE_PROCESS_MAX; process++) {
        char found[CE_NAME_MAX];
        if (ce_process_name(ce, process, found, sizeof found) && !strcasecmp(found, name)) return process;
    }
    return -1;
}

static bool module_valid(ce_t *ce, uint32_t module) {
    const module_layout_t *layout = &MODULE_LAYOUTS[ce->version];
    uint32_t self, signature;
    if (module < KSEG0 || module >= KSEG2 || (module & 3)) return false;
    if (!kernel_word(ce, module + layout->self, &self) || (self & KSEG_PA_MASK) != (module & KSEG_PA_MASK)) return false;
    if (ce->version != 1) return true;
    uint32_t type, magic;
    return kernel_word(ce, module, &signature) && signature == CE1_SIGNATURE && kernel_word(ce, module + CE1_MODULE_TYPE, &type) &&
           kernel_word(ce, type, &magic) && magic == CE1_MODULE_MAGIC;
}

static int chain_length(ce_t *ce, uint32_t module) {
    int length = 0;
    while (module && length < CE_MODULE_MAX * 4 && module_valid(ce, module)) {
        length++;
        if (!kernel_word(ce, module + MODULE_LAYOUTS[ce->version].next, &module)) break;
    }
    return length;
}

static bool find_module_list(ce_t *ce) {
    int best = 0;
    for (uint32_t pa = 0; pa < MODULE_SCAN_END; pa += 4) {
        uint32_t pointer;
        if (!physical_word(ce, pa, &pointer) || !module_valid(ce, pointer)) continue;
        int length = chain_length(ce, pointer);
        if (length > best) {
            best = length;
            ce->module_list = pa;
        }
    }
    return best > 0;
}

static void read_name(ce_t *ce, uint32_t pointer, int process, char *name, size_t size) {
    uint8_t text[CE_NAME_MAX * 2];
    uint32_t available = 0;
    name[0] = 0;
    while (available < sizeof text && ce_read(ce, pointer + available, process, text + available, 1)) available++;
    bool wide = available >= 2 && text[1] == 0;
    size_t length = 0;
    for (uint32_t i = 0; i < available && length + 1 < size; i += wide ? 2 : 1) {
        if (!text[i] || (wide && (i + 1 >= available || text[i + 1]))) break;
        name[length++] = (char)text[i];
    }
    name[length] = 0;
}

bool ce_first_module(ce_t *ce, uint32_t *module) {
    if (!ce_ready(ce)) return false;
    if (ce->module_list && physical_word(ce, ce->module_list, module) && module_valid(ce, *module)) return true;
    return find_module_list(ce) && physical_word(ce, ce->module_list, module);
}

bool ce_module_list_address(ce_t *ce, uint32_t *pa) {
    uint32_t module;
    if (!ce_first_module(ce, &module)) return false;
    *pa = ce->module_list;
    return true;
}

uint32_t ce_module_entry_field(ce_t *ce, uint32_t module) {
    return (module & KSEG_PA_MASK) + MODULE_LAYOUTS[ce->version].entry;
}

bool ce_module_entry(ce_t *ce, uint32_t module, uint32_t *entry) {
    return module_valid(ce, module) && kernel_word(ce, module + MODULE_LAYOUTS[ce->version].entry, entry) && *entry && *entry != 0xFFFFFFFFu;
}

int ce_modules(ce_t *ce, ce_module_t *modules, int max) {
    uint32_t module;
    if (!ce_first_module(ce, &module)) return 0;
    const module_layout_t *layout = &MODULE_LAYOUTS[ce->version];
    int count = 0;
    for (int guard = 0; module && guard < CE_MODULE_MAX * 4 && count < max && module_valid(ce, module); guard++) {
        uint32_t name, base, in_use;
        ce_module_t *entry = &modules[count];
        if (kernel_word(ce, module + layout->name, &name) && kernel_word(ce, module + layout->base, &base) && kernel_word(ce, module + layout->in_use, &in_use)) {
            read_name(ce, name, 0, entry->name, sizeof entry->name);
            entry->base = base & (MIPS_SLOT_SIZE - 1);
            entry->in_use = in_use;
            if (entry->name[0]) count++;
        }
        if (!kernel_word(ce, module + layout->next, &module)) break;
    }
    return count;
}
