#include "core/mips.h"

#include <string.h>

#define STATUS_IEC  (1u << 0)
#define STATUS_KUC  (1u << 1)
#define STATUS_BEV  (1u << 22)
#define STATUS_CU0  (1u << 28)

#define CAUSE_BD        (1u << 31)
#define CAUSE_CE_SHIFT  28
#define CAUSE_IP_MASK   0x0000FF00u
#define CAUSE_SW_MASK   0x00000300u
#define CAUSE_CODE_MASK 0x0000007Cu

#define ENTRYHI_VPN_MASK  0xFFFFF000u
#define ENTRYHI_PID_SHIFT 6
#define ENTRYHI_PID_MASK  0x3Fu
#define ENTRYLO_PFN_MASK  0xFFFFF000u
#define ENTRYLO_N         (1u << 11)
#define ENTRYLO_D         (1u << 10)
#define ENTRYLO_V         (1u << 9)
#define ENTRYLO_G         (1u << 8)
#define INDEX_PROBE_FAIL  (1u << 31)
#define INDEX_SHIFT       8
#define INDEX_MASK        0x1Fu
#define RANDOM_FIRST      8

#define CONTEXT_PTE_BASE  0xFFE00000u
#define CONTEXT_BAD_VPN   0x001FFFFCu

#define VEC_UTLB      0x80000000u
#define VEC_GENERAL   0x80000080u
#define VEC_UTLB_BEV  0xBFC00100u
#define VEC_GENERAL_BEV 0xBFC00180u

void mips_reset(mips_cpu_t *cpu, uint32_t entry) {
    mips_bus_t bus = cpu->bus;
    uint32_t speed = cpu->speed;
    mips_debug_t *debug = cpu->debug;
    bool (*on_break)(void *, uint32_t) = cpu->on_break;
    memset(cpu, 0, sizeof *cpu);
    cpu->bus = bus;
    cpu->debug = debug;
    cpu->on_break = on_break;
    cpu->speed = speed ? speed : 1;
    cpu->pc = entry;
    cpu->next_pc = entry + 4;
    cpu->cp0[CP0_STATUS] = STATUS_BEV;
    cpu->cp0[CP0_PRID] = 0x00002200u;
    cpu->random_state = 0x12345678u;
}

void mips_set_external_ip(mips_cpu_t *cpu, uint32_t ip_bits) {
    cpu->external_ip = ip_bits & CAUSE_IP_MASK & ~CAUSE_SW_MASK;
}

static uint32_t tlb_pid(const mips_cpu_t *cpu) {
    return (cpu->cp0[CP0_ENTRYHI] >> ENTRYHI_PID_SHIFT) & ENTRYHI_PID_MASK;
}

static int tlb_find(const mips_cpu_t *cpu, uint32_t vpn, uint32_t pid) {
    for (int i = 0; i < MIPS_TLB_ENTRIES; i++) {
        const mips_tlb_entry_t *entry = &cpu->tlb[i];
        if (entry->vpn == vpn && (entry->global || entry->pid == pid)) return i;
    }
    return -1;
}

static uint32_t tlb_random_index(mips_cpu_t *cpu) {
    cpu->random_state = cpu->random_state * 1103515245u + 12345u;
    return RANDOM_FIRST + (cpu->random_state >> 16) % (MIPS_TLB_ENTRIES - RANDOM_FIRST);
}

static void forget_page(mips_cpu_t *cpu, uint32_t vpn) {
    memset(&cpu->page_cache[(vpn >> 12) & (MIPS_PAGE_CACHE - 1)], 0, sizeof cpu->page_cache[0]);
    memset(&cpu->fetch_cache[(vpn >> 12) & (MIPS_FETCH_CACHE - 1)], 0, sizeof cpu->fetch_cache[0]);
}

static void tlb_write(mips_cpu_t *cpu, uint32_t index) {
    mips_tlb_entry_t *entry = &cpu->tlb[index & INDEX_MASK];
    uint32_t entrylo = cpu->cp0[CP0_ENTRYLO];
    forget_page(cpu, entry->vpn);
    entry->vpn = cpu->cp0[CP0_ENTRYHI] & ENTRYHI_VPN_MASK;
    entry->pid = tlb_pid(cpu);
    entry->pfn = entrylo & ENTRYLO_PFN_MASK;
    entry->global = (entrylo & ENTRYLO_G) != 0;
    entry->valid = (entrylo & ENTRYLO_V) != 0;
    entry->dirty = (entrylo & ENTRYLO_D) != 0;
    entry->noncache = (entrylo & ENTRYLO_N) != 0;
    forget_page(cpu, entry->vpn);
    cpu->epoch++;
}

static void tlb_read(mips_cpu_t *cpu) {
    cpu->epoch++;
    const mips_tlb_entry_t *entry = &cpu->tlb[(cpu->cp0[CP0_INDEX] >> INDEX_SHIFT) & INDEX_MASK];
    cpu->cp0[CP0_ENTRYHI] = entry->vpn | (entry->pid << ENTRYHI_PID_SHIFT);
    cpu->cp0[CP0_ENTRYLO] = entry->pfn
                            | (entry->noncache ? ENTRYLO_N : 0)
                            | (entry->dirty ? ENTRYLO_D : 0)
                            | (entry->valid ? ENTRYLO_V : 0)
                            | (entry->global ? ENTRYLO_G : 0);
}

static void tlb_probe(mips_cpu_t *cpu) {
    int index = tlb_find(cpu, cpu->cp0[CP0_ENTRYHI] & ENTRYHI_VPN_MASK, tlb_pid(cpu));
    if (index < 0) cpu->cp0[CP0_INDEX] |= INDEX_PROBE_FAIL;
    else cpu->cp0[CP0_INDEX] = (uint32_t)index << INDEX_SHIFT;
}

static void raise_exception(mips_cpu_t *cpu, uint32_t code, uint32_t faulting_pc, bool in_delay_slot) {
    cpu->exceptions[code & 15]++;
    if (cpu->debug && cpu->debug->exception && code != MIPS_EXC_INT && code != MIPS_EXC_SYS)
        cpu->debug->exception(cpu->debug->context, code, faulting_pc, (cpu->cp0[CP0_STATUS] & STATUS_KUC) != 0);
    uint32_t cause = cpu->cp0[CP0_CAUSE] & ~(CAUSE_BD | CAUSE_CODE_MASK | (3u << CAUSE_CE_SHIFT));
    cause |= code << 2;
    if (in_delay_slot) {
        cause |= CAUSE_BD;
        cpu->cp0[CP0_EPC] = faulting_pc - 4;
    } else {
        cpu->cp0[CP0_EPC] = faulting_pc;
    }
    cpu->cp0[CP0_CAUSE] = cause;
    uint32_t status = cpu->cp0[CP0_STATUS];
    cpu->cp0[CP0_STATUS] = (status & ~0x3Fu) | ((status << 2) & 0x3Cu);
    bool bev = (status & STATUS_BEV) != 0;
    bool utlb = (code == MIPS_EXC_TLBL || code == MIPS_EXC_TLBS) && cpu->cp0[CP0_BADVADDR] < 0x80000000u
                && tlb_find(cpu, cpu->cp0[CP0_BADVADDR] & ENTRYHI_VPN_MASK, tlb_pid(cpu)) < 0;
    uint32_t vector = bev ? (utlb ? VEC_UTLB_BEV : VEC_GENERAL_BEV) : (utlb ? VEC_UTLB : VEC_GENERAL);
    cpu->pc = vector;
    cpu->next_pc = vector + 4;
    cpu->next_in_delay_slot = false;
    cpu->fault = true;
    cpu->epoch++;
}

