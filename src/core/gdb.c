#include "core/gdb.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#include "core/ce.h"

#define PACKET_MAX         0x4000
#define BREAKPOINT_MAX     64
#define WATCHPOINT_MAX     16
#define FAULT_HISTORY      8
#define RUN_QUANTUM        (MACHINE_CLOCK_HZ / 100)
#define LIBRARY_CHECK_QUANTA 10
#define EXIT_CHECK_QUANTA  5
#define FILE_MAX           16
#define FILE_PATH_MAX      260
#define AGENT_TIMEOUT      (MACHINE_CLOCK_HZ * 5ull)
#define AGENT_PROBE_TIMEOUT (MACHINE_CLOCK_HZ * 2ull)
#define RUN_TIMEOUT        (MACHINE_CLOCK_HZ * 30ull)
#define AGENT_PING         1
#define AGENT_WRITE        2
#define AGENT_READ         3
#define AGENT_RUN          4
#define AGENT_KILL         5
#define AGENT_DELETE       7
#define AGENT_CREATE       1u
#define FILEIO_O_CREAT     0x200u
#define FILEIO_O_TRUNC     0x400u
#define FILEIO_ENOENT      2
#define FILEIO_EBADF       9
#define FILEIO_EACCES      13
#define FILEIO_EEXIST      17
#define FILEIO_EMFILE      24
#define FILEIO_ENOSPC      28
#define FILEIO_EUNKNOWN    9999
#define REGISTER_COUNT     72
#define REGISTER_SR        32
#define REGISTER_LO        33
#define REGISTER_HI        34
#define REGISTER_BADVADDR  35
#define REGISTER_CAUSE     36
#define REGISTER_PC        37
#define KSEG0              0x80000000u
#define ANY_PROCESS        (-1)

#define SIGNAL_INT  2
#define SIGNAL_ILL  4
#define SIGNAL_TRAP 5
#define SIGNAL_FPE  8
#define SIGNAL_BUS  10
#define SIGNAL_SEGV 11