static void address_fault(mips_cpu_t *cpu, uint32_t code, uint32_t va) {
    cpu->cp0[CP0_BADVADDR] = va;
    raise_exception(cpu, code, cpu->current_pc, cpu->current_in_delay_slot);
}

static void tlb_fault(mips_cpu_t *cpu, uint32_t code, uint32_t va) {
    cpu->cp0[CP0_BADVADDR] = va;
    cpu->cp0[CP0_CONTEXT] = (cpu->cp0[CP0_CONTEXT] & CONTEXT_PTE_BASE) | ((va >> 10) & CONTEXT_BAD_VPN);
    cpu->cp0[CP0_ENTRYHI] = (cpu->cp0[CP0_ENTRYHI] & ~ENTRYHI_VPN_MASK) | (va & ENTRYHI_VPN_MASK);
    raise_exception(cpu, code, cpu->current_pc, cpu->current_in_delay_slot);
}

typedef enum { TRANSLATE_OK, TRANSLATE_ADDRESS, TRANSLATE_MISS, TRANSLATE_INVALID, TRANSLATE_MODIFIED } translate_result_t;

void mips_flush_translations(mips_cpu_t *cpu) {
    cpu->epoch++;
    memset(cpu->page_cache, 0, sizeof cpu->page_cache);
    memset(cpu->fetch_cache, 0, sizeof cpu->fetch_cache);
}

static translate_result_t translate(mips_cpu_t *cpu, uint32_t va, bool write, uint32_t *pa) {
    bool user = (cpu->cp0[CP0_STATUS] & STATUS_KUC) != 0;
    if (va >= 0x80000000u) {
        if (user) return TRANSLATE_ADDRESS;
        if (va < 0xA0000000u) { *pa = va - 0x80000000u; return TRANSLATE_OK; }
        if (va < 0xC0000000u) { *pa = va - 0xA0000000u; return TRANSLATE_OK; }
    }
    uint32_t vpn = va & ENTRYHI_VPN_MASK, pid = tlb_pid(cpu);
    uint32_t tag = vpn | pid << 1 | 1;
    mips_page_cache_t *cached = &cpu->page_cache[(va >> 12) & (MIPS_PAGE_CACHE - 1)];
    if (cached->tag == tag && (!write || cached->dirty)) {
        *pa = cached->pfn | (va & 0xFFFu);
        return TRANSLATE_OK;
    }
    int index = tlb_find(cpu, vpn, pid);
    if (index < 0) return TRANSLATE_MISS;
    const mips_tlb_entry_t *entry = &cpu->tlb[index];
    if (!entry->valid) return TRANSLATE_INVALID;
    if (write && !entry->dirty) return TRANSLATE_MODIFIED;
    *cached = (mips_page_cache_t){ tag, entry->pfn, entry->dirty };
    *pa = entry->pfn | (va & 0xFFFu);
    return TRANSLATE_OK;
}

void mips_jump(mips_cpu_t *cpu, uint32_t target) {
    cpu->pc = target;
    cpu->next_pc = target + 4;
    cpu->next_in_delay_slot = false;
    cpu->fault = true;
    cpu->epoch++;
}

void mips_return(mips_cpu_t *cpu, uint32_t value) {
    cpu->gpr[2] = value;
    mips_jump(cpu, cpu->gpr[31]);
}

bool mips_translate(mips_cpu_t *cpu, uint32_t va, bool write, uint32_t *pa) {
    return translate(cpu, va, write, pa) == TRANSLATE_OK;
}

static bool translate_or_fault(mips_cpu_t *cpu, uint32_t va, bool write, uint32_t *pa) {
    switch (translate(cpu, va, write, pa)) {
    case TRANSLATE_OK: return true;
    case TRANSLATE_ADDRESS: address_fault(cpu, write ? MIPS_EXC_ADES : MIPS_EXC_ADEL, va); return false;
    case TRANSLATE_MISS:
    case TRANSLATE_INVALID: tlb_fault(cpu, write ? MIPS_EXC_TLBS : MIPS_EXC_TLBL, va); return false;
    case TRANSLATE_MODIFIED: tlb_fault(cpu, MIPS_EXC_MOD, va); return false;
    }
    return false;
}

static inline uint32_t read_dram(const uint8_t *base, int size) {
    if (size == 4) {
        uint32_t word;
        memcpy(&word, base, 4);
        return word;
    }
    if (size == 2) {
        uint16_t half;
        memcpy(&half, base, 2);
        return half;
    }
    return base[0];
}

static inline void write_dram(uint8_t *base, int size, uint32_t value) {
    if (size == 4) {
        memcpy(base, &value, 4);
    } else if (size == 2) {
        uint16_t half = (uint16_t)value;
        memcpy(base, &half, 2);
    } else {
        base[0] = (uint8_t)value;
    }
}

static bool load(mips_cpu_t *cpu, uint32_t va, int size, uint32_t *value) {
    if (va & (uint32_t)(size - 1)) { address_fault(cpu, MIPS_EXC_ADEL, va); return false; }
    uint32_t pa;
    if (!translate_or_fault(cpu, va, false, &pa)) return false;
    if (cpu->debug && cpu->debug->data && cpu->debug->access(cpu->debug->context, va, size, false)) {
        cpu->debug->stop = true;
        cpu->debug->undo = true;
        return false;
    }
    if (pa < cpu->bus.dram_end) {
        *value = read_dram(cpu->bus.dram + (pa & cpu->bus.dram_mask), size);
        return true;
    }
    if (!cpu->bus.read(cpu->bus.context, pa, size, value)) {
        raise_exception(cpu, MIPS_EXC_DBE, cpu->current_pc, cpu->current_in_delay_slot);
        return false;
    }
    return true;
}

static bool store(mips_cpu_t *cpu, uint32_t va, int size, uint32_t value) {
    if (va & (uint32_t)(size - 1)) { address_fault(cpu, MIPS_EXC_ADES, va); return false; }
    uint32_t pa;
    if (!translate_or_fault(cpu, va, true, &pa)) return false;
    if (cpu->debug && cpu->debug->data && cpu->debug->access(cpu->debug->context, va, size, true)) {
        cpu->debug->stop = true;
        cpu->debug->undo = true;
        return false;
    }
    if (pa < cpu->bus.dram_end) {
        write_dram(cpu->bus.dram + (pa & cpu->bus.dram_mask), size, value);
        return true;
    }
    if (!cpu->bus.write(cpu->bus.context, pa, size, value)) {
        raise_exception(cpu, MIPS_EXC_DBE, cpu->current_pc, cpu->current_in_delay_slot);
        return false;
    }
    return true;
}

void mips_raise_tlb_miss(mips_cpu_t *cpu, uint32_t va) {
    tlb_fault(cpu, MIPS_EXC_TLBL, va);
}

void mips_raise_tlb_store_miss(mips_cpu_t *cpu, uint32_t va) {
    tlb_fault(cpu, MIPS_EXC_TLBS, va);
}

static bool page_watched(const mips_cpu_t *cpu, uint32_t pc) {
    for (int w = 0; w < cpu->watch_count; w++) {
        uint32_t va = cpu->watch[w];
        if ((va >> 12) == (pc >> 12)) return true;
        if (va < MIPS_SLOT_SIZE && pc < 0x80000000u && (va >> 12) == ((pc & (MIPS_SLOT_SIZE - 1)) >> 12)) return true;
    }
    return false;
}

static bool fetch(mips_cpu_t *cpu, uint32_t va, uint32_t *instruction) {
    if (va & 3) { address_fault(cpu, MIPS_EXC_ADEL, va); return false; }
    uint32_t user = (cpu->cp0[CP0_STATUS] & STATUS_KUC) != 0;
    uint32_t tag = (va & ENTRYHI_VPN_MASK) | tlb_pid(cpu) << 2 | user << 1 | 1;
    mips_fetch_cache_t *cached = &cpu->fetch_cache[(va >> 12) & (MIPS_FETCH_CACHE - 1)];
    if (cached->tag != tag) {
        uint32_t pa;
        if (!translate_or_fault(cpu, va, false, &pa)) return false;
        uint8_t *page = cpu->bus.fetch_page(cpu->bus.context, pa & ENTRYHI_VPN_MASK);
        if (!page) {
            if (!cpu->bus.read(cpu->bus.context, pa, 4, instruction)) {
                raise_exception(cpu, MIPS_EXC_IBE, cpu->current_pc, cpu->current_in_delay_slot);
                return false;
            }
            return true;
        }
        cached->tag = tag;
        cached->watched = page_watched(cpu, va);
        cached->page = page;
    }
    memcpy(instruction, cached->page + (va & 0xFFFu), 4);
    return true;
}

static void branch(mips_cpu_t *cpu, bool taken, uint32_t target) {
    if (taken) cpu->next_pc = target;
    cpu->next_in_delay_slot = true;
}

static void branch_likely(mips_cpu_t *cpu, bool taken, uint32_t target) {
    if (taken) {
        cpu->next_pc = target;
        cpu->next_in_delay_slot = true;
    } else {
        cpu->pc = cpu->next_pc;
        cpu->next_pc = cpu->pc + 4;
    }
}

static bool interrupt_pending(const mips_cpu_t *cpu) {
    uint32_t status = cpu->cp0[CP0_STATUS];
    if (!(status & STATUS_IEC)) return false;
    uint32_t ip = (cpu->cp0[CP0_CAUSE] & CAUSE_SW_MASK) | cpu->external_ip;
    return (ip & status & CAUSE_IP_MASK) != 0;
}

static bool coprocessor_usable(mips_cpu_t *cpu, int unit) {
    uint32_t status = cpu->cp0[CP0_STATUS];
    if (unit == 0 && !(status & STATUS_KUC)) return true;
    if (status & (1u << (28 + unit))) return true;
    raise_exception(cpu, MIPS_EXC_CPU, cpu->current_pc, cpu->current_in_delay_slot);
    cpu->cp0[CP0_CAUSE] = (cpu->cp0[CP0_CAUSE] & ~(3u << CAUSE_CE_SHIFT)) | ((uint32_t)unit << CAUSE_CE_SHIFT);
    return false;
}

static uint32_t read_cp0(mips_cpu_t *cpu, int reg) {
    switch (reg) {
    case CP0_RANDOM: return tlb_random_index(cpu) << INDEX_SHIFT;
    case CP0_CAUSE:  return (cpu->cp0[CP0_CAUSE] & ~(CAUSE_IP_MASK & ~CAUSE_SW_MASK)) | cpu->external_ip;
    default:         return cpu->cp0[reg];
    }
}

static void write_cp0(mips_cpu_t *cpu, int reg, uint32_t value) {
    cpu->epoch++;
    switch (reg) {
    case CP0_RANDOM:
    case CP0_BADVADDR:
    case CP0_PRID:
        return;
    case CP0_INDEX:
        cpu->cp0[reg] = (cpu->cp0[reg] & INDEX_PROBE_FAIL) | (value & (INDEX_MASK << INDEX_SHIFT));
        return;
    case CP0_ENTRYHI:
        cpu->cp0[reg] = value & (ENTRYHI_VPN_MASK | (ENTRYHI_PID_MASK << ENTRYHI_PID_SHIFT));
        break;
    case CP0_ENTRYLO:
        cpu->cp0[reg] = value & 0xFFFFFF00u;
        return;
    case CP0_CONTEXT:
        cpu->cp0[reg] = (value & CONTEXT_PTE_BASE) | (cpu->cp0[reg] & CONTEXT_BAD_VPN);
        return;
    case CP0_CAUSE:
        cpu->cp0[reg] = (cpu->cp0[reg] & ~CAUSE_SW_MASK) | (value & CAUSE_SW_MASK);
        return;
    default:
        cpu->cp0[reg] = value;
        break;
    }
}

static inline int32_t sign16(uint32_t value) {
    return (int16_t)(value & 0xFFFFu);
}