static const char TARGET_XML[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "<architecture>mips</architecture>\n"
    "<feature name=\"org.gnu.gdb.mips.cpu\">\n"
    "<reg name=\"r0\" bitsize=\"32\" regnum=\"0\"/><reg name=\"r1\" bitsize=\"32\"/><reg name=\"r2\" bitsize=\"32\"/>"
    "<reg name=\"r3\" bitsize=\"32\"/><reg name=\"r4\" bitsize=\"32\"/><reg name=\"r5\" bitsize=\"32\"/><reg name=\"r6\" bitsize=\"32\"/>"
    "<reg name=\"r7\" bitsize=\"32\"/><reg name=\"r8\" bitsize=\"32\"/><reg name=\"r9\" bitsize=\"32\"/><reg name=\"r10\" bitsize=\"32\"/>"
    "<reg name=\"r11\" bitsize=\"32\"/><reg name=\"r12\" bitsize=\"32\"/><reg name=\"r13\" bitsize=\"32\"/><reg name=\"r14\" bitsize=\"32\"/>"
    "<reg name=\"r15\" bitsize=\"32\"/><reg name=\"r16\" bitsize=\"32\"/><reg name=\"r17\" bitsize=\"32\"/><reg name=\"r18\" bitsize=\"32\"/>"
    "<reg name=\"r19\" bitsize=\"32\"/><reg name=\"r20\" bitsize=\"32\"/><reg name=\"r21\" bitsize=\"32\"/><reg name=\"r22\" bitsize=\"32\"/>"
    "<reg name=\"r23\" bitsize=\"32\"/><reg name=\"r24\" bitsize=\"32\"/><reg name=\"r25\" bitsize=\"32\"/><reg name=\"r26\" bitsize=\"32\"/>"
    "<reg name=\"r27\" bitsize=\"32\"/><reg name=\"r28\" bitsize=\"32\"/><reg name=\"r29\" bitsize=\"32\"/><reg name=\"r30\" bitsize=\"32\"/>"
    "<reg name=\"r31\" bitsize=\"32\"/>\n"
    "<reg name=\"lo\" bitsize=\"32\" regnum=\"33\"/><reg name=\"hi\" bitsize=\"32\" regnum=\"34\"/>"
    "<reg name=\"pc\" bitsize=\"32\" regnum=\"37\"/>\n"
    "</feature>\n"
    "<feature name=\"org.gnu.gdb.mips.cp0\">\n"
    "<reg name=\"status\" bitsize=\"32\" regnum=\"32\"/><reg name=\"badvaddr\" bitsize=\"32\" regnum=\"35\"/>"
    "<reg name=\"cause\" bitsize=\"32\" regnum=\"36\"/>\n"
    "</feature>\n"
    "<feature name=\"org.gnu.gdb.mips.fpu\">\n"
    "<reg name=\"f0\" bitsize=\"32\" type=\"ieee_single\" regnum=\"38\"/><reg name=\"f1\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f2\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f3\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f4\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f5\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f6\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f7\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f8\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f9\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f10\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f11\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f12\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f13\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f14\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f15\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f16\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f17\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f18\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f19\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f20\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f21\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f22\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f23\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f24\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f25\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f26\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f27\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f28\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f29\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f30\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f31\" bitsize=\"32\" type=\"ieee_single\"/>\n"
    "<reg name=\"fcsr\" bitsize=\"32\" group=\"float\" regnum=\"70\"/><reg name=\"fir\" bitsize=\"32\" group=\"float\" regnum=\"71\"/>\n"
    "</feature>\n"
    "</target>\n";

typedef enum { STOP_NONE, STOP_SIGNAL, STOP_WATCH, STOP_LIBRARY } stop_kind_t;

typedef struct {
    uint32_t gpr[32];
    uint32_t lo, hi, status, badvaddr, cause, pc;
    uint32_t code;
    int      process;
} fault_t;

typedef struct {
    uint32_t address;
    uint32_t length;
    int      type;
} watchpoint_t;

struct gdb {
    machine_t   *machine;
    ce_t         ce;
    gdb_log_fn   log;
    mips_debug_t debug;
    int          listener;
    int          client;
    bool         no_ack;
    bool         halted;
    bool         killed;
    bool         catch_faults;
    bool         forward_output;
    bool         elf_libraries;
    uint32_t     library_hash;
    int          library_check;
    uint32_t     module_list_pa;
    bool         module_list_known;
    bool         module_added;
    uint32_t     new_module;
    uint32_t     new_module_entry_pa;
    bool         entry_written;
    uint32_t     entry_breaks[BREAKPOINT_MAX];
    int          entry_break_count;

    uint8_t  input[PACKET_MAX * 2];
    size_t   input_length;
    size_t   packet_length;

    bool     extended;
    bool     non_stop;
    bool     sync_reply;
    bool     inferior;
    int      agent_state;
    uint16_t agent_sequence;
    char     files[FILE_MAX][FILE_PATH_MAX];
    bool     file_open[FILE_MAX];
    uint16_t run_sequence;
    uint32_t run_status;
    uint32_t run_pid;
    bool     run_replied;
    bool     run_active;
    int      exit_check;

    uint32_t     breakpoints[BREAKPOINT_MAX];
    int          breakpoint_count;
    watchpoint_t watchpoints[WATCHPOINT_MAX];
    int          watchpoint_count;

    char process_name[CE_NAME_MAX];
    int  process;
    bool waiting_for_process;
    int  last_user_process;

    bool     stepping;
    bool     step_executed;
    uint32_t step_pc;
    int      step_process;
    bool     step_user;

    bool     resume_skip;
    bool     watch_skip;
    uint32_t resume_pc;
    int      resume_process;

    stop_kind_t stop_kind;
    int         stop_signal;
    uint32_t    stop_address;
    int         stop_watch_type;

    fault_t faults[FAULT_HISTORY];
    int     fault_next;
    bool    post_mortem;
    fault_t post_mortem_fault;
};

static void logf_gdb(gdb_t *gdb, const char *format, ...) {
    if (!gdb->log) return;
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    gdb->log(message);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char HEX[] = "0123456789abcdef";

static void hex_bytes(char *out, const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        out[i * 2] = HEX[data[i] >> 4];
        out[i * 2 + 1] = HEX[data[i] & 15];
    }
    out[length * 2] = 0;
}

static size_t unhex_bytes(uint8_t *out, const char *text, size_t max) {
    size_t length = 0;
    while (length < max && hex_value(text[0]) >= 0 && hex_value(text[1]) >= 0) {
        out[length++] = (uint8_t)(hex_value(text[0]) << 4 | hex_value(text[1]));
        text += 2;
    }
    return length;
}

static uint32_t parse_hex(const char **text) {
    uint32_t value = 0;
    while (hex_value(**text) >= 0) {
        value = value << 4 | (uint32_t)hex_value(**text);
        (*text)++;
    }
    return value;
}

static void hex_word(char *out, uint32_t value) {
    uint8_t bytes[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    hex_bytes(out, bytes, 4);
}

static uint32_t unhex_word(const char *text) {
    uint8_t bytes[4] = { 0 };
    unhex_bytes(bytes, text, 4);
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void close_client(gdb_t *gdb) {
    if (gdb->client >= 0) close(gdb->client);
    gdb->client = -1;
    gdb->input_length = 0;
}

static bool send_all(gdb_t *gdb, const char *data, size_t length) {
    while (length) {
        ssize_t sent = send(gdb->client, data, length, SEND_FLAGS);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) {
            close_client(gdb);
            return false;
        }
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool send_packet_length(gdb_t *gdb, const char *payload, size_t payload_length) {
    if (gdb->client < 0) return false;
    static char frame[PACKET_MAX * 2 + 8];
    size_t length = 0;
    uint8_t checksum = 0;
    frame[length++] = '$';
    for (size_t i = 0; i < payload_length && length < sizeof frame - 4; i++) {
        frame[length++] = payload[i];
        checksum += (uint8_t)payload[i];
    }
    frame[length++] = '#';
    frame[length++] = HEX[checksum >> 4];
    frame[length++] = HEX[checksum & 15];
    return send_all(gdb, frame, length);
}

static bool send_packet(gdb_t *gdb, const char *payload) {
    return send_packet_length(gdb, payload, strlen(payload));
}

static void send_console(gdb_t *gdb, const char *text) {
    char payload[PACKET_MAX];
    size_t length = strlen(text);
    if (length > (sizeof payload - 2) / 2) length = (sizeof payload - 2) / 2;
    payload[0] = 'O';
    hex_bytes(payload + 1, (const uint8_t *)text, length);
    send_packet(gdb, payload);
}

static int fault_signal(uint32_t code) {
    switch (code) {
    case MIPS_EXC_MOD:
    case MIPS_EXC_TLBL:
    case MIPS_EXC_TLBS:
    case MIPS_EXC_ADEL:
    case MIPS_EXC_ADES: return SIGNAL_SEGV;
    case MIPS_EXC_IBE:
    case MIPS_EXC_DBE: return SIGNAL_BUS;
    case MIPS_EXC_RI:
    case MIPS_EXC_CPU: return SIGNAL_ILL;
    case MIPS_EXC_OV: return SIGNAL_FPE;
    default: return SIGNAL_TRAP;
    }
}

static int current_process(gdb_t *gdb) {
    return ce_current_process(&gdb->ce);
}

static int memory_process(gdb_t *gdb) {
    if (gdb->post_mortem) return gdb->post_mortem_fault.process;
    return gdb->process >= 0 ? gdb->process : CE_CURRENT;
}

static bool split_address(uint32_t va, int process, int *owner, uint32_t *offset) {
    if (va >= KSEG0) return false;
    if (va < MIPS_SLOT_SIZE) {
        *owner = process;
        *offset = va;
    } else {
        *owner = (int)(va / MIPS_SLOT_SIZE) - 1;
        *offset = va & (MIPS_SLOT_SIZE - 1);
    }
    return true;
}

static bool address_matches(gdb_t *gdb, uint32_t target, uint32_t va, int process) {
    int target_owner, owner;
    uint32_t target_offset, offset;
    if (!split_address(va, process, &owner, &offset) || !split_address(target, ANY_PROCESS, &target_owner, &target_offset)) return target == va;
    if (offset != target_offset) return false;
    if (target_owner != ANY_PROCESS) return target_owner == owner;
    return gdb->process < 0 || owner == gdb->process;
}

static uint32_t current_library_hash(gdb_t *gdb);

static void update_debug(gdb_t *gdb) {
    memset(gdb->debug.filter, 0, sizeof gdb->debug.filter);
    for (int i = 0; i < gdb->breakpoint_count; i++) mips_debug_filter_add(&gdb->debug, gdb->breakpoints[i]);
    for (int i = 0; i < gdb->entry_break_count; i++) mips_debug_filter_add(&gdb->debug, gdb->entry_breaks[i]);
    gdb->debug.every = gdb->stepping || gdb->resume_skip || gdb->waiting_for_process || gdb->module_added || gdb->entry_written;
    gdb->debug.data = gdb->watchpoint_count > 0 || (gdb->client >= 0 && gdb->module_list_known);
}

static void request_stop(gdb_t *gdb, stop_kind_t kind, int signal) {
    gdb->stop_kind = kind;
    gdb->stop_signal = signal;
    gdb->debug.stop = true;
}

static bool on_before(void *context, uint32_t pc) {
    gdb_t *gdb = context;
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    int process = current_process(gdb);
    bool user = mips_user_mode(cpu);
    if (gdb->resume_skip && pc == gdb->resume_pc && process == gdb->resume_process) {
        gdb->resume_skip = false;
        update_debug(gdb);
        if (!gdb->stepping) return false;
    }
    if (gdb->module_added) {
        gdb->module_added = false;
        if (ce_first_module(&gdb->ce, &gdb->new_module)) gdb->new_module_entry_pa = ce_module_entry_field(&gdb->ce, gdb->new_module);
        else gdb->new_module = 0;
        update_debug(gdb);
    }
    if (gdb->entry_written) {
        gdb->entry_written = false;
        uint32_t entry;
        if (gdb->new_module && ce_module_entry(&gdb->ce, gdb->new_module, &entry)) {
            if (gdb->entry_break_count == BREAKPOINT_MAX) memmove(gdb->entry_breaks, gdb->entry_breaks + 1, sizeof gdb->entry_breaks - sizeof gdb->entry_breaks[0]);
            else gdb->entry_break_count++;
            gdb->entry_breaks[gdb->entry_break_count - 1] = entry;
            gdb->new_module = 0;
    gdb->entry_written = false;
        }
        update_debug(gdb);
    }
    for (int i = 0; i < gdb->entry_break_count; i++) {
        if (!address_matches(gdb, gdb->entry_breaks[i], pc, process)) continue;
        gdb->entry_breaks[i] = gdb->entry_breaks[--gdb->entry_break_count];
        update_debug(gdb);
        if (current_library_hash(gdb) == gdb->library_hash) break;
        gdb->stop_kind = STOP_LIBRARY;
        return true;
    }
    if (gdb->waiting_for_process && user && process != gdb->last_user_process) {
        gdb->last_user_process = process;
        char name[CE_NAME_MAX];
        if (ce_process_name(&gdb->ce, process, name, sizeof name) && !strcasecmp(name, gdb->process_name)) {
            gdb->process = process;
            gdb->waiting_for_process = false;
            update_debug(gdb);
            gdb->stop_kind = STOP_SIGNAL;
            gdb->stop_signal = SIGNAL_TRAP;
            logf_gdb(gdb, "gdb: %s started as process %d\n", name, process);
            return true;
        }
    }
    if (gdb->stepping) {
        if (!gdb->step_executed) {
            if (pc == gdb->step_pc && process == gdb->step_process) gdb->step_executed = true;
            return false;
        }
        if (user == gdb->step_user && (!user || process == gdb->step_process)) {
            gdb->stop_kind = STOP_SIGNAL;
            gdb->stop_signal = SIGNAL_TRAP;
            return true;
        }
    }
    for (int i = 0; i < gdb->breakpoint_count; i++) {
        if (!address_matches(gdb, gdb->breakpoints[i], pc, process)) continue;
        gdb->stop_kind = STOP_SIGNAL;
        gdb->stop_signal = SIGNAL_TRAP;
        return true;
    }
    return false;
}

static bool on_access(void *context, uint32_t va, int size, bool write) {
    gdb_t *gdb = context;
    uint32_t pa;
    if (write && gdb->module_list_known && (va & 0xFFFu) == (gdb->module_list_pa & 0xFFFu) &&
        ce_translate(&gdb->ce, va, CE_CURRENT, false, &pa) && pa == gdb->module_list_pa) {
        gdb->module_added = true;
        gdb->debug.every = true;
    }
    if (write && gdb->new_module && va >= KSEG0 && va < 0xC0000000u && (va & 0x1FFFFFFFu) == gdb->new_module_entry_pa) {
        gdb->entry_written = true;
        gdb->debug.every = true;
    }
    if (!gdb->watchpoint_count) return false;
    int process = current_process(gdb);
    if (gdb->watch_skip) {
        if (gdb->debug.pc == gdb->resume_pc && process == gdb->resume_process) {
            gdb->watch_skip = false;
            return false;
        }
    }
    for (int i = 0; i < gdb->watchpoint_count; i++) {
        const watchpoint_t *watch = &gdb->watchpoints[i];
        if (watch->type == 2 && !write) continue;
        if (watch->type == 3 && write) continue;
        for (int byte = 0; byte < size; byte++) {
            uint32_t address = va + (uint32_t)byte;
            bool hit = false;
            for (uint32_t offset = 0; offset < watch->length && !hit; offset++) hit = address_matches(gdb, watch->address + offset, address, process);
            if (!hit) continue;
            gdb->stop_address = watch->address;
            gdb->stop_watch_type = watch->type;
            gdb->stop_kind = STOP_WATCH;
            gdb->stop_signal = SIGNAL_TRAP;
            return true;
        }
    }
    return false;
}

static void on_exception(void *context, uint32_t code, uint32_t pc, bool user) {
    gdb_t *gdb = context;
    if (!user) return;
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    fault_t *fault = &gdb->faults[gdb->fault_next];
    gdb->fault_next = (gdb->fault_next + 1) % FAULT_HISTORY;
    memcpy(fault->gpr, cpu->gpr, sizeof fault->gpr);
    fault->lo = cpu->lo;
    fault->hi = cpu->hi;
    fault->status = cpu->cp0[CP0_STATUS];
    fault->badvaddr = cpu->cp0[CP0_BADVADDR];
    fault->cause = (cpu->cp0[CP0_CAUSE] & ~0x7Cu) | code << 2;
    fault->pc = pc;
    fault->code = code;
    fault->process = current_process(gdb);
}

void gdb_debug_line(gdb_t *gdb, const char *line) {
    if (gdb->client < 0) return;
    if (gdb->forward_output && !gdb->halted && gdb->inferior && !gdb->non_stop) {
        char text[600];
        snprintf(text, sizeof text, "%s\n", line);
        send_console(gdb, text);
    }
    unsigned code;
    if (!gdb->catch_faults || sscanf(line, "Exception %u", &code) != 1) return;
    for (int back = 1; back <= FAULT_HISTORY; back++) {
        const fault_t *fault = &gdb->faults[(gdb->fault_next + FAULT_HISTORY - back) % FAULT_HISTORY];
        if (fault->code != code || (!fault->pc && !fault->badvaddr)) continue;
        if (gdb->process >= 0 && fault->process != gdb->process) return;
        gdb->post_mortem_fault = *fault;
        gdb->post_mortem = true;
        gdb->faults[(gdb->fault_next + FAULT_HISTORY - back) % FAULT_HISTORY] = (fault_t){ 0 };
        request_stop(gdb, STOP_SIGNAL, fault_signal(code));
        return;
    }
}

static uint32_t read_register(gdb_t *gdb, int number) {
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    if (gdb->post_mortem) {
        const fault_t *fault = &gdb->post_mortem_fault;
        if (number < 32) return fault->gpr[number];
        switch (number) {
        case REGISTER_SR: return fault->status;
        case REGISTER_LO: return fault->lo;
        case REGISTER_HI: return fault->hi;
        case REGISTER_BADVADDR: return fault->badvaddr;
        case REGISTER_CAUSE: return fault->cause;
        case REGISTER_PC: return fault->pc;
        default: return 0;
        }
    }
    if (number < 32) return cpu->gpr[number];
    switch (number) {
    case REGISTER_SR: return cpu->cp0[CP0_STATUS];
    case REGISTER_LO: return cpu->lo;
    case REGISTER_HI: return cpu->hi;
    case REGISTER_BADVADDR: return cpu->cp0[CP0_BADVADDR];
    case REGISTER_CAUSE: return cpu->cp0[CP0_CAUSE];
    case REGISTER_PC: return cpu->pc;
    default: return 0;
    }
}

static void write_register(gdb_t *gdb, int number, uint32_t value) {
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    if (gdb->post_mortem) return;
    if (number > 0 && number < 32) cpu->gpr[number] = value;
    else if (number == REGISTER_LO) cpu->lo = value;
    else if (number == REGISTER_HI) cpu->hi = value;
    else if (number == REGISTER_PC && value != cpu->pc) {
        cpu->pc = value;
        cpu->next_pc = value + 4;
        cpu->next_in_delay_slot = false;
        mips_flush_translations(cpu);
    }
}

static void monitor_reply(gdb_t *gdb, const char *format, ...) {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    send_console(gdb, text);
}

static int library_process(gdb_t *gdb) {
    return gdb->process;
}

static void library_file_name(gdb_t *gdb, const char *name, char *file, size_t size) {
    snprintf(file, size, "%s", name);
    char *extension = strrchr(file, '.');
    if (gdb->elf_libraries && extension && !strcasecmp(extension, ".dll")) snprintf(extension, size - (size_t)(extension - file), ".elf");
}

static size_t library_list(gdb_t *gdb, char *xml, size_t size) {
    static ce_module_t modules[CE_MODULE_MAX];
    int count = ce_modules(&gdb->ce, modules, CE_MODULE_MAX), process = library_process(gdb);
    size_t length = (size_t)snprintf(xml, size, "<library-list>\n");
    for (int i = 0; i < count && length < size; i++) {
        if (process >= 0 && process < CE_PROCESS_MAX && !(modules[i].in_use >> process & 1)) continue;
        char file[CE_NAME_MAX + 8];
        library_file_name(gdb, modules[i].name, file, sizeof file);
        length += (size_t)snprintf(xml + length, size - length, "<library name=\"%s\"><segment address=\"0x%08x\"/></library>\n", file,
                                   modules[i].base);
    }
    if (length < size) length += (size_t)snprintf(xml + length, size - length, "</library-list>\n");
    return length < size ? length : size - 1;
}

static uint32_t current_library_hash(gdb_t *gdb) {
    static char xml[CE_MODULE_MAX * 128 + 64];
    size_t length = library_list(gdb, xml, sizeof xml);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) hash = (hash ^ (uint8_t)xml[i]) * 16777619u;
    return hash;
}

static void build_stop_reply(gdb_t *gdb, char *reply, size_t size) {
    int signal = gdb->stop_signal ? gdb->stop_signal : SIGNAL_TRAP;
    const char *thread = gdb->non_stop ? "thread:1;" : "";
    if (gdb->stop_kind == STOP_LIBRARY) {
        gdb->library_hash = current_library_hash(gdb);
        snprintf(reply, size, "T%02x%slibrary:;", SIGNAL_TRAP, thread);
    } else if (gdb->stop_kind == STOP_WATCH) {
        const char *kind = gdb->stop_watch_type == 3 ? "rwatch" : gdb->stop_watch_type == 4 ? "awatch" : "watch";
        snprintf(reply, size, "T%02x%s%s:%08x;", signal, thread, kind, gdb->stop_address);
    } else {
        snprintf(reply, size, "T%02x%s", signal, thread);
    }
}

static void send_notification(gdb_t *gdb, const char *payload) {
    if (gdb->client < 0) return;
    char frame[256];
    uint8_t checksum = 0;
    size_t length = (size_t)snprintf(frame, sizeof frame, "%%Stop:%s", payload);
    for (size_t i = 1; i < length; i++) checksum += (uint8_t)frame[i];
    length += (size_t)snprintf(frame + length, sizeof frame - length, "#%c%c", HEX[checksum >> 4], HEX[checksum & 15]);
    send_all(gdb, frame, length);
}

static void send_async_stop(gdb_t *gdb, const char *payload) {
    if (gdb->non_stop && !gdb->sync_reply) send_notification(gdb, payload);
    else send_packet(gdb, payload);
}

static void send_stop_reply(gdb_t *gdb) {
    char reply[96];
    build_stop_reply(gdb, reply, sizeof reply);
    send_async_stop(gdb, reply);
}

static void monitor_modules(gdb_t *gdb) {
    static ce_module_t modules[CE_MODULE_MAX];
    int count = ce_modules(&gdb->ce, modules, CE_MODULE_MAX), process = library_process(gdb);
    if (!count) {
        monitor_reply(gdb, "CE's module list isn't set up yet\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        bool used = process >= 0 && process < CE_PROCESS_MAX && (modules[i].in_use >> process & 1);
        char file[CE_NAME_MAX + 8];
        library_file_name(gdb, modules[i].name, file, sizeof file);
        monitor_reply(gdb, "%c %08x  %-16s add-symbol-file %s -o 0x%x\n", used ? '*' : ' ', modules[i].base, modules[i].name, file,
                      modules[i].base - 0x10000u);
    }
}

static void halt(gdb_t *gdb) {
    gdb->halted = true;
    gdb->stepping = false;
    gdb->resume_skip = false;
    update_debug(gdb);
    if (gdb->post_mortem && (!gdb->non_stop || gdb->sync_reply)) {
        char name[CE_NAME_MAX] = "";
        ce_process_name(&gdb->ce, gdb->post_mortem_fault.process, name, sizeof name);
        char text[200];
        snprintf(text, sizeof text, "velo: exception %u in %s at %08x, badvaddr %08x\n", gdb->post_mortem_fault.code, name[0] ? name : "a process",
                 gdb->post_mortem_fault.pc, gdb->post_mortem_fault.badvaddr);
        send_console(gdb, text);
    }
    send_stop_reply(gdb);
}

static void resume(gdb_t *gdb, bool step) {
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    gdb->watch_skip = gdb->stop_kind == STOP_WATCH;
    gdb->halted = false;
    gdb->post_mortem = false;
    gdb->stop_kind = STOP_NONE;
    gdb->stop_signal = 0;
    gdb->debug.stop = false;
    gdb->resume_pc = cpu->pc;
    gdb->resume_process = current_process(gdb);
    gdb->resume_skip = false;
    for (int i = 0; i < gdb->breakpoint_count && !gdb->resume_skip; i++)
        gdb->resume_skip = address_matches(gdb, gdb->breakpoints[i], cpu->pc, gdb->resume_process);
    gdb->stepping = step;
    if (step) {
        gdb->step_executed = false;
        gdb->step_pc = cpu->pc;
        gdb->step_process = gdb->resume_process;
        gdb->step_user = mips_user_mode(cpu);
    }
    update_debug(gdb);
}

static bool add_breakpoint(gdb_t *gdb, uint32_t address) {
    for (int i = 0; i < gdb->breakpoint_count; i++) {
        if (gdb->breakpoints[i] == address) return true;
    }
    if (gdb->breakpoint_count == BREAKPOINT_MAX) return false;
    gdb->breakpoints[gdb->breakpoint_count++] = address;
    return true;
}

static void remove_breakpoint(gdb_t *gdb, uint32_t address) {
    for (int i = 0; i < gdb->breakpoint_count; i++) {
        if (gdb->breakpoints[i] != address) continue;
        gdb->breakpoints[i] = gdb->breakpoints[--gdb->breakpoint_count];
        return;
    }
}

static bool add_watchpoint(gdb_t *gdb, uint32_t address, uint32_t length, int type) {
    if (gdb->watchpoint_count == WATCHPOINT_MAX || !length) return false;
    gdb->watchpoints[gdb->watchpoint_count++] = (watchpoint_t){ address, length, type };
    return true;
}

static void remove_watchpoint(gdb_t *gdb, uint32_t address, uint32_t length, int type) {
    for (int i = 0; i < gdb->watchpoint_count; i++) {
        const watchpoint_t *watch = &gdb->watchpoints[i];
        if (watch->address != address || watch->length != length || watch->type != type) continue;
        gdb->watchpoints[i] = gdb->watchpoints[--gdb->watchpoint_count];
        return;
    }
}

static void handle_breakpoint_packet(gdb_t *gdb, const char *packet) {
    bool insert = packet[0] == 'Z';
    const char *cursor = packet + 1;
    int type = (int)parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t address = parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t kind = parse_hex(&cursor);
    bool ok = true;
    if (type == 0 || type == 1) {
        if (insert) ok = add_breakpoint(gdb, address);
        else remove_breakpoint(gdb, address);
    } else if (type >= 2 && type <= 4) {
        if (insert) ok = add_watchpoint(gdb, address, kind, type);
        else remove_watchpoint(gdb, address, kind, type);
    } else {
        send_packet(gdb, "");
        return;
    }
    update_debug(gdb);
    send_packet(gdb, ok ? "OK" : "E0e");
}

static void handle_read_memory(gdb_t *gdb, const char *packet) {
    const char *cursor = packet + 1;
    uint32_t address = parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t length = parse_hex(&cursor);
    if (length > PACKET_MAX / 2 - 8) length = PACKET_MAX / 2 - 8;
    static uint8_t data[PACKET_MAX];
    uint32_t done = 0;
    while (done < length && ce_read(&gdb->ce, address + done, memory_process(gdb), data + done, 1)) done++;
    if (!done && length) { send_packet(gdb, "E14"); return; }
    static char reply[PACKET_MAX + 1];
    hex_bytes(reply, data, done);
    send_packet(gdb, reply);
}

static void handle_write_memory(gdb_t *gdb, const char *packet) {
    const char *cursor = packet + 1;
    uint32_t address = parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t length = parse_hex(&cursor);
    if (*cursor++ != ':') { send_packet(gdb, "E01"); return; }
    static uint8_t data[PACKET_MAX];
    if (length > sizeof data || unhex_bytes(data, cursor, length) != length) { send_packet(gdb, "E01"); return; }
    send_packet(gdb, ce_write(&gdb->ce, address, memory_process(gdb), data, length) ? "OK" : "E14");
}

static void handle_read_registers(gdb_t *gdb) {
    char reply[REGISTER_COUNT * 8 + 1];
    for (int i = 0; i < REGISTER_COUNT; i++) hex_word(reply + i * 8, read_register(gdb, i));
    send_packet(gdb, reply);
}

static void handle_write_registers(gdb_t *gdb, const char *packet) {
    const char *values = packet + 1;
    for (int i = 0; i < REGISTER_COUNT && strlen(values) >= 8; i++, values += 8) write_register(gdb, i, unhex_word(values));
    send_packet(gdb, "OK");
}

static void handle_xfer(gdb_t *gdb, const char *packet, const char *object, const char *document, size_t document_length) {
    char prefix[64];
    snprintf(prefix, sizeof prefix, "qXfer:%s:read:", object);
    const char *cursor = packet + strlen(prefix);
    while (*cursor && *cursor != ':') cursor++;
    if (*cursor++ != ':') { send_packet(gdb, "E01"); return; }
    uint32_t offset = parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t length = parse_hex(&cursor);
    if (length > PACKET_MAX / 2) length = PACKET_MAX / 2;
    static char reply[PACKET_MAX + 2];
    if (offset >= document_length) {
        send_packet(gdb, "l");
        return;
    }
    size_t available = document_length - offset, take = available < length ? available : length, used = 0;
    reply[used++] = take == available ? 'l' : 'm';
    for (size_t i = 0; i < take && used < sizeof reply - 3; i++) {
        char c = document[offset + i];
        if (c == '#' || c == '$' || c == '}' || c == '*') {
            reply[used++] = '}';
            reply[used++] = (char)(c ^ 0x20);
        } else {
            reply[used++] = c;
        }
    }
    reply[used] = 0;
    send_packet(gdb, reply);
}


static void monitor_processes(gdb_t *gdb) {
    if (!ce_ready(&gdb->ce)) {
        monitor_reply(gdb, "CE's process table isn't set up yet\n");
        return;
    }
    int current = current_process(gdb);
    for (int process = 0; process < CE_PROCESS_MAX; process++) {
        char name[CE_NAME_MAX];
        if (!ce_process_name(&gdb->ce, process, name, sizeof name)) continue;
        monitor_reply(gdb, "%c%c %2d  %08x  %s\n", process == current ? '*' : ' ', process == gdb->process ? '>' : ' ', process,
                      (uint32_t)(process + 1) * MIPS_SLOT_SIZE, name);
    }
}

static void handle_monitor(gdb_t *gdb, const char *packet) {
    char command[256];
    size_t length = unhex_bytes((uint8_t *)command, packet + strlen("qRcmd,"), sizeof command - 1);
    command[length] = 0;
    char *argument = strchr(command, ' ');
    if (argument) {
        *argument++ = 0;
        while (*argument == ' ') argument++;
    }
    if (!strcmp(command, "processes") || !strcmp(command, "ps")) {
        monitor_processes(gdb);
    } else if (!strcmp(command, "process")) {
        if (!argument || !*argument) {
            if (gdb->process >= 0) monitor_reply(gdb, "debugging %s (process %d)\n", gdb->process_name, gdb->process);
            else if (gdb->waiting_for_process) monitor_reply(gdb, "waiting for %s to start\n", gdb->process_name);
            else monitor_reply(gdb, "breakpoints below 0x02000000 match any process\n");
        } else if (!strcmp(argument, "any")) {
            gdb_set_process(gdb, NULL);
            monitor_reply(gdb, "breakpoints below 0x02000000 now match any process\n");
        } else if (gdb_set_process(gdb, argument)) {
            monitor_reply(gdb, "debugging %s (process %d)\n", gdb->process_name, gdb->process);
        } else {
            monitor_reply(gdb, "%s isn't running; stopping when it starts\n", gdb->process_name);
        }
    } else if (!strcmp(command, "modules")) {
        monitor_modules(gdb);
    } else if (!strcmp(command, "libraries")) {
        if (argument && !strcmp(argument, "dll")) gdb->elf_libraries = false;
        else if (argument && !strcmp(argument, "elf")) gdb->elf_libraries = true;
        monitor_reply(gdb, "DLLs are reported to GDB as %s files\n", gdb->elf_libraries ? ".elf" : ".dll");
    } else if (!strcmp(command, "catch")) {
        if (argument && !strcmp(argument, "off")) gdb->catch_faults = false;
        else if (argument && !strcmp(argument, "on")) gdb->catch_faults = true;
        monitor_reply(gdb, "stopping on crashes: %s\n", gdb->catch_faults ? "on" : "off");
    } else if (!strcmp(command, "output")) {
        if (argument && !strcmp(argument, "off")) gdb->forward_output = false;
        else if (argument && !strcmp(argument, "on")) gdb->forward_output = true;
        monitor_reply(gdb, "CE debug output: %s\n", gdb->forward_output ? "on" : "off");
    } else {
        monitor_reply(gdb, "velo-emu monitor commands:\n"
                           "  processes          list CE's processes (* current, > debugged)\n"
                           "  process [NAME|any] debug one process; stops when it starts if it isn't running\n"
                           "  modules            list loaded modules (* used by the debugged process)\n"
                           "  libraries elf|dll  name DLLs to GDB by their .elf (default) or .dll file\n"
                           "  catch on|off       stop when CE reports a crash (default on)\n"
                           "  output on|off      show CE's debug output while running (default on)\n");
    }
    send_packet(gdb, "OK");
}

static void stop_running(gdb_t *gdb, int signal) {
    if (gdb->halted || !gdb->inferior) return;
    gdb->stop_kind = STOP_SIGNAL;
    gdb->stop_signal = signal;
    halt(gdb);
}

static void handle_vcont(gdb_t *gdb, const char *packet) {
    if (!strcmp(packet, "vCont?")) {
        send_packet(gdb, "vCont;c;C;s;S;t");
        return;
    }
    const char *action = packet + strlen("vCont;");
    if (action[0] == 't') {
        send_packet(gdb, "OK");
        stop_running(gdb, 0);
        return;
    }
    if (gdb->non_stop) send_packet(gdb, "OK");
    resume(gdb, action[0] == 's' || action[0] == 'S');
}


static void put_u16(uint8_t *data, size_t *length, uint32_t value) {
    data[(*length)++] = (uint8_t)value;
    data[(*length)++] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *data, size_t *length, uint32_t value) {
    put_u16(data, length, value & 0xFFFF);
    put_u16(data, length, value >> 16);
}

static void put_string(uint8_t *data, size_t *length, const char *text) {
    size_t count = strlen(text);
    put_u16(data, length, (uint32_t)count);
    for (size_t i = 0; i < count; i++) put_u16(data, length, (uint8_t)text[i]);
}

static uint32_t get_u32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint16_t start_request(gdb_t *gdb, uint8_t *message, size_t *length, uint16_t command) {
    gdb->agent_sequence = (uint16_t)((gdb->agent_sequence + 1) & 0x7FFF);
    uint16_t sequence = (uint16_t)(MAILBOX_EMULATOR_SEQUENCE | gdb->agent_sequence);
    *length = 0;
    put_u16(message, length, command);
    put_u16(message, length, sequence);
    return sequence;
}

static bool take_reply(gdb_t *gdb, uint16_t sequence, uint8_t *reply, uint32_t *reply_length) {
    mailbox_t *mailbox = machine_mailbox(gdb->machine);
    const mailbox_message_t *message;
    while ((message = mailbox_peek(&mailbox->to_emulator))) {
        uint16_t got = message->length >= 8 ? (uint16_t)(message->data[2] | message->data[3] << 8) : 0;
        if (got && got == gdb->run_sequence && got != sequence) {
            gdb->run_status = get_u32(message->data + 4);
            gdb->run_pid = message->length >= 12 ? get_u32(message->data + 8) : 0;
            gdb->run_replied = true;
        }
        bool wanted = got == sequence;
        if (wanted) {
            *reply_length = message->length;
            memcpy(reply, message->data, message->length);
        }
        mailbox_pop(&mailbox->to_emulator);
        if (wanted) return true;
    }
    return false;
}

static bool agent_request(gdb_t *gdb, const uint8_t *message, size_t length, uint16_t sequence, uint64_t timeout, uint8_t *reply,
                          uint32_t *reply_length) {
    mailbox_t *mailbox = machine_mailbox(gdb->machine);
    if (!mailbox_push(&mailbox->to_guest_from_emulator, message, (uint32_t)length)) return false;
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    mips_debug_t *debug = cpu->debug;
    cpu->debug = NULL;
    uint64_t start = machine_cycles(gdb->machine);
    bool replied = false;
    while (!(replied = take_reply(gdb, sequence, reply, reply_length)) && machine_cycles(gdb->machine) - start < timeout)
        machine_run(gdb->machine, RUN_QUANTUM);
    cpu->debug = debug;
    if (!replied) mailbox_clear_queue(&mailbox->to_guest_from_emulator);
    return replied && *reply_length >= 8;
}

static bool agent_available(gdb_t *gdb) {
    if (gdb->agent_state) return gdb->agent_state > 0;
    static uint8_t message[16], reply[MAILBOX_MESSAGE_MAX];
    size_t length;
    uint32_t reply_length;
    uint16_t sequence = start_request(gdb, message, &length, AGENT_PING);
    bool ok = agent_request(gdb, message, length, sequence, AGENT_PROBE_TIMEOUT, reply, &reply_length) && get_u32(reply + 4) == 0;
    gdb->agent_state = ok ? 1 : -1;
    logf_gdb(gdb, ok ? "gdb: debugmgr answered; file transfer and run go through it\n" : "gdb: no agent answered; file transfer and run are off\n");
    return ok;
}

static int fileio_errno(uint32_t status) {
    switch (status) {
    case 2:
    case 3: return FILEIO_ENOENT;
    case 5: return FILEIO_EACCES;
    case 80:
    case 183: return FILEIO_EEXIST;
    case 112: return FILEIO_ENOSPC;
    default: return FILEIO_EUNKNOWN;
    }
}

static void reply_errno(gdb_t *gdb, int error) {
    char reply[32];
    snprintf(reply, sizeof reply, "F-1,%x", error);
    send_packet(gdb, reply);
}

static void reply_result(gdb_t *gdb, uint32_t value) {
    char reply[32];
    snprintf(reply, sizeof reply, "F%x", value);
    send_packet(gdb, reply);
}

static bool decode_hex_string(const char **cursor, char *text, size_t size) {
    size_t length = 0;
    while (hex_value((*cursor)[0]) >= 0 && hex_value((*cursor)[1]) >= 0) {
        if (length + 1 >= size) return false;
        text[length++] = (char)(hex_value((*cursor)[0]) << 4 | hex_value((*cursor)[1]));
        *cursor += 2;
    }
    text[length] = 0;
    return true;
}

static bool decode_hex_path(const char **cursor, char *path, size_t size) {
    if (!decode_hex_string(cursor, path, size)) return false;
    for (char *c = path; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    return true;
}

static bool file_call(gdb_t *gdb, uint16_t command, uint32_t offset, uint32_t value, const char *path, const uint8_t *data, size_t data_length,
                      uint8_t *reply, uint32_t *reply_length, uint32_t *status) {
    static uint8_t message[MAILBOX_MESSAGE_MAX];
    size_t length;
    uint16_t sequence = start_request(gdb, message, &length, command);
    if (command == AGENT_WRITE || command == AGENT_READ) {
        put_u32(message, &length, offset);
        put_u32(message, &length, value);
    }
    put_string(message, &length, path);
    if (data_length > sizeof message - length) return false;
    memcpy(message + length, data, data_length);
    length += data_length;
    if (!agent_request(gdb, message, length, sequence, AGENT_TIMEOUT, reply, reply_length)) {
        logf_gdb(gdb, "gdb: debugmgr didn't answer about %s\n", path);
        return false;
    }
    *status = get_u32(reply + 4);
    if (*status && command != AGENT_READ) logf_gdb(gdb, "gdb: debugmgr couldn't %s %s (status %u)\n", command == AGENT_DELETE ? "delete" : "write", path, *status);
    return true;
}

static int file_descriptor(gdb_t *gdb, const char **cursor) {
    uint32_t fd = parse_hex(cursor);
    return fd < FILE_MAX && gdb->file_open[fd] ? (int)fd : -1;
}

static size_t unescape_binary(const char *data, size_t length, uint8_t *out) {
    size_t used = 0;
    for (size_t i = 0; i < length; i++) out[used++] = data[i] == '}' && i + 1 < length ? (uint8_t)(data[++i] ^ 0x20) : (uint8_t)data[i];
    return used;
}

static void send_binary_reply(gdb_t *gdb, const char *prefix, const uint8_t *data, size_t length) {
    static char reply[PACKET_MAX * 2 + 32];
    size_t used = (size_t)snprintf(reply, sizeof reply, "%s", prefix);
    for (size_t i = 0; i < length && used < sizeof reply - 3; i++) {
        uint8_t c = data[i];
        if (c == '#' || c == '$' || c == '}' || c == '*' || c == 0) {
            reply[used++] = '}';
            reply[used++] = (char)(c ^ 0x20);
        } else {
            reply[used++] = (char)c;
        }
    }
    send_packet_length(gdb, reply, used);
}

static void handle_vfile(gdb_t *gdb, const char *packet) {
    if (!strncmp(packet, "vFile:setfs:", 12)) {
        send_packet(gdb, "F0");
        return;
    }
    if (!agent_available(gdb)) {
        send_packet(gdb, "");
        return;
    }
    static uint8_t reply[MAILBOX_MESSAGE_MAX];
    uint32_t reply_length, status;
    char path[FILE_PATH_MAX];
    if (!strncmp(packet, "vFile:open:", 11)) {
        const char *cursor = packet + 11;
        if (!decode_hex_path(&cursor, path, sizeof path) || *cursor++ != ',') { reply_errno(gdb, FILEIO_ENOENT); return; }
        uint32_t flags = parse_hex(&cursor);
        int fd = 0;
        while (fd < FILE_MAX && gdb->file_open[fd]) fd++;
        if (fd == FILE_MAX) { reply_errno(gdb, FILEIO_EMFILE); return; }
        bool create = flags & (FILEIO_O_CREAT | FILEIO_O_TRUNC);
        bool done = create ? file_call(gdb, AGENT_WRITE, 0, AGENT_CREATE, path, NULL, 0, reply, &reply_length, &status)
                           : file_call(gdb, AGENT_READ, 0, 0, path, NULL, 0, reply, &reply_length, &status);
        if (!done) { reply_errno(gdb, FILEIO_EUNKNOWN); return; }
        if (status) { reply_errno(gdb, fileio_errno(status)); return; }
        snprintf(gdb->files[fd], sizeof gdb->files[fd], "%s", path);
        gdb->file_open[fd] = true;
        reply_result(gdb, (uint32_t)fd);
    } else if (!strncmp(packet, "vFile:close:", 12)) {
        const char *cursor = packet + 12;
        int fd = file_descriptor(gdb, &cursor);
        if (fd < 0) { reply_errno(gdb, FILEIO_EBADF); return; }
        gdb->file_open[fd] = false;
        reply_result(gdb, 0);
    } else if (!strncmp(packet, "vFile:pread:", 12)) {
        const char *cursor = packet + 12;
        int fd = file_descriptor(gdb, &cursor);
        if (fd < 0 || *cursor++ != ',') { reply_errno(gdb, FILEIO_EBADF); return; }
        uint32_t count = parse_hex(&cursor);
        if (*cursor++ != ',') { reply_errno(gdb, FILEIO_EBADF); return; }
        uint32_t offset = parse_hex(&cursor);
        if (count > PACKET_MAX / 2 - 32) count = PACKET_MAX / 2 - 32;
        if (!file_call(gdb, AGENT_READ, offset, count, gdb->files[fd], NULL, 0, reply, &reply_length, &status)) { reply_errno(gdb, FILEIO_EUNKNOWN); return; }
        if (status || reply_length < 12) { reply_errno(gdb, fileio_errno(status)); return; }
        char prefix[32];
        snprintf(prefix, sizeof prefix, "F%x;", reply_length - 12);
        send_binary_reply(gdb, prefix, reply + 12, reply_length - 12);
    } else if (!strncmp(packet, "vFile:pwrite:", 13)) {
        const char *cursor = packet + 13;
        int fd = file_descriptor(gdb, &cursor);
        if (fd < 0 || *cursor++ != ',') { reply_errno(gdb, FILEIO_EBADF); return; }
        uint32_t offset = parse_hex(&cursor);
        if (*cursor++ != ',') { reply_errno(gdb, FILEIO_EBADF); return; }
        static uint8_t data[PACKET_MAX];
        size_t length = unescape_binary(cursor, gdb->packet_length - (size_t)(cursor - packet), data);
        if (!file_call(gdb, AGENT_WRITE, offset, 0, gdb->files[fd], data, length, reply, &reply_length, &status)) { reply_errno(gdb, FILEIO_EUNKNOWN); return; }
        if (status) { reply_errno(gdb, fileio_errno(status)); return; }
        reply_result(gdb, (uint32_t)length);
    } else if (!strncmp(packet, "vFile:unlink:", 13)) {
        const char *cursor = packet + 13;
        if (!decode_hex_path(&cursor, path, sizeof path)) { reply_errno(gdb, FILEIO_ENOENT); return; }
        if (!file_call(gdb, AGENT_DELETE, 0, 0, path, NULL, 0, reply, &reply_length, &status)) { reply_errno(gdb, FILEIO_EUNKNOWN); return; }
        if (status) reply_errno(gdb, fileio_errno(status));
        else reply_result(gdb, 0);
    } else {
        send_packet(gdb, "");
    }
}

static const char *leaf_name(const char *path) {
    const char *leaf = path;
    for (const char *c = path; *c; c++) {
        if (*c == '\\' || *c == '/') leaf = c + 1;
    }
    return leaf;
}

static void handle_vrun(gdb_t *gdb, const char *packet) {
    char program[FILE_PATH_MAX], arguments[FILE_PATH_MAX] = "", argument[FILE_PATH_MAX];
    const char *cursor = packet + 5;
    if (!decode_hex_path(&cursor, program, sizeof program) || !program[0] || !agent_available(gdb)) {
        send_packet(gdb, "E01");
        return;
    }
    while (*cursor == ';') {
        cursor++;
        if (!decode_hex_string(&cursor, argument, sizeof argument)) break;
        size_t used = strlen(arguments);
        snprintf(arguments + used, sizeof arguments - used, "%s%s", used ? " " : "", argument);
    }
    gdb_set_process(gdb, NULL);
    snprintf(gdb->process_name, sizeof gdb->process_name, "%s", leaf_name(program));
    gdb->waiting_for_process = true;
    gdb->last_user_process = current_process(gdb);
    gdb->breakpoint_count = 0;
    gdb->watchpoint_count = 0;
    gdb->post_mortem = false;
    update_debug(gdb);
    static uint8_t message[MAILBOX_MESSAGE_MAX];
    size_t length;
    gdb->run_sequence = start_request(gdb, message, &length, AGENT_RUN);
    gdb->run_replied = false;
    put_string(message, &length, program);
    put_string(message, &length, arguments);
    mailbox_t *mailbox = machine_mailbox(gdb->machine);
    mailbox_push(&mailbox->to_guest_from_emulator, message, (uint32_t)length);
    gdb->debug.stop = false;
    gdb->inferior = true;
    uint64_t start = machine_cycles(gdb->machine);
    uint8_t unused[8];
    uint32_t unused_length;
    while (!gdb->debug.stop && machine_cycles(gdb->machine) - start < RUN_TIMEOUT) {
        machine_run(gdb->machine, RUN_QUANTUM);
        take_reply(gdb, 0, unused, &unused_length);
        if (gdb->run_replied && gdb->run_status) break;
    }
    if (!gdb->debug.stop) {
        gdb->waiting_for_process = false;
        update_debug(gdb);
        if (gdb->run_replied && gdb->run_status) logf_gdb(gdb, "gdb: debugmgr couldn't start %s (status %u)\n", program, gdb->run_status);
        else logf_gdb(gdb, "gdb: %s didn't start\n", program);
        send_packet(gdb, "E01");
        return;
    }
    gdb->debug.stop = false;
    gdb->run_active = true;
    gdb->exit_check = 0;
    gdb->sync_reply = true;
    halt(gdb);
    gdb->sync_reply = false;
}

static void wait_for_exit(gdb_t *gdb) {
    int process = gdb->process;
    char name[CE_NAME_MAX];
    if (process < 0 || !ce_process_name(&gdb->ce, process, name, sizeof name)) return;
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    mips_debug_t *debug = cpu->debug;
    cpu->debug = NULL;
    uint64_t start = machine_cycles(gdb->machine);
    char now[CE_NAME_MAX];
    while (ce_process_name(&gdb->ce, process, now, sizeof now) && !strcmp(now, name) && machine_cycles(gdb->machine) - start < AGENT_TIMEOUT)
        machine_run(gdb->machine, RUN_QUANTUM);
    cpu->debug = debug;
}

static bool kill_run_process(gdb_t *gdb) {
    uint8_t unused[8];
    uint32_t unused_length;
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    mips_debug_t *debug = cpu->debug;
    cpu->debug = NULL;
    uint64_t start = machine_cycles(gdb->machine);
    while (take_reply(gdb, 0, unused, &unused_length), !gdb->run_replied && machine_cycles(gdb->machine) - start < AGENT_TIMEOUT)
        machine_run(gdb->machine, RUN_QUANTUM);
    cpu->debug = debug;
    if (!gdb->run_replied || gdb->run_status || !agent_available(gdb)) return false;
    static uint8_t message[16], reply[MAILBOX_MESSAGE_MAX];
    size_t length;
    uint32_t reply_length;
    uint16_t sequence = start_request(gdb, message, &length, AGENT_KILL);
    put_u32(message, &length, gdb->run_pid);
    gdb->run_replied = false;
    if (!agent_request(gdb, message, length, sequence, AGENT_TIMEOUT, reply, &reply_length)) return false;
    uint32_t status = get_u32(reply + 4);
    if (status) {
        logf_gdb(gdb, "gdb: debugmgr couldn't end process %x (status %x)\n", gdb->run_pid, status);
        return false;
    }
    wait_for_exit(gdb);
    logf_gdb(gdb, "gdb: ended %s\n", gdb->process_name);
    return true;
}

static void forget_inferior(gdb_t *gdb) {
    gdb->inferior = false;
    gdb->halted = false;
    gdb->run_active = false;
    gdb->waiting_for_process = false;
    gdb->process = -1;
    gdb->process_name[0] = 0;
    gdb->breakpoint_count = 0;
    gdb->watchpoint_count = 0;
    gdb->post_mortem = false;
    update_debug(gdb);
}

static void check_run_exit(gdb_t *gdb) {
    if (++gdb->exit_check < EXIT_CHECK_QUANTA) return;
    gdb->exit_check = 0;
    char name[CE_NAME_MAX];
    if (gdb->process >= 0 && ce_process_name(&gdb->ce, gdb->process, name, sizeof name) && !strcasecmp(name, gdb->process_name)) return;
    logf_gdb(gdb, "gdb: %s has exited\n", gdb->process_name);
    forget_inferior(gdb);
    gdb->run_sequence = 0;
    send_async_stop(gdb, "W00");
}

static void handle_vkill(gdb_t *gdb) {
    bool started = gdb->run_sequence != 0;
    if (started && !kill_run_process(gdb)) logf_gdb(gdb, "gdb: debugmgr couldn't end the program\n");
    gdb->run_sequence = 0;
    forget_inferior(gdb);
    send_packet(gdb, "OK");
}

static void handle_packet(gdb_t *gdb, const char *packet) {
    switch (packet[0]) {
    case '?':
        if (gdb->non_stop && (!gdb->inferior || !gdb->halted)) send_packet(gdb, "OK");
        else if (!gdb->inferior) send_packet(gdb, "W00");
        else {
            gdb->sync_reply = true;
            send_stop_reply(gdb);
            gdb->sync_reply = false;
        }
        return;
    case 'g':
        handle_read_registers(gdb);
        return;
    case 'G':
        handle_write_registers(gdb, packet);
        return;
    case 'p': {
        const char *cursor = packet + 1;
        int number = (int)parse_hex(&cursor);
        char reply[9];
        hex_word(reply, read_register(gdb, number));
        send_packet(gdb, number < REGISTER_COUNT ? reply : "E01");
        return;
    }
    case 'P': {
        const char *cursor = packet + 1;
        int number = (int)parse_hex(&cursor);
        if (*cursor++ != '=') { send_packet(gdb, "E01"); return; }
        write_register(gdb, number, unhex_word(cursor));
        send_packet(gdb, "OK");
        return;
    }
    case 'm':
        handle_read_memory(gdb, packet);
        return;
    case 'M':
        handle_write_memory(gdb, packet);
        return;
    case 'c':
    case 'C':
        if (gdb->non_stop) send_packet(gdb, "OK");
        resume(gdb, false);
        return;
    case 's':
    case 'S':
        if (gdb->non_stop) send_packet(gdb, "OK");
        resume(gdb, true);
        return;
    case 'Z':
    case 'z':
        handle_breakpoint_packet(gdb, packet);
        return;
    case 'H':
        send_packet(gdb, "OK");
        return;
    case 'T':
        send_packet(gdb, "OK");
        return;
    case 'k':
        if (gdb->extended) {
            if (gdb->run_sequence) kill_run_process(gdb);
            gdb->run_sequence = 0;
            forget_inferior(gdb);
            return;
        }
        gdb->killed = true;
        close_client(gdb);
        return;
    case '!':
        gdb->extended = true;
        gdb->inferior = false;
        gdb->halted = false;
        gdb->run_active = false;
        update_debug(gdb);
        send_packet(gdb, "OK");
        return;
    case 'D':
        send_packet(gdb, "OK");
        close_client(gdb);
        return;
    case 'v':
        if (!strncmp(packet, "vCont", 5)) handle_vcont(gdb, packet);
        else if (!strncmp(packet, "vFile:", 6)) handle_vfile(gdb, packet);
        else if (!strncmp(packet, "vRun;", 5)) handle_vrun(gdb, packet);
        else if (!strncmp(packet, "vKill;", 6)) handle_vkill(gdb);
        else if (!strcmp(packet, "vStopped")) send_packet(gdb, "OK");
        else if (!strcmp(packet, "vCtrlC")) {
            send_packet(gdb, "OK");
            stop_running(gdb, SIGNAL_INT);
        }
        else send_packet(gdb, "");
        return;
    case 'q':
        if (!strncmp(packet, "qSupported", 10)) send_packet(gdb, "PacketSize=4000;qXfer:features:read+;qXfer:libraries:read+;QStartNoAckMode+;vContSupported+;QNonStop+");
        else if (!strncmp(packet, "qXfer:features:read:target.xml:", 31)) handle_xfer(gdb, packet, "features", TARGET_XML, sizeof TARGET_XML - 1);
        else if (!strncmp(packet, "qXfer:libraries:read::", 22)) {
            static char xml[CE_MODULE_MAX * 128 + 64];
            size_t length = library_list(gdb, xml, sizeof xml);
            gdb->library_hash = current_library_hash(gdb);
            handle_xfer(gdb, packet, "libraries", xml, length);
        }
        else if (!strcmp(packet, "qAttached")) send_packet(gdb, gdb->run_active ? "0" : "1");
        else if (!strcmp(packet, "qfThreadInfo")) send_packet(gdb, gdb->inferior ? "m1" : "l");
        else if (!strcmp(packet, "qsThreadInfo")) send_packet(gdb, "l");
        else if (!strcmp(packet, "qC")) send_packet(gdb, gdb->inferior ? "QC1" : "");
        else if (!strncmp(packet, "qRcmd,", 6)) handle_monitor(gdb, packet);
        else if (!strcmp(packet, "qSymbol::")) send_packet(gdb, "OK");
        else send_packet(gdb, "");
        return;
    case 'Q':
        if (!strncmp(packet, "QNonStop:", 9)) {
            gdb->non_stop = packet[9] == '1';
            send_packet(gdb, "OK");
        } else if (!strcmp(packet, "QStartNoAckMode")) {
            send_packet(gdb, "OK");
            gdb->no_ack = true;
        } else {
            send_packet(gdb, "");
        }
        return;
    default:
        send_packet(gdb, "");
    }
}

static bool receive(gdb_t *gdb, bool wait) {
    if (gdb->client < 0) return false;
    struct pollfd poll_fd = { .fd = gdb->client, .events = POLLIN };
    int ready = poll(&poll_fd, 1, wait ? -1 : 0);
    if (ready <= 0) return ready == 0 || errno == EINTR;
    if (gdb->input_length == sizeof gdb->input) gdb->input_length = 0;
    ssize_t received = recv(gdb->client, gdb->input + gdb->input_length, sizeof gdb->input - gdb->input_length, 0);
    if (received <= 0) {
        logf_gdb(gdb, "gdb: client disconnected\n");
        close_client(gdb);
        return false;
    }
    gdb->input_length += (size_t)received;
    return true;
}

static void consume(gdb_t *gdb, size_t count) {
    memmove(gdb->input, gdb->input + count, gdb->input_length - count);
    gdb->input_length -= count;
}

static bool next_packet(gdb_t *gdb, char *packet, bool *interrupt) {
    *interrupt = false;
    while (gdb->input_length) {
        uint8_t first = gdb->input[0];
        if (first == 0x03) {
            consume(gdb, 1);
            *interrupt = true;
            return false;
        }
        if (first != '$') {
            consume(gdb, 1);
            continue;
        }
        uint8_t *end = memchr(gdb->input, '#', gdb->input_length);
        if (!end || (size_t)(end - gdb->input) + 3 > gdb->input_length) return false;
        size_t length = (size_t)(end - gdb->input) - 1;
        if (length > PACKET_MAX) {
            consume(gdb, (size_t)(end - gdb->input) + 3);
            logf_gdb(gdb, "gdb: dropped a %zu byte packet, over the %d byte limit\n", length, PACKET_MAX);
            if (!gdb->no_ack) send_all(gdb, "+", 1);
            send_packet(gdb, "E01");
            continue;
        }
        memcpy(packet, gdb->input + 1, length);
        packet[length] = 0;
        gdb->packet_length = length;
        consume(gdb, (size_t)(end - gdb->input) + 3);
        if (!gdb->no_ack) send_all(gdb, "+", 1);
        return true;
    }
    return false;
}

static void detach_debugger(gdb_t *gdb) {
    gdb->breakpoint_count = 0;
    gdb->entry_break_count = 0;
    gdb->module_added = false;
    gdb->new_module = 0;
    gdb->entry_written = false;
    gdb->watchpoint_count = 0;
    gdb->halted = false;
    gdb->stepping = false;
    gdb->resume_skip = false;
    gdb->post_mortem = false;
    gdb->debug.stop = false;
    update_debug(gdb);
}

static bool accept_client(gdb_t *gdb, bool wait) {
    struct pollfd poll_fd = { .fd = gdb->listener, .events = POLLIN };
    if (poll(&poll_fd, 1, wait ? -1 : 0) <= 0) return false;
    int client = accept(gdb->listener, NULL, NULL);
    if (client < 0) return false;
    int on = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
#ifdef SO_NOSIGPIPE
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    gdb->client = client;
    gdb->no_ack = false;
    gdb->library_hash = 0;
    gdb->extended = false;
    gdb->non_stop = false;
    gdb->inferior = true;
    gdb->agent_state = 0;
    memset(gdb->file_open, 0, sizeof gdb->file_open);
    gdb->killed = false;
    gdb->input_length = 0;
    gdb->stop_kind = STOP_SIGNAL;
    gdb->stop_signal = SIGNAL_TRAP;
    gdb->halted = true;
    gdb->module_list_known = ce_module_list_address(&gdb->ce, &gdb->module_list_pa);
    update_debug(gdb);
    logf_gdb(gdb, "gdb: client connected\n");
    return true;
}

gdb_t *gdb_create(machine_t *machine, int port, gdb_log_fn log) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return NULL;
    int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(listener, (struct sockaddr *)&address, sizeof address) < 0 || listen(listener, 1) < 0) {
        close(listener);
        return NULL;
    }
    gdb_t *gdb = calloc(1, sizeof *gdb);
    gdb->machine = machine;
    gdb->log = log;
    gdb->listener = listener;
    gdb->client = -1;
    gdb->process = -1;
    gdb->last_user_process = -1;
    gdb->catch_faults = true;
    gdb->forward_output = true;
    gdb->elf_libraries = true;
    ce_init(&gdb->ce, machine);
    gdb->debug.context = gdb;
    gdb->debug.before = on_before;
    gdb->debug.access = on_access;
    gdb->debug.exception = on_exception;
    machine_cpu(machine)->debug = &gdb->debug;
    logf_gdb(gdb, "gdb: listening on 127.0.0.1:%d\n", port);
    return gdb;
}

void gdb_destroy(gdb_t *gdb) {
    if (!gdb) return;
    if (gdb->client >= 0 && !gdb->killed) send_packet(gdb, "W00");
    machine_cpu(gdb->machine)->debug = NULL;
    close_client(gdb);
    close(gdb->listener);
    free(gdb);
}

bool gdb_wait_for_client(gdb_t *gdb) {
    return accept_client(gdb, true);
}

bool gdb_connected(const gdb_t *gdb) {
    return gdb->client >= 0;
}

bool gdb_set_process(gdb_t *gdb, const char *name) {
    gdb->process = -1;
    gdb->waiting_for_process = false;
    gdb->process_name[0] = 0;
    if (name && *name) {
        snprintf(gdb->process_name, sizeof gdb->process_name, "%s", name);
        gdb->process = ce_find_process(&gdb->ce, name);
        gdb->waiting_for_process = gdb->process < 0;
        gdb->last_user_process = current_process(gdb);
    }
    update_debug(gdb);
    return gdb->process >= 0;
}

void gdb_service(gdb_t *gdb) {
    if (gdb->client < 0) {
        detach_debugger(gdb);
        if (!accept_client(gdb, false)) return;
    }
    static char packet[PACKET_MAX + 1];
    receive(gdb, false);
    while (gdb->client >= 0) {
        bool interrupt;
        bool got = next_packet(gdb, packet, &interrupt);
        if (interrupt) {
            if (!gdb->halted) {
                gdb->stop_kind = STOP_SIGNAL;
                gdb->stop_signal = SIGNAL_INT;
                halt(gdb);
            }
            continue;
        }
        if (!got) break;
        handle_packet(gdb, packet);
    }
    if (gdb->client < 0) detach_debugger(gdb);
}

bool gdb_halted(const gdb_t *gdb) {
    return gdb->halted && gdb->client >= 0;
}

static void check_libraries(gdb_t *gdb) {
    if (++gdb->library_check < LIBRARY_CHECK_QUANTA) return;
    gdb->library_check = 0;
    if (!gdb->module_list_known && (gdb->module_list_known = ce_module_list_address(&gdb->ce, &gdb->module_list_pa))) update_debug(gdb);
    uint32_t hash = current_library_hash(gdb);
    if (hash == gdb->library_hash) return;
    gdb->library_hash = hash;
    gdb->halted = true;
    gdb->stop_kind = STOP_SIGNAL;
    gdb->stop_signal = SIGNAL_TRAP;
    update_debug(gdb);
    send_async_stop(gdb, gdb->non_stop ? "T05thread:1;library:;" : "T05library:;");
}

void gdb_after_run(gdb_t *gdb) {
    if (gdb->debug.stop) {
        gdb->debug.stop = false;
        if (gdb->client >= 0 && gdb->inferior) halt(gdb);
        return;
    }
    if (gdb->client >= 0 && !gdb->halted && gdb->inferior) check_libraries(gdb);
    if (gdb->client >= 0 && !gdb->halted && gdb->run_active) check_run_exit(gdb);
}

void gdb_set_machine(gdb_t *gdb, machine_t *machine) {
    if (gdb->machine && gdb->machine != machine) machine_cpu(gdb->machine)->debug = NULL;
    gdb->machine = machine;
    ce_init(&gdb->ce, machine);
    gdb->debug.stop = false;
    gdb->post_mortem = false;
    memset(gdb->faults, 0, sizeof gdb->faults);
    if (gdb->process_name[0]) {
        char name[CE_NAME_MAX];
        snprintf(name, sizeof name, "%s", gdb->process_name);
        gdb_set_process(gdb, name);
    }
    machine_cpu(machine)->debug = &gdb->debug;
    if (gdb_halted(gdb)) {
        gdb->stop_kind = STOP_SIGNAL;
        gdb->stop_signal = SIGNAL_TRAP;
        send_console(gdb, "velo: switched machine\n");
    }
}

bool gdb_run(gdb_t *gdb, uint64_t cycles) {
    uint64_t target = machine_cycles(gdb->machine) + cycles;
    while (machine_cycles(gdb->machine) < target && !gdb->killed) {
        gdb_service(gdb);
        if (gdb->killed) break;
        if (gdb_halted(gdb)) {
            receive(gdb, true);
            continue;
        }
        uint64_t remaining = target - machine_cycles(gdb->machine);
        machine_run(gdb->machine, remaining < RUN_QUANTUM ? remaining : RUN_QUANTUM);
        gdb_after_run(gdb);
    }
    return !gdb->killed;
}