static void execute(mips_cpu_t *cpu, uint32_t op) {
    uint32_t *r = cpu->gpr;
    uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    uint32_t imm = op & 0xFFFFu;
    int32_t simm = sign16(op);
    uint32_t value;

    switch (op >> 26) {
    case 0x00:
        switch (op & 63) {
        case 0x00: r[rd] = r[rt] << sa; break;
        case 0x02: r[rd] = r[rt] >> sa; break;
        case 0x03: r[rd] = (uint32_t)((int32_t)r[rt] >> sa); break;
        case 0x04: r[rd] = r[rt] << (r[rs] & 31); break;
        case 0x06: r[rd] = r[rt] >> (r[rs] & 31); break;
        case 0x07: r[rd] = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31)); break;
        case 0x08: branch(cpu, true, r[rs]); break;
        case 0x09: { uint32_t target = r[rs]; r[rd] = cpu->current_pc + 8; branch(cpu, true, target); break; }
        case 0x0C: raise_exception(cpu, MIPS_EXC_SYS, cpu->current_pc, cpu->current_in_delay_slot); break;
        case 0x0D:
            if (cpu->on_break && cpu->on_break(cpu->bus.context, (op >> 6) & 0xFFFFFu)) break;
            raise_exception(cpu, MIPS_EXC_BP, cpu->current_pc, cpu->current_in_delay_slot);
            break;
        case 0x0F: break;
        case 0x10: r[rd] = cpu->hi; break;
        case 0x11: cpu->hi = r[rs]; break;
        case 0x12: r[rd] = cpu->lo; break;
        case 0x13: cpu->lo = r[rs]; break;
        case 0x18: { int64_t product = (int64_t)(int32_t)r[rs] * (int32_t)r[rt]; cpu->lo = (uint32_t)product; cpu->hi = (uint32_t)((uint64_t)product >> 32); if (rd) r[rd] = cpu->lo; break; }
        case 0x19: { uint64_t product = (uint64_t)r[rs] * r[rt]; cpu->lo = (uint32_t)product; cpu->hi = (uint32_t)(product >> 32); if (rd) r[rd] = cpu->lo; break; }
        case 0x1A: {
            int32_t numerator = (int32_t)r[rs], denominator = (int32_t)r[rt];
            if (denominator == 0) { cpu->lo = numerator < 0 ? 1 : 0xFFFFFFFFu; cpu->hi = (uint32_t)numerator; }
            else if (numerator == INT32_MIN && denominator == -1) { cpu->lo = (uint32_t)INT32_MIN; cpu->hi = 0; }
            else { cpu->lo = (uint32_t)(numerator / denominator); cpu->hi = (uint32_t)(numerator % denominator); }
            break;
        }
        case 0x1B:
            if (r[rt] == 0) { cpu->lo = 0xFFFFFFFFu; cpu->hi = r[rs]; }
            else { cpu->lo = r[rs] / r[rt]; cpu->hi = r[rs] % r[rt]; }
            break;
        case 0x20: {
            uint32_t sum = r[rs] + r[rt];
            if (~(r[rs] ^ r[rt]) & (r[rs] ^ sum) & 0x80000000u) raise_exception(cpu, MIPS_EXC_OV, cpu->current_pc, cpu->current_in_delay_slot);
            else r[rd] = sum;
            break;
        }
        case 0x21: r[rd] = r[rs] + r[rt]; break;
        case 0x22: {
            uint32_t difference = r[rs] - r[rt];
            if ((r[rs] ^ r[rt]) & (r[rs] ^ difference) & 0x80000000u) raise_exception(cpu, MIPS_EXC_OV, cpu->current_pc, cpu->current_in_delay_slot);
            else r[rd] = difference;
            break;
        }
        case 0x23: r[rd] = r[rs] - r[rt]; break;
        case 0x24: r[rd] = r[rs] & r[rt]; break;
        case 0x25: r[rd] = r[rs] | r[rt]; break;
        case 0x26: r[rd] = r[rs] ^ r[rt]; break;
        case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
        case 0x2A: r[rd] = (int32_t)r[rs] < (int32_t)r[rt]; break;
        case 0x2B: r[rd] = r[rs] < r[rt]; break;
        default: raise_exception(cpu, MIPS_EXC_RI, cpu->current_pc, cpu->current_in_delay_slot); break;
        }
        break;
    case 0x01: {
        uint32_t target = cpu->current_pc + 4 + ((uint32_t)simm << 2);
        bool less = (int32_t)r[rs] < 0;
        switch (rt) {
        case 0x00: branch(cpu, less, target); break;
        case 0x01: branch(cpu, !less, target); break;
        case 0x02: branch_likely(cpu, less, target); break;
        case 0x03: branch_likely(cpu, !less, target); break;
        case 0x10: r[31] = cpu->current_pc + 8; branch(cpu, less, target); break;
        case 0x11: r[31] = cpu->current_pc + 8; branch(cpu, !less, target); break;
        case 0x12: r[31] = cpu->current_pc + 8; branch_likely(cpu, less, target); break;
        case 0x13: r[31] = cpu->current_pc + 8; branch_likely(cpu, !less, target); break;
        default: raise_exception(cpu, MIPS_EXC_RI, cpu->current_pc, cpu->current_in_delay_slot); break;
        }
        break;
    }
    case 0x02: branch(cpu, true, ((cpu->current_pc + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2)); break;
    case 0x03: r[31] = cpu->current_pc + 8; branch(cpu, true, ((cpu->current_pc + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2)); break;
    case 0x04: branch(cpu, r[rs] == r[rt], cpu->current_pc + 4 + ((uint32_t)simm << 2)); break;
    case 0x05: branch(cpu, r[rs] != r[rt], cpu->current_pc + 4 + ((uint32_t)simm << 2)); break;
    case 0x06: branch(cpu, (int32_t)r[rs] <= 0, cpu->current_pc + 4 + ((uint32_t)simm << 2)); break;
    case 0x07: branch(cpu, (int32_t)r[rs] > 0, cpu->current_pc + 4 + ((uint32_t)simm << 2)); break;
    case 0x08: {
        uint32_t sum = r[rs] + (uint32_t)simm;
        if (~(r[rs] ^ (uint32_t)simm) & (r[rs] ^ sum) & 0x80000000u) raise_exception(cpu, MIPS_EXC_OV, cpu->current_pc, cpu->current_in_delay_slot);
        else r[rt] = sum;
        break;
    }
    case 0x09: r[rt] = r[rs] + (uint32_t)simm; break;
    case 0x0A: r[rt] = (int32_t)r[rs] < simm; break;
    case 0x0B: r[rt] = r[rs] < (uint32_t)simm; break;
    case 0x0C: r[rt] = r[rs] & imm; break;
    case 0x0D: r[rt] = r[rs] | imm; break;
    case 0x0E: r[rt] = r[rs] ^ imm; break;
    case 0x0F: r[rt] = imm << 16; break;
    case 0x10:
        if (!coprocessor_usable(cpu, 0)) break;
        if (op & (1u << 25)) {
            switch (op & 63) {
            case 0x01: tlb_read(cpu); break;
            case 0x02: tlb_write(cpu, (cpu->cp0[CP0_INDEX] >> INDEX_SHIFT) & INDEX_MASK); break;
            case 0x06: tlb_write(cpu, tlb_random_index(cpu)); break;
            case 0x08: tlb_probe(cpu); break;
            case 0x10:
                cpu->cp0[CP0_STATUS] = (cpu->cp0[CP0_STATUS] & ~0xFu) | ((cpu->cp0[CP0_STATUS] >> 2) & 0xFu);
                cpu->epoch++;
                break;
            default: raise_exception(cpu, MIPS_EXC_RI, cpu->current_pc, cpu->current_in_delay_slot); break;
            }
        } else {
            switch (rs) {
            case 0x00: if (rt) r[rt] = read_cp0(cpu, (int)rd); break;
            case 0x04: write_cp0(cpu, (int)rd, r[rt]); break;
            default: raise_exception(cpu, MIPS_EXC_RI, cpu->current_pc, cpu->current_in_delay_slot); break;
            }
        }
        break;
    case 0x11:
    case 0x12:
    case 0x13:
        coprocessor_usable(cpu, (int)((op >> 26) & 3));
        if (!cpu->fault) raise_exception(cpu, MIPS_EXC_RI, cpu->current_pc, cpu->current_in_delay_slot);
        break;
    case 0x14: branch_likely(cpu, r[rs] == r[rt], cpu->current_pc + 4 + ((uint32_t)simm << 2)); break;
    case 0x15: branch_likely(cpu, r[rs] != r[rt], cpu->current_pc + 4 + ((uint32_t)simm << 2)); break;
    case 0x16: branch_likely(cpu, (int32_t)r[rs] <= 0, cpu->current_pc + 4 + ((uint32_t)simm << 2)); break;
    case 0x17: branch_likely(cpu, (int32_t)r[rs] > 0, cpu->current_pc + 4 + ((uint32_t)simm << 2)); break;
    case 0x20: if (load(cpu, r[rs] + (uint32_t)simm, 1, &value)) r[rt] = (uint32_t)(int8_t)value; break;
    case 0x21: if (load(cpu, r[rs] + (uint32_t)simm, 2, &value)) r[rt] = (uint32_t)(int16_t)value; break;
    case 0x22: {
        uint32_t address = r[rs] + (uint32_t)simm;
        if (!load(cpu, address & ~3u, 4, &value)) break;
        int shift = (int)(3 - (address & 3)) * 8;
        uint32_t mask = 0xFFFFFFFFu << shift;
        r[rt] = (r[rt] & ~mask) | (value << shift);
        break;
    }
    case 0x23: if (load(cpu, r[rs] + (uint32_t)simm, 4, &value)) r[rt] = value; break;
    case 0x24: if (load(cpu, r[rs] + (uint32_t)simm, 1, &value)) r[rt] = value & 0xFFu; break;
    case 0x25: if (load(cpu, r[rs] + (uint32_t)simm, 2, &value)) r[rt] = value & 0xFFFFu; break;
    case 0x26: {
        uint32_t address = r[rs] + (uint32_t)simm;
        if (!load(cpu, address & ~3u, 4, &value)) break;
        int shift = (int)(address & 3) * 8;
        uint32_t mask = 0xFFFFFFFFu >> shift;
        r[rt] = (r[rt] & ~mask) | (value >> shift);
        break;
    }
    case 0x28: store(cpu, r[rs] + (uint32_t)simm, 1, r[rt] & 0xFFu); break;
    case 0x29: store(cpu, r[rs] + (uint32_t)simm, 2, r[rt] & 0xFFFFu); break;
    case 0x2A: {
        uint32_t address = r[rs] + (uint32_t)simm;
        if (!load(cpu, address & ~3u, 4, &value)) break;
        int shift = (int)(3 - (address & 3)) * 8;
        uint32_t mask = 0xFFFFFFFFu >> shift;
        store(cpu, address & ~3u, 4, (value & ~mask) | (r[rt] >> shift));
        break;
    }
    case 0x2B: store(cpu, r[rs] + (uint32_t)simm, 4, r[rt]); break;
    case 0x2E: {
        uint32_t address = r[rs] + (uint32_t)simm;
        if (!load(cpu, address & ~3u, 4, &value)) break;
        int shift = (int)(address & 3) * 8;
        uint32_t mask = 0xFFFFFFFFu << shift;
        store(cpu, address & ~3u, 4, (value & ~mask) | (r[rt] << shift));
        break;
    }
    case 0x2F: break;
    default: raise_exception(cpu, MIPS_EXC_RI, cpu->current_pc, cpu->current_in_delay_slot); break;
    }
    r[0] = 0;
}

static uint32_t watch_bit(uint32_t va) {
    return (va >> 2) & 4095;
}

void mips_debug_filter_add(mips_debug_t *debug, uint32_t va) {
    uint32_t bit = watch_bit(va);
    debug->filter[bit >> 5] |= 1u << (bit & 31);
}

bool mips_user_mode(const mips_cpu_t *cpu) {
    return (cpu->cp0[CP0_STATUS] & STATUS_KUC) != 0;
}

uint32_t mips_asid(const mips_cpu_t *cpu) {
    return tlb_pid(cpu);
}

static bool debug_stops_before(mips_cpu_t *cpu, uint32_t pc) {
    mips_debug_t *debug = cpu->debug;
    if (!debug->every) {
        uint32_t bit = watch_bit(pc);
        if (!(debug->filter[bit >> 5] >> (bit & 31) & 1)) return false;
    }
    return debug->before(debug->context, pc);
}

static void run_checked(mips_cpu_t *cpu, uint64_t until_cycle) {
    uint32_t speed = cpu->speed ? cpu->speed : 1;
    while (cpu->cycles < until_cycle && !cpu->yield) {
        if (++cpu->speed_count >= speed) {
            cpu->speed_count = 0;
            cpu->cycles++;
        }
        cpu->fault = false;
        cpu->current_pc = cpu->pc;
        cpu->current_in_delay_slot = cpu->next_in_delay_slot;
        if (interrupt_pending(cpu)) {
            raise_exception(cpu, MIPS_EXC_INT, cpu->current_pc, cpu->current_in_delay_slot);
            continue;
        }
        uint32_t instruction;
        if (!fetch(cpu, cpu->current_pc, &instruction)) continue;
        uint32_t bit = watch_bit(cpu->current_pc);
        for (int w = 0; (cpu->watch_filter[bit >> 5] >> (bit & 31) & 1) && w < cpu->watch_count; w++) {
            uint32_t va = cpu->watch[w];
            bool slot_relative = va < MIPS_SLOT_SIZE && cpu->current_pc < 0x80000000u;
            if (slot_relative ? (cpu->current_pc & (MIPS_SLOT_SIZE - 1)) == va : cpu->current_pc == va) cpu->on_watch(cpu->bus.context, cpu->current_pc);
        }
        if (cpu->fault) continue;
        if (cpu->debug) {
            if (debug_stops_before(cpu, cpu->current_pc)) {
                cpu->debug->stop = true;
                break;
            }
            cpu->debug->pc = cpu->current_pc;
        }
        uint32_t next_pc = cpu->next_pc;
        bool in_delay_slot = cpu->in_delay_slot;
        cpu->pc = cpu->next_pc;
        cpu->next_pc = cpu->pc + 4;
        cpu->next_in_delay_slot = false;
        cpu->in_delay_slot = cpu->current_in_delay_slot;
        execute(cpu, instruction);
        if (cpu->debug && cpu->debug->stop) {
            if (cpu->debug->undo) {
                cpu->debug->undo = false;
                cpu->pc = cpu->current_pc;
                cpu->next_pc = next_pc;
                cpu->next_in_delay_slot = cpu->current_in_delay_slot;
                cpu->in_delay_slot = in_delay_slot;
            }
            break;
        }
    }
}

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "guest memory is accessed as host words");

#define BUDGET_MAX 0x7FFFFFFFu
#define FLAG_DELAY     1u
#define FLAG_WAS_DELAY 2u
#define ALWAYS_INLINE  inline __attribute__((always_inline))

typedef struct {
    uint32_t pc, next_pc, flags;
} flow_t;

static void __attribute__((noinline)) sync_out(mips_cpu_t *cpu, const flow_t *flow, uint32_t current, bool current_delay, uint32_t budget) {
    uint32_t speed = cpu->speed ? cpu->speed : 1;
    uint32_t total = cpu->run_base_count + (cpu->run_base_budget - budget - cpu->run_stash);
    if (speed == 1) {
        cpu->cycles = cpu->run_base_cycles + total;
        cpu->speed_count = 0;
    } else {
        cpu->cycles = cpu->run_base_cycles + total / speed;
        cpu->speed_count = total % speed;
    }
    cpu->pc = flow->pc;
    cpu->next_pc = flow->next_pc;
    cpu->next_in_delay_slot = (flow->flags & FLAG_DELAY) != 0;
    cpu->in_delay_slot = (flow->flags & FLAG_WAS_DELAY) != 0;
    cpu->current_pc = current;
    cpu->current_in_delay_slot = current_delay;
    cpu->run_stash = 0;
    cpu->fault = false;
}

static uint32_t __attribute__((noinline)) sync_in(mips_cpu_t *cpu, flow_t *flow) {
    uint32_t speed = cpu->speed ? cpu->speed : 1;
    flow->pc = cpu->pc;
    flow->next_pc = cpu->next_pc;
    flow->flags = (cpu->next_in_delay_slot ? FLAG_DELAY : 0) | (cpu->in_delay_slot ? FLAG_WAS_DELAY : 0);
    cpu->run_base_cycles = cpu->cycles;
    cpu->run_base_count = cpu->speed_count;
    uint64_t budget = 0;
    if (cpu->cycles < cpu->run_until) budget = (cpu->run_until - cpu->cycles) * speed - cpu->speed_count;
    cpu->run_base_budget = budget > BUDGET_MAX ? BUDGET_MAX : (uint32_t)budget;
    cpu->run_stash = cpu->run_base_budget;
    return 0;
}

static ALWAYS_INLINE void flow_branch(flow_t *flow, bool taken, uint32_t target) {
    if (taken) flow->next_pc = target;
    flow->flags |= FLAG_DELAY;
}

static ALWAYS_INLINE void flow_branch_likely(flow_t *flow, bool taken, uint32_t target) {
    if (taken) {
        flow->next_pc = target;
        flow->flags |= FLAG_DELAY;
    } else {
        flow->pc = flow->next_pc;
        flow->next_pc = flow->pc + 4;
    }
}

static ALWAYS_INLINE bool fast_translate(const mips_cpu_t *cpu, uint32_t va, uint32_t size, bool write, uint32_t *pa) {
    if (va & (size - 1)) return false;
    if (va >= 0x80000000u) {
        if (cpu->cp0[CP0_STATUS] & STATUS_KUC) return false;
        if (va < 0xC0000000u) {
            *pa = va & 0x1FFFFFFFu;
            return true;
        }
    }
    uint32_t tag = (va & ENTRYHI_VPN_MASK) | tlb_pid(cpu) << 1 | 1;
    const mips_page_cache_t *cached = &cpu->page_cache[(va >> 12) & (MIPS_PAGE_CACHE - 1)];
    if (cached->tag != tag || (write && !cached->dirty)) return false;
    *pa = cached->pfn | (va & 0xFFFu);
    return true;
}

typedef enum { FAST_SLOW, FAST_DONE, FAST_BUS, FAST_SETTLE } fast_result_t;

typedef struct {
    uint32_t pa, size, value, reg;
    bool write, sign;
} bus_access_t;

static ALWAYS_INLINE fast_result_t fast_load(mips_cpu_t *cpu, uint32_t va, uint32_t size, uint32_t reg, bool sign, bus_access_t *access) {
    uint32_t pa;
    if (!fast_translate(cpu, va, size, false, &pa)) return FAST_SLOW;
    if (pa >= cpu->bus.dram_end) {
        *access = (bus_access_t){ pa, size, 0, reg, false, sign };
        return FAST_BUS;
    }
    uint32_t value = read_dram(cpu->bus.dram + (pa & cpu->bus.dram_mask), (int)size);
    if (sign) value = size == 1 ? (uint32_t)(int8_t)value : (uint32_t)(int16_t)value;
    cpu->gpr[reg] = value;
    return FAST_DONE;
}

static ALWAYS_INLINE fast_result_t fast_store(const mips_cpu_t *cpu, uint32_t va, uint32_t size, uint32_t value, bus_access_t *access) {
    uint32_t pa;
    if (!fast_translate(cpu, va, size, true, &pa)) return FAST_SLOW;
    if (pa >= cpu->bus.dram_end) {
        *access = (bus_access_t){ pa, size, value, 0, true, false };
        return FAST_BUS;
    }
    write_dram(cpu->bus.dram + (pa & cpu->bus.dram_mask), (int)size, value);
    return FAST_DONE;
}

static ALWAYS_INLINE fast_result_t execute_fast(mips_cpu_t *cpu, flow_t *hot, uint32_t pc, uint32_t op, bus_access_t *access) {
    uint32_t *r = cpu->gpr;
    uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
    uint32_t simm = (uint32_t)sign16(op);
    fast_result_t result = FAST_DONE;

    switch (op >> 26) {
    case 0x00:
        switch (op & 63) {
        case 0x00: r[rd] = r[rt] << ((op >> 6) & 31); break;
        case 0x02: r[rd] = r[rt] >> ((op >> 6) & 31); break;
        case 0x03: r[rd] = (uint32_t)((int32_t)r[rt] >> ((op >> 6) & 31)); break;
        case 0x04: r[rd] = r[rt] << (r[rs] & 31); break;
        case 0x06: r[rd] = r[rt] >> (r[rs] & 31); break;
        case 0x07: r[rd] = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31)); break;
        case 0x08: flow_branch(hot, true, r[rs]); break;
        case 0x09: { uint32_t target = r[rs]; r[rd] = pc + 8; flow_branch(hot, true, target); break; }
        case 0x10: r[rd] = cpu->hi; break;
        case 0x11: cpu->hi = r[rs]; break;
        case 0x12: r[rd] = cpu->lo; break;
        case 0x13: cpu->lo = r[rs]; break;
        case 0x18: { int64_t product = (int64_t)(int32_t)r[rs] * (int32_t)r[rt]; cpu->lo = (uint32_t)product; cpu->hi = (uint32_t)((uint64_t)product >> 32); if (rd) r[rd] = cpu->lo; break; }
        case 0x19: { uint64_t product = (uint64_t)r[rs] * r[rt]; cpu->lo = (uint32_t)product; cpu->hi = (uint32_t)(product >> 32); if (rd) r[rd] = cpu->lo; break; }
        case 0x1A: {
            int32_t numerator = (int32_t)r[rs], denominator = (int32_t)r[rt];
            if (denominator == 0) { cpu->lo = numerator < 0 ? 1 : 0xFFFFFFFFu; cpu->hi = (uint32_t)numerator; }
            else if (numerator == INT32_MIN && denominator == -1) { cpu->lo = (uint32_t)INT32_MIN; cpu->hi = 0; }
            else { cpu->lo = (uint32_t)(numerator / denominator); cpu->hi = (uint32_t)(numerator % denominator); }
            break;
        }
        case 0x1B:
            if (r[rt] == 0) { cpu->lo = 0xFFFFFFFFu; cpu->hi = r[rs]; }
            else { cpu->lo = r[rs] / r[rt]; cpu->hi = r[rs] % r[rt]; }
            break;
        case 0x20: {
            uint32_t sum = r[rs] + r[rt];
            if (~(r[rs] ^ r[rt]) & (r[rs] ^ sum) & 0x80000000u) return FAST_SLOW;
            r[rd] = sum;
            break;
        }
        case 0x21: r[rd] = r[rs] + r[rt]; break;
        case 0x22: {
            uint32_t difference = r[rs] - r[rt];
            if ((r[rs] ^ r[rt]) & (r[rs] ^ difference) & 0x80000000u) return FAST_SLOW;
            r[rd] = difference;
            break;
        }
        case 0x23: r[rd] = r[rs] - r[rt]; break;
        case 0x24: r[rd] = r[rs] & r[rt]; break;
        case 0x25: r[rd] = r[rs] | r[rt]; break;
        case 0x26: r[rd] = r[rs] ^ r[rt]; break;
        case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
        case 0x2A: r[rd] = (int32_t)r[rs] < (int32_t)r[rt]; break;
        case 0x2B: r[rd] = r[rs] < r[rt]; break;
        default: return FAST_SLOW;
        }
        break;
    case 0x01: {
        uint32_t target = pc + 4 + (simm << 2);
        bool less = (int32_t)r[rs] < 0;
        switch (rt) {
        case 0x00: flow_branch(hot, less, target); break;
        case 0x01: flow_branch(hot, !less, target); break;
        case 0x02: flow_branch_likely(hot, less, target); break;
        case 0x03: flow_branch_likely(hot, !less, target); break;
        case 0x10: r[31] = pc + 8; flow_branch(hot, less, target); break;
        case 0x11: r[31] = pc + 8; flow_branch(hot, !less, target); break;
        case 0x12: r[31] = pc + 8; flow_branch_likely(hot, less, target); break;
        case 0x13: r[31] = pc + 8; flow_branch_likely(hot, !less, target); break;
        default: return FAST_SLOW;
        }
        break;
    }
    case 0x02: flow_branch(hot, true, ((pc + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2)); break;
    case 0x03: r[31] = pc + 8; flow_branch(hot, true, ((pc + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2)); break;
    case 0x04: flow_branch(hot, r[rs] == r[rt], pc + 4 + (simm << 2)); break;
    case 0x05: flow_branch(hot, r[rs] != r[rt], pc + 4 + (simm << 2)); break;
    case 0x06: flow_branch(hot, (int32_t)r[rs] <= 0, pc + 4 + (simm << 2)); break;
    case 0x07: flow_branch(hot, (int32_t)r[rs] > 0, pc + 4 + (simm << 2)); break;
    case 0x08: {
        uint32_t sum = r[rs] + simm;
        if (~(r[rs] ^ simm) & (r[rs] ^ sum) & 0x80000000u) return FAST_SLOW;
        r[rt] = sum;
        break;
    }
    case 0x09: r[rt] = r[rs] + simm; break;
    case 0x0A: r[rt] = (int32_t)r[rs] < (int32_t)simm; break;
    case 0x0B: r[rt] = r[rs] < simm; break;
    case 0x0C: r[rt] = r[rs] & (op & 0xFFFFu); break;
    case 0x0D: r[rt] = r[rs] | (op & 0xFFFFu); break;
    case 0x0E: r[rt] = r[rs] ^ (op & 0xFFFFu); break;
    case 0x0F: r[rt] = op << 16; break;
    case 0x10: {
        uint32_t status = cpu->cp0[CP0_STATUS];
        if ((status & STATUS_KUC) && !(status & STATUS_CU0)) return FAST_SLOW;
        if (op & (1u << 25)) {
            switch (op & 63) {
            case 0x01: tlb_read(cpu); return FAST_SETTLE;
            case 0x02: tlb_write(cpu, (cpu->cp0[CP0_INDEX] >> INDEX_SHIFT) & INDEX_MASK); return FAST_SETTLE;
            case 0x06: tlb_write(cpu, tlb_random_index(cpu)); return FAST_SETTLE;
            case 0x08: tlb_probe(cpu); return FAST_DONE;
            case 0x10:
                cpu->cp0[CP0_STATUS] = (status & ~0xFu) | ((status >> 2) & 0xFu);
                cpu->epoch++;
                return FAST_SETTLE;
            default: return FAST_SLOW;
            }
        }
        if (rs == 0x00) {
            if (rt) r[rt] = read_cp0(cpu, (int)rd);
            r[0] = 0;
            return FAST_DONE;
        }
        if (rs == 0x04) {
            write_cp0(cpu, (int)rd, r[rt]);
            return FAST_SETTLE;
        }
        return FAST_SLOW;
    }
    case 0x14: flow_branch_likely(hot, r[rs] == r[rt], pc + 4 + (simm << 2)); break;
    case 0x15: flow_branch_likely(hot, r[rs] != r[rt], pc + 4 + (simm << 2)); break;
    case 0x16: flow_branch_likely(hot, (int32_t)r[rs] <= 0, pc + 4 + (simm << 2)); break;
    case 0x17: flow_branch_likely(hot, (int32_t)r[rs] > 0, pc + 4 + (simm << 2)); break;
    case 0x20: result = fast_load(cpu, r[rs] + simm, 1, rt, true, access); break;
    case 0x21: result = fast_load(cpu, r[rs] + simm, 2, rt, true, access); break;
    case 0x23: result = fast_load(cpu, r[rs] + simm, 4, rt, false, access); break;
    case 0x24: result = fast_load(cpu, r[rs] + simm, 1, rt, false, access); break;
    case 0x25: result = fast_load(cpu, r[rs] + simm, 2, rt, false, access); break;
    case 0x28: result = fast_store(cpu, r[rs] + simm, 1, r[rt] & 0xFFu, access); break;
    case 0x29: result = fast_store(cpu, r[rs] + simm, 2, r[rt] & 0xFFFFu, access); break;
    case 0x2B: result = fast_store(cpu, r[rs] + simm, 4, r[rt], access); break;
    default: return FAST_SLOW;
    }
    r[0] = 0;
    return result;
}

static void narrow_window(uint32_t pc, uint32_t watched, uint32_t *low, uint32_t *high) {
    if (watched < *low || watched >= *high) return;
    if (watched > pc) *high = watched;
    else *low = watched + 4;
}

static void window_bounds(const mips_cpu_t *cpu, uint32_t pc, uint32_t *low, uint32_t *high) {
    *low = pc & ENTRYHI_VPN_MASK;
    *high = *low + 0x1000u;
    for (int w = 0; w < cpu->watch_count; w++) {
        uint32_t va = cpu->watch[w];
        narrow_window(pc, va, low, high);
        if (va < MIPS_SLOT_SIZE && pc < 0x80000000u) narrow_window(pc, (pc & ~(MIPS_SLOT_SIZE - 1)) | va, low, high);
    }
}

static inline uint32_t rotate_right_2(uint32_t value) {
    return value >> 2 | value << 30;
}

static uint32_t fetch_tag(const mips_cpu_t *cpu, uint32_t va) {
    uint32_t user = (cpu->cp0[CP0_STATUS] & STATUS_KUC) != 0;
    return (va & ENTRYHI_VPN_MASK) | tlb_pid(cpu) << 2 | user << 1 | 1;
}

static bool run_watches(mips_cpu_t *cpu, uint32_t pc) {
    uint32_t bit = watch_bit(pc);
    bool faulted = false;
    for (int w = 0; (cpu->watch_filter[bit >> 5] >> (bit & 31) & 1) && w < cpu->watch_count; w++) {
        uint32_t va = cpu->watch[w];
        bool slot_relative = va < MIPS_SLOT_SIZE && pc < 0x80000000u;
        if (slot_relative ? (pc & (MIPS_SLOT_SIZE - 1)) == va : pc == va) {
            cpu->on_watch(cpu->bus.context, pc);
            faulted = faulted || cpu->fault;
        }
    }
    return !faulted;
}

static void run_fast(mips_cpu_t *cpu, uint64_t until_cycle) {
    flow_t flow;
    cpu->run_until = until_cycle;
    cpu->run_window_epoch = cpu->epoch - 1;
    uint32_t current = cpu->current_pc;
    uint32_t budget = sync_in(cpu, &flow);
    uint32_t window_low = 0, window_words = 0, tag_bits = 0;
    const uint8_t *window = NULL;
    for (;;) {
        if (budget == 0) {
            budget = cpu->run_stash;
            cpu->run_stash = 0;
            if (budget == 0) {
                sync_out(cpu, &flow, current, (flow.flags & FLAG_WAS_DELAY) != 0, 0);
                sync_in(cpu, &flow);
                budget = cpu->run_stash;
                cpu->run_stash = 0;
                if (budget == 0) break;
            }
            if (cpu->yield) break;
            if (cpu->epoch != cpu->run_window_epoch) {
                window_words = 0;
                tag_bits = fetch_tag(cpu, 0);
                cpu->run_window_epoch = cpu->epoch;
            }
            if (interrupt_pending(cpu)) {
                current = flow.pc;
                sync_out(cpu, &flow, current, (flow.flags & FLAG_DELAY) != 0, budget - 1);
                raise_exception(cpu, MIPS_EXC_INT, current, (flow.flags & FLAG_DELAY) != 0);
                budget = sync_in(cpu, &flow);
                continue;
            }
        }
        budget--;
        current = flow.pc;
        uint32_t instruction;
        uint32_t window_offset = current - window_low;
        const mips_fetch_cache_t *entry;
        if (rotate_right_2(window_offset) < window_words) {
            memcpy(&instruction, window + window_offset, 4);
        } else if (!(current & 3) && (entry = &cpu->fetch_cache[(current >> 12) & (MIPS_FETCH_CACHE - 1)])->tag == ((current & ENTRYHI_VPN_MASK) | tag_bits)
                   && !entry->watched) {
            window_low = current & ENTRYHI_VPN_MASK;
            window_words = 0x1000u / 4;
            window = entry->page;
            memcpy(&instruction, window + (current & 0xFFFu), 4);
        } else {
            sync_out(cpu, &flow, current, (flow.flags & FLAG_DELAY) != 0, budget);
            bool fetched = fetch(cpu, current, &instruction) && (!cpu->watch_count || run_watches(cpu, current));
            budget = sync_in(cpu, &flow);
            if (!fetched) continue;
            const mips_fetch_cache_t *cached = &cpu->fetch_cache[(current >> 12) & (MIPS_FETCH_CACHE - 1)];
            if (cached->tag == fetch_tag(cpu, current) && cached->page) {
                uint32_t high;
                window_bounds(cpu, current, &window_low, &high);
                window_words = high > window_low ? (high - window_low) / 4 : 0;
                window = cached->page + (window_low & 0xFFFu);
            }
        }
        flow.pc = flow.next_pc;
        flow.next_pc = flow.pc + 4;
        flow.flags = (flow.flags & FLAG_DELAY) << 1;
        bus_access_t access;
        fast_result_t result = execute_fast(cpu, &flow, current, instruction, &access);
        if (result == FAST_DONE) continue;
        if (result == FAST_SETTLE) {
            cpu->gpr[0] = 0;
            cpu->run_stash += budget;
            budget = 0;
            continue;
        }
        bool current_delay = (flow.flags & FLAG_WAS_DELAY) != 0;
        budget += cpu->run_stash;
        cpu->run_stash = 0;
        sync_out(cpu, &flow, current, current_delay, budget);
        uint64_t cycles_before = cpu->cycles;
        if (result == FAST_BUS) {
            uint32_t value = access.value;
            bool ok = access.write ? cpu->bus.write(cpu->bus.context, access.pa, (int)access.size, value)
                                   : cpu->bus.read(cpu->bus.context, access.pa, (int)access.size, &value);
            if (ok && !access.write) cpu->gpr[access.reg] = access.sign ? (access.size == 1 ? (uint32_t)(int8_t)value : (uint32_t)(int16_t)value) : value;
            cpu->gpr[0] = 0;
            if (ok && cpu->pc == flow.pc && cpu->cycles == cycles_before) {
                cpu->run_stash = budget;
                budget = 0;
                continue;
            }
            if (!ok) raise_exception(cpu, MIPS_EXC_DBE, current, current_delay);
        } else {
            execute(cpu, instruction);
        }
        budget = sync_in(cpu, &flow);
    }
    sync_out(cpu, &flow, current, (flow.flags & FLAG_WAS_DELAY) != 0, budget);
}

static void build_watch_filter(mips_cpu_t *cpu) {
    memset(cpu->watch_filter, 0, sizeof cpu->watch_filter);
    for (int w = 0; w < cpu->watch_count; w++) cpu->watch_filter[watch_bit(cpu->watch[w]) >> 5] |= 1u << (watch_bit(cpu->watch[w]) & 31);
}

void mips_watches_changed(mips_cpu_t *cpu) {
    build_watch_filter(cpu);
    mips_flush_translations(cpu);
}

void mips_run(mips_cpu_t *cpu, uint64_t until_cycle) {
    cpu->yield = false;
    build_watch_filter(cpu);
    if (cpu->debug) run_checked(cpu, until_cycle);
    else run_fast(cpu, until_cycle);
}
