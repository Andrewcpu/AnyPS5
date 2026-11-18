#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <wasm/Discover.hpp>
#include <wasm/Emit.hpp>
#include <wasm/WasmTarget.hpp>
#include <domain/Types.hpp>
#include <nid/NidCompute.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include <initializer_list>
#include <vector>
#include <csignal>
#include <setjmp.h>

extern char** environ;

extern "C" void Anyps5EnterGuest(std::uint64_t entry, std::uint64_t stack);

asm(R"(
.text
.globl Anyps5EnterGuest
.type Anyps5EnterGuest, @function
Anyps5EnterGuest:
    pushq $2
    popfq
    movq %rdi, anyps5_saved_entry(%rip)
    movq %rsi, %rsp
    pxor %xmm0, %xmm0
    pxor %xmm1, %xmm1
    pxor %xmm2, %xmm2
    pxor %xmm3, %xmm3
    pxor %xmm4, %xmm4
    pxor %xmm5, %xmm5
    pxor %xmm6, %xmm6
    pxor %xmm7, %xmm7
    pxor %xmm8, %xmm8
    pxor %xmm9, %xmm9
    pxor %xmm10, %xmm10
    pxor %xmm11, %xmm11
    pxor %xmm12, %xmm12
    pxor %xmm13, %xmm13
    pxor %xmm14, %xmm14
    pxor %xmm15, %xmm15
    movl $0, %eax
    movl $0, %ecx
    movl $0, %edx
    movl $0, %ebx
    movl $0, %ebp
    movl $0, %esi
    movl $0, %edi
    movl $0, %r8d
    movl $0, %r9d
    movl $0, %r10d
    movl $0, %r11d
    movl $0, %r12d
    movl $0, %r13d
    movl $0, %r14d
    movl $0, %r15d
    jmpq *anyps5_saved_entry(%rip)
.data
.globl anyps5_saved_entry
anyps5_saved_entry:
    .quad 0
.text
)");

namespace {

struct Capture {
    std::uint64_t Gpr[16]{};
    std::uint64_t Rip = 0;
    std::uint64_t Flags = 0;
    std::uint8_t Xmm[16][16]{};
    int Signal = 0;
};

Capture captured;
sigjmp_buf guestJump;
constexpr std::uint64_t kResumeFlag = 1ull << 16;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void GuestSignal(int signal, siginfo_t*, void* context) {
    auto* machine = static_cast<ucontext_t*>(context);
    const auto* general = machine->uc_mcontext.gregs;
    const int names[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    for (int index = 0; index < 16; ++index) captured.Gpr[index] = static_cast<std::uint64_t>(general[names[index]]);
    captured.Rip = static_cast<std::uint64_t>(general[REG_RIP]);
    captured.Flags = static_cast<std::uint64_t>(general[REG_EFL]);
    const auto* fp = reinterpret_cast<const _libc_fpstate*>(machine->uc_mcontext.fpregs);
    if (fp != nullptr) {
        for (int index = 0; index < 16; ++index) std::memcpy(captured.Xmm[index], &fp->_xmm[index], 16);
    }
    captured.Signal = signal;
    siglongjmp(guestJump, 1);
}

void ArmSignals() {
    stack_t stack{};
    stack.ss_size = 65536;
    stack.ss_sp = new unsigned char[stack.ss_size];
    Require(sigaltstack(&stack, nullptr) == 0, "sigaltstack failed");
    struct sigaction action{};
    action.sa_sigaction = GuestSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    Require(sigaction(SIGILL, &action, nullptr) == 0, "sigaction SIGILL failed");
    Require(sigaction(SIGSEGV, &action, nullptr) == 0, "sigaction SIGSEGV failed");
    Require(sigaction(SIGALRM, &action, nullptr) == 0, "sigaction SIGALRM failed");
}

void MapRange(std::uint64_t start, std::uint64_t end, int protection, const std::uint8_t* bytes, std::size_t size) {
    const auto page = start & ~std::uint64_t{0xfff};
    const auto limit = (end + 0xfff) & ~std::uint64_t{0xfff};
    auto* mapped = mmap(reinterpret_cast<void*>(page), static_cast<std::size_t>(limit - page), protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    Require(mapped != MAP_FAILED, "mmap failed");
    if (bytes != nullptr) std::memcpy(reinterpret_cast<void*>(start), bytes, size);
}

struct Result {
    int Status = 0;
    std::string Out;
    std::string Err;
};

Result Run(const std::vector<std::string>& arguments) {
    int output[2] = {-1, -1};
    int errors[2] = {-1, -1};
    Require(pipe(output) == 0 && pipe(errors) == 0, "pipe failed");
    posix_spawn_file_actions_t actions;
    Require(posix_spawn_file_actions_init(&actions) == 0, "posix_spawn_file_actions_init failed");
    posix_spawn_file_actions_adddup2(&actions, output[1], 1);
    posix_spawn_file_actions_adddup2(&actions, errors[1], 2);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    posix_spawn_file_actions_addclose(&actions, errors[0]);
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output[1]);
    close(errors[1]);
    Require(spawned == 0, "cannot run " + arguments.front());
    Result result;
    auto readAll = [](int fd) {
        std::string text;
        char buffer[4096];
        while (true) {
            const auto count = read(fd, buffer, sizeof(buffer));
            if (count == 0) break;
            Require(count > 0, "read failed");
            text.append(buffer, static_cast<std::size_t>(count));
        }
        close(fd);
        return text;
    };
    result.Out = readAll(output[0]);
    result.Err = readAll(errors[0]);
    int status = 0;
    Require(waitpid(pid, &status, 0) == pid && WIFEXITED(status), arguments.front() + " did not exit");
    result.Status = WEXITSTATUS(status);
    return result;
}

void Put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value, int size) {
    for (int index = 0; index < size; ++index) bytes[offset + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value >> (8 * index));
}

std::vector<std::uint8_t> Elf(std::uint64_t entry, std::uint64_t address, const std::vector<std::uint8_t>& image) {
    std::vector<std::uint8_t> elf(64 + 56 + image.size());
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    Put(elf, 16, 2, 2);
    Put(elf, 18, 62, 2);
    Put(elf, 20, 1, 4);
    Put(elf, 24, entry, 8);
    Put(elf, 32, 64, 8);
    Put(elf, 52, 64, 2);
    Put(elf, 54, 56, 2);
    Put(elf, 56, 1, 2);
    Put(elf, 64, 1, 4);
    Put(elf, 68, 7, 4);
    Put(elf, 72, 120, 8);
    Put(elf, 80, address, 8);
    Put(elf, 96, image.size(), 8);
    Put(elf, 104, image.size(), 8);
    Put(elf, 112, 0x1000, 8);
    std::memcpy(elf.data() + 120, image.data(), image.size());
    return elf;
}

std::vector<std::uint8_t> DecodeHex(const std::string& text) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < text.size(); index += 2) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

std::uint64_t Read64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
    return value;
}

std::string Hex(const std::uint8_t* bytes, std::size_t size) {
    std::string text;
    text.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        text.push_back("0123456789abcdef"[bytes[index] >> 4]);
        text.push_back("0123456789abcdef"[bytes[index] & 0xf]);
    }
    return text;
}

std::vector<std::uint8_t> FromHex(const std::string& text) {
    Require(text.size() % 2 == 0, "odd hex dump");
    return DecodeHex(text);
}

void ExpectEqual(const std::string& name, std::uint64_t actual, std::uint64_t expected) {
    if (actual == expected) return;
    std::ostringstream message;
    message << name << " is 0x" << std::hex << actual << ", expected 0x" << expected;
    throw std::runtime_error(message.str());
}

void CheckState(const std::string& side, const std::uint64_t gpr[16], std::uint64_t rip, std::uint64_t flags, const std::uint8_t xmm[16][16]) {
    const std::uint64_t expected[16] = {0x10, 0, 0xf, 0x10100, 0x21000, 0, 1, 0xe, 0x10108, 0x1010f, 0, 0, 0, 0, 0, 0};
    for (int index = 0; index < 16; ++index) ExpectEqual(side + " gpr " + std::to_string(index), gpr[index], expected[index]);
    ExpectEqual(side + " rip", rip, 0x10054);
    ExpectEqual(side + " rflags", flags & ~kResumeFlag, 0x212);
    for (int reg = 0; reg < 16; ++reg) {
        for (int byte = 0; byte < 16; ++byte) {
            const auto want = reg == 0 && byte == 0 ? 0x0f : 0;
            if (xmm[reg][byte] != want) ExpectEqual(side + " xmm" + std::to_string(reg), xmm[reg][byte], want);
        }
    }
}

const char* kNodeScript = R"(
const fs = require('fs');
const bytes = fs.readFileSync(process.argv[2]);
const imports = { env: { anyps5_host_monotonic_us() { return process.hrtime.bigint() / 1000n; } } };
const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), imports);
const status = instance.exports.anyps5_run();
const state = instance.exports.anyps5_state();
const base = instance.exports.anyps5_guest_base();
const limit = instance.exports.anyps5_guest_limit();
const memoryBase = instance.exports.anyps5_memory_base();
const span = Number(process.argv[3]);
const memory = new Uint8Array(instance.exports.memory.buffer);
function hex(start, length) {
  let text = '';
  for (let index = 0; index < length; index += 1) text += memory[start + index].toString(16).padStart(2, '0');
  return text;
}
function text(value) { return typeof value === 'bigint' ? value.toString() : String(value); }
process.stdout.write(text(status) + '\n' + text(state) + '\n' + text(base) + '\n' + text(limit) + '\n' + text(memoryBase) + '\n' + hex(state, 400) + '\n' + hex(memoryBase, span) + '\n');
)";

void Unsupported() {
    const std::uint8_t code[] = {0x0f, 0xa2};
    bool threw = false;
    try {
        const auto module = Relinker::Wasm::CompileWasm(Elf(0x10000, 0x10000, std::vector<std::uint8_t>(code, code + 2)));
        (void)module;
    } catch (const std::exception& error) {
        threw = true;
        Require(std::string(error.what()).find("unsupported") != std::string::npos, std::string("unexpected reject: ") + error.what());
    }
    Require(threw, "CPUID was lifted");
}

void Milestone() {
    auto image = DecodeHex("b905000000b80000000089c789cee84a000000ffc975f383f80f753aeb020f0b83e8017c3183c00189c2660f6ec266480f7ec048c7c300010100488903488b034883c001488943084c8d43084c8d0c13660fefc90f0bb8ee0000000f0b89f801f0c3");
    image.resize(0x120);
    const auto elf = Elf(0x10000, 0x10000, image);
    const auto plan = Relinker::Wasm::PlanWasmImage(elf);
    const auto directory = std::filesystem::temp_directory_path() / ("anyps5-wasm-lift-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto input = directory / "guest.elf";
    const auto output = directory / "guest.wasm";
    const auto script = directory / "run.js";
    {
        std::ofstream elfFile(input, std::ios::binary);
        elfFile.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
        std::ofstream scriptFile(script);
        scriptFile << kNodeScript;
    }
    const auto linked = Run({ANYPS5_RELINKER, "--wasm", input.string(), output.string()});
    Require(linked.Status == 0, "relinker --wasm failed: " + linked.Err + linked.Out);
    std::ifstream wasm(output, std::ios::binary);
    std::vector<std::uint8_t> module((std::istreambuf_iterator<char>(wasm)), std::istreambuf_iterator<char>());
    Require(module.size() >= 4 && module[0] == 0 && module[1] == 'a' && module[2] == 's' && module[3] == 'm', "output is not a WebAssembly module");

    for (const auto& segment : plan.Segments) {
        MapRange(segment.Virtual, segment.Virtual + segment.MemorySize, PROT_READ | PROT_WRITE | PROT_EXEC, segment.Bytes.data(), segment.Bytes.size());
    }
    MapRange(plan.StackBase, plan.StackTop, PROT_READ | PROT_WRITE, nullptr, 0);
    alarm(5);
    if (sigsetjmp(guestJump, 1) == 0) Anyps5EnterGuest(plan.Entry, plan.StackTop);
    alarm(0);
    asm volatile("" ::: "memory");
    Require(captured.Signal == SIGILL, "native guest stopped with signal " + std::to_string(captured.Signal));
    CheckState("native", captured.Gpr, captured.Rip, captured.Flags, captured.Xmm);

    const auto node = Run({ANYPS5_NODE, script.string(), output.string(), std::to_string(plan.GuestLimit - plan.GuestBase)});
    Require(node.Status == 0, "node failed: " + node.Err);
    std::istringstream lines(node.Out);
    std::string statusText, stateText, baseText, limitText, memoryBaseText, stateHex, memoryHex;
    Require(static_cast<bool>(std::getline(lines, statusText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, stateText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, baseText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, limitText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, memoryBaseText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, stateHex)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, memoryHex)), "node dump is incomplete");
    ExpectEqual("wasm status", std::stoull(statusText), 0);
    ExpectEqual("state", std::stoull(stateText), Relinker::Wasm::kStateAddress);
    ExpectEqual("guest base", std::stoull(baseText), plan.GuestBase);
    ExpectEqual("guest limit", std::stoull(limitText), plan.GuestLimit);
    ExpectEqual("memory base", std::stoull(memoryBaseText), Relinker::Wasm::kWasmGuestBase);
    Require(std::stoull(memoryBaseText) != plan.GuestBase, "guest address was used as a wasm pointer");
    const auto state = FromHex(stateHex);
    const auto memory = FromHex(memoryHex);
    Require(state.size() == Relinker::Wasm::kStateBytes, "state size");
    Require(memory.size() == plan.GuestLimit - plan.GuestBase, "guest memory size");
    std::uint64_t wasmGpr[16];
    for (int index = 0; index < 16; ++index) wasmGpr[index] = Read64(state.data() + index * 8);
    std::uint8_t wasmXmm[16][16];
    for (int reg = 0; reg < 16; ++reg) std::memcpy(wasmXmm[reg], state.data() + Relinker::Wasm::kXmmOffset + static_cast<std::size_t>(reg) * 16, 16);
    CheckState("wasm", wasmGpr, Read64(state.data() + Relinker::Wasm::kRipOffset), Read64(state.data() + Relinker::Wasm::kRflagsOffset), wasmXmm);
    auto at = [&](std::uint64_t address) {
        Require(address >= plan.GuestBase && address < plan.GuestLimit, "address outside the guest");
        return memory.data() + static_cast<std::size_t>(address - plan.GuestBase);
    };
    for (const auto& segment : plan.Segments) {
        Require(std::memcmp(reinterpret_cast<const void*>(segment.Virtual), at(segment.Virtual), static_cast<std::size_t>(segment.MemorySize)) == 0, "segment bytes differ");
    }
    const auto stackBytes = static_cast<std::size_t>(plan.StackTop - plan.StackBase);
    Require(std::memcmp(reinterpret_cast<const void*>(plan.StackBase), at(plan.StackBase), stackBytes) == 0, "stack bytes differ");
    ExpectEqual("return slot", Read64(at(plan.StackTop - 8)), 0x10013);
    ExpectEqual("stored sum", Read64(at(0x10100)), 15);
    ExpectEqual("stored increment", Read64(at(0x10108)), 16);
    std::filesystem::remove_all(directory);
    munmap(reinterpret_cast<void*>(plan.GuestBase & ~std::uint64_t{0xfff}), static_cast<std::size_t>(((plan.StackTop + 0xfff) & ~std::uint64_t{0xfff}) - (plan.GuestBase & ~std::uint64_t{0xfff})));
}

constexpr std::uint64_t kFlagMask = 0xad7;
constexpr std::uint64_t kLogicalMask = 0xac7;

struct Asm {
    std::vector<std::uint8_t> Bytes;
    void B(std::initializer_list<std::uint8_t> bytes) { Bytes.insert(Bytes.end(), bytes.begin(), bytes.end()); }
    void U32(std::uint32_t value) { for (int index = 0; index < 4; ++index) Bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index))); }
    void U64(std::uint64_t value) { for (int index = 0; index < 8; ++index) Bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index))); }
};

struct GuestRun {
    Relinker::Wasm::WasmImage Plan;
    Capture Native;
    int Status = 0;
    std::uint64_t MemoryBase = 0;
    std::vector<std::uint8_t> State;
    std::vector<std::uint8_t> WasmMemory;
    std::vector<std::uint8_t> NativeMemory;
    std::uint64_t Gpr[16]{};
    std::uint64_t Rip = 0;
    std::uint64_t Flags = 0;
    std::uint8_t Xmm[16][16]{};
};

std::uint32_t Read32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(bytes[index]) << (8 * index);
    return value;
}

std::uint64_t At(const std::vector<std::uint8_t>& memory, std::uint64_t base, std::uint64_t address) {
    return Read64(memory.data() + static_cast<std::size_t>(address - base));
}

void ExpectFlags(const std::string& name, std::uint64_t native, std::uint64_t wasm, bool logical) {
    ExpectEqual(name, wasm & (logical ? kLogicalMask : kFlagMask), native & (logical ? kLogicalMask : kFlagMask));
}

GuestRun RunGuest(const std::vector<std::uint8_t>& elf) {
    GuestRun run;
    run.Plan = Relinker::Wasm::PlanWasmImage(elf);
    static int serial = 0;
    const auto directory = std::filesystem::temp_directory_path() / ("anyps5-wasm-mem-" + std::to_string(getpid()) + "-" + std::to_string(serial++));
    std::filesystem::create_directories(directory);
    const auto input = directory / "guest.elf";
    const auto output = directory / "guest.wasm";
    const auto script = directory / "run.js";
    {
        std::ofstream elfFile(input, std::ios::binary);
        elfFile.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
        std::ofstream scriptFile(script);
        scriptFile << kNodeScript;
    }
    const auto linked = Run({ANYPS5_RELINKER, "--wasm", input.string(), output.string()});
    if (linked.Status != 0) {
        std::filesystem::remove_all(directory);
        Require(false, "relinker --wasm failed: " + linked.Err + linked.Out);
    }
    const auto span = static_cast<std::size_t>(run.Plan.GuestLimit - run.Plan.GuestBase);
    const auto page = run.Plan.GuestBase & ~std::uint64_t{0xfff};
    const auto limit = (run.Plan.GuestLimit + 0xfff) & ~std::uint64_t{0xfff};
    auto* mapped = mmap(reinterpret_cast<void*>(page), static_cast<std::size_t>(limit - page), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    Require(mapped == reinterpret_cast<void*>(page), "mmap failed");
    for (const auto& segment : run.Plan.Segments) std::memcpy(reinterpret_cast<void*>(segment.Virtual), segment.Bytes.data(), segment.Bytes.size());
    std::memset(&captured, 0, sizeof(captured));
    alarm(5);
    if (sigsetjmp(guestJump, 1) == 0) Anyps5EnterGuest(run.Plan.Entry, run.Plan.StackTop);
    alarm(0);
    asm volatile("" ::: "memory");
    run.Native = captured;
    run.NativeMemory.resize(span);
    std::memcpy(run.NativeMemory.data(), reinterpret_cast<const void*>(run.Plan.GuestBase), span);
    munmap(mapped, static_cast<std::size_t>(limit - page));
    const auto node = Run({ANYPS5_NODE, script.string(), output.string(), std::to_string(span)});
    std::filesystem::remove_all(directory);
    Require(node.Status == 0, "node failed: " + node.Err + node.Out);
    std::istringstream lines(node.Out);
    std::string statusText, stateText, baseText, limitText, memoryBaseText, stateHex, memoryHex;
    Require(static_cast<bool>(std::getline(lines, statusText) && std::getline(lines, stateText) && std::getline(lines, baseText) && std::getline(lines, limitText) && std::getline(lines, memoryBaseText) && std::getline(lines, stateHex) && std::getline(lines, memoryHex)), "node dump is incomplete");
    run.Status = static_cast<int>(std::stoll(statusText));
    ExpectEqual("state", std::stoull(stateText), Relinker::Wasm::kStateAddress);
    ExpectEqual("guest base", std::stoull(baseText), run.Plan.GuestBase);
    ExpectEqual("guest limit", std::stoull(limitText), run.Plan.GuestLimit);
    run.MemoryBase = std::stoull(memoryBaseText);
    ExpectEqual("memory base", run.MemoryBase, Relinker::Wasm::kWasmGuestBase);
    if (run.Plan.GuestBase != 0) Require(run.MemoryBase != run.Plan.GuestBase, "guest address was used as a wasm pointer");
    run.State = FromHex(stateHex);
    run.WasmMemory = FromHex(memoryHex);
    Require(run.State.size() == Relinker::Wasm::kStateBytes, "state size");
    Require(run.WasmMemory.size() == span, "guest memory size");
    for (int index = 0; index < 16; ++index) run.Gpr[index] = Read64(run.State.data() + index * 8);
    run.Rip = Read64(run.State.data() + Relinker::Wasm::kRipOffset);
    run.Flags = Read64(run.State.data() + Relinker::Wasm::kRflagsOffset);
    for (int reg = 0; reg < 16; ++reg) std::memcpy(run.Xmm[reg], run.State.data() + Relinker::Wasm::kXmmOffset + static_cast<std::size_t>(reg) * 16, 16);
    return run;
}

void ExpectSameMemory(const GuestRun& run) {
    Require(run.NativeMemory.size() == run.WasmMemory.size(), "memory length");
    for (std::size_t index = 0; index < run.NativeMemory.size(); ++index) {
        if (run.NativeMemory[index] == run.WasmMemory[index]) continue;
        ExpectEqual("memory " + std::to_string(run.Plan.GuestBase + index), run.WasmMemory[index], run.NativeMemory[index]);
    }
}

struct ElfLoad {
    std::uint64_t Virtual = 0;
    std::uint32_t Flags = 5;
    std::uint64_t Memory = 0;
    std::vector<std::uint8_t> Bytes;
};

struct ElfSym {
    std::uint64_t Value = 0;
    std::uint16_t Section = 0;
    std::uint8_t Info = 0x10;
};

struct ElfRel {
    std::uint64_t Offset = 0;
    std::uint32_t Type = 0;
    std::uint32_t Symbol = 0;
    std::int64_t Addend = 0;
};

void Add(std::vector<std::uint8_t>& bytes, std::uint64_t value, int size) {
    for (int index = 0; index < size; ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
}

void Shdr(std::vector<std::uint8_t>& bytes, std::uint32_t type, std::uint64_t flags, std::uint64_t address, std::uint64_t offset, std::uint64_t size, std::uint32_t link, std::uint32_t info, std::uint64_t entsize) {
    Add(bytes, 0, 4);
    Add(bytes, type, 4);
    Add(bytes, flags, 8);
    Add(bytes, address, 8);
    Add(bytes, offset, 8);
    Add(bytes, size, 8);
    Add(bytes, link, 4);
    Add(bytes, info, 4);
    Add(bytes, 1, 8);
    Add(bytes, entsize, 8);
}

std::vector<std::uint8_t> MakeElf(std::uint64_t entry, std::uint16_t kind, const std::vector<ElfLoad>& loads, const std::vector<ElfSym>& symbols, const std::vector<ElfRel>& relocs, bool useRel) {
    const bool sections = !symbols.empty() || !relocs.empty() || useRel;
    const int relCount = useRel && relocs.empty() ? 1 : static_cast<int>(relocs.size());
    const int relEnt = useRel ? 16 : 24;
    const std::size_t phOff = 64;
    std::size_t cursor = phOff + 56 * loads.size();
    std::vector<std::size_t> loadAt(loads.size());
    for (std::size_t index = 0; index < loads.size(); ++index) {
        loadAt[index] = cursor;
        cursor += loads[index].Bytes.size();
    }
    const auto symAt = cursor;
    const auto symBytes = sections ? (symbols.size() + 1) * 24 : 0;
    cursor += symBytes;
    const auto strAt = cursor;
    const auto strBytes = sections ? std::size_t{1} : 0;
    cursor += strBytes;
    const auto relAt = cursor;
    const auto relBytes = sections ? static_cast<std::size_t>(relCount) * static_cast<std::size_t>(relEnt) : 0;
    cursor += relBytes;
    const auto shAt = cursor;
    const int shnum = sections ? static_cast<int>(loads.size()) + 4 : 0;
    std::vector<std::uint8_t> elf(shAt + static_cast<std::size_t>(shnum) * 64);
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    Put(elf, 16, kind, 2);
    Put(elf, 18, 62, 2);
    Put(elf, 20, 1, 4);
    Put(elf, 24, entry, 8);
    Put(elf, 32, phOff, 8);
    Put(elf, 40, sections ? shAt : 0, 8);
    Put(elf, 52, 64, 2);
    Put(elf, 54, 56, 2);
    Put(elf, 56, loads.size(), 2);
    Put(elf, 58, 64, 2);
    Put(elf, 60, static_cast<std::uint64_t>(shnum), 2);
    for (std::size_t index = 0; index < loads.size(); ++index) {
        const auto header = phOff + index * 56;
        const auto memory = loads[index].Memory == 0 ? loads[index].Bytes.size() : loads[index].Memory;
        Put(elf, header, 1, 4);
        Put(elf, header + 4, loads[index].Flags, 4);
        Put(elf, header + 8, loadAt[index], 8);
        Put(elf, header + 16, loads[index].Virtual, 8);
        Put(elf, header + 24, loads[index].Virtual, 8);
        Put(elf, header + 32, loads[index].Bytes.size(), 8);
        Put(elf, header + 40, memory, 8);
        Put(elf, header + 48, 0x1000, 8);
        std::memcpy(elf.data() + loadAt[index], loads[index].Bytes.data(), loads[index].Bytes.size());
    }
    if (!sections) return elf;
    const auto symtab = loads.size() + 1;
    const auto strtab = symtab + 1;
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        const auto at = symAt + (index + 1) * 24;
        elf[at + 4] = symbols[index].Info;
        Put(elf, at + 6, symbols[index].Section, 2);
        Put(elf, at + 8, symbols[index].Value, 8);
    }
    for (std::size_t index = 0; index < relocs.size(); ++index) {
        const auto at = relAt + index * static_cast<std::size_t>(relEnt);
        Put(elf, at, relocs[index].Offset, 8);
        Put(elf, at + 8, (static_cast<std::uint64_t>(relocs[index].Symbol) << 32) | relocs[index].Type, 8);
        if (!useRel) Put(elf, at + 16, static_cast<std::uint64_t>(relocs[index].Addend), 8);
    }
    const auto shdr = elf.data() + shAt;
    (void)shdr;
    std::vector<std::uint8_t> headers;
    Shdr(headers, 0, 0, 0, 0, 0, 0, 0, 0);
    for (std::size_t index = 0; index < loads.size(); ++index) {
        std::uint64_t flags = 2;
        if ((loads[index].Flags & 1u) != 0) flags |= 4;
        if ((loads[index].Flags & 2u) != 0) flags |= 1;
        Shdr(headers, 1, flags, loads[index].Virtual, loadAt[index], loads[index].Bytes.size(), 0, 0, 0);
    }
    Shdr(headers, 2, 0, 0, symAt, symBytes, static_cast<std::uint32_t>(strtab), 1, 24);
    Shdr(headers, 3, 0, 0, strAt, strBytes, 0, 0, 0);
    Shdr(headers, useRel ? 9 : 4, 0, 0, relAt, relBytes, static_cast<std::uint32_t>(symtab), 0, static_cast<std::uint64_t>(relEnt));
    Require(headers.size() == static_cast<std::size_t>(shnum) * 64, "section header size");
    std::memcpy(elf.data() + shAt, headers.data(), headers.size());
    return elf;
}

std::vector<ElfLoad> BaseLoads(std::uint64_t base) {
    return {{base, 5, 2, {0x0f, 0x0b}}, {base + 0x1000, 6, 16, std::vector<std::uint8_t>(16)}};
}

void Throws(const std::vector<std::uint8_t>& elf, const char* text) {
    bool threw = false;
    try {
        (void)Relinker::Wasm::PlanWasmImage(elf);
    } catch (const std::exception& error) {
        threw = true;
        Require(std::string(error.what()).find(text) != std::string::npos, std::string("unexpected reject: ") + error.what());
    }
    Require(threw, std::string("relocation was accepted: ") + text);
}

void RelocationBytes() {
    constexpr std::uint64_t base = 0x60000000;
    const std::uint64_t data = base + 0x1000;
    std::vector<std::uint8_t> bytes(0x28);
    const std::vector<ElfLoad> loads = {{base, 5, 2, {0x0f, 0x0b}}, {data, 6, bytes.size(), bytes}};
    const std::vector<ElfSym> symbols = {{data + 0x20, 2, 0x10}, {0, 0, 0x20}};
    const std::vector<ElfRel> relocs = {
        {data, 10, 1, 0},
        {data + 4, 11, 1, 0},
        {data + 8, 24, 1, 0},
        {data + 16, 6, 1, 0},
        {data + 24, 1, 2, 0},
        {data + 32, 1, 1, 8},
    };
    const auto plan = Relinker::Wasm::PlanWasmImage(MakeElf(base, 2, loads, symbols, relocs, false));
    const auto& segment = plan.Segments[1];
    ExpectEqual("R_X86_64_32", Read32(segment.Bytes.data()), static_cast<std::uint32_t>(data + 0x20));
    ExpectEqual("R_X86_64_32S", Read32(segment.Bytes.data() + 4), static_cast<std::uint32_t>(data + 0x20));
    ExpectEqual("R_X86_64_PC64", Read64(segment.Bytes.data() + 8), 0x18);
    ExpectEqual("R_X86_64_GLOB_DAT", Read64(segment.Bytes.data() + 16), data + 0x20);
    ExpectEqual("weak undefined", Read64(segment.Bytes.data() + 24), 0);
    ExpectEqual("R_X86_64_64 addend", Read64(segment.Bytes.data() + 32), data + 0x28);
    std::vector<std::uint8_t> dyn(96);
    const std::uint64_t slot = data;
    const std::uint64_t rela = data + 8;
    const std::uint64_t dynamic = data + 32;
    const std::uint64_t addend = 0x123456789abcdef0ull;
    Put(dyn, 8, slot, 8);
    Put(dyn, 16, 8, 8);
    Put(dyn, 24, addend, 8);
    Put(dyn, 32, 7, 8);
    Put(dyn, 40, rela, 8);
    Put(dyn, 48, 8, 8);
    Put(dyn, 56, 24, 8);
    Put(dyn, 64, 9, 8);
    Put(dyn, 72, 24, 8);
    std::vector<std::uint8_t> code = {0x0f, 0x0b};
    const std::size_t phOff = 64;
    const std::size_t codeAt = phOff + 56 * 3;
    const std::size_t dataAt = codeAt + code.size();
    std::vector<std::uint8_t> dynamicElf(dataAt + dyn.size());
    dynamicElf[0] = 0x7f;
    dynamicElf[1] = 'E';
    dynamicElf[2] = 'L';
    dynamicElf[3] = 'F';
    dynamicElf[4] = 2;
    dynamicElf[5] = 1;
    dynamicElf[6] = 1;
    Put(dynamicElf, 16, 3, 2);
    Put(dynamicElf, 18, 62, 2);
    Put(dynamicElf, 20, 1, 4);
    Put(dynamicElf, 24, base, 8);
    Put(dynamicElf, 32, phOff, 8);
    Put(dynamicElf, 52, 64, 2);
    Put(dynamicElf, 54, 56, 2);
    Put(dynamicElf, 56, 3, 2);
    auto phdr = [&](std::size_t index, std::uint32_t type, std::uint32_t flags, std::uint64_t offset, std::uint64_t address, std::uint64_t filesz, std::uint64_t memsz) {
        const auto header = phOff + index * 56;
        Put(dynamicElf, header, type, 4);
        Put(dynamicElf, header + 4, flags, 4);
        Put(dynamicElf, header + 8, offset, 8);
        Put(dynamicElf, header + 16, address, 8);
        Put(dynamicElf, header + 24, address, 8);
        Put(dynamicElf, header + 32, filesz, 8);
        Put(dynamicElf, header + 40, memsz, 8);
        Put(dynamicElf, header + 48, 0x1000, 8);
    };
    phdr(0, 1, 5, codeAt, base, code.size(), code.size());
    phdr(1, 1, 6, dataAt, data, dyn.size(), dyn.size());
    phdr(2, 2, 6, dataAt + 32, dynamic, 64, 64);
    std::memcpy(dynamicElf.data() + codeAt, code.data(), code.size());
    std::memcpy(dynamicElf.data() + dataAt, dyn.data(), dyn.size());
    const auto dynamicPlan = Relinker::Wasm::PlanWasmImage(dynamicElf);
    ExpectEqual("R_X86_64_RELATIVE", Read64(dynamicPlan.Segments[1].Bytes.data()), addend);
}

void RelocationRejects() {
    constexpr std::uint64_t base = 0x200000;
    const auto loads = BaseLoads(base);
    const ElfSym defined{base + 0x1000, 2, 0x10};
    const ElfSym absolute{0x100000000ull, 0xfff1, 0x10};
    const ElfSym far{base + 0x1000 + 0x80000000ull, 0xfff1, 0x10};
    Throws(MakeElf(base, 2, loads, {{0, 0, 0x10}}, {{base + 0x1000, 1, 1, 0}}, false), "undefined");
    Throws(MakeElf(base, 2, loads, {defined}, {{base + 0x1000, 18, 1, 0}}, false), "TLS");
    Throws(MakeElf(base, 2, loads, {absolute}, {{base + 0x1000, 10, 1, 0}}, false), "overflow");
    Throws(MakeElf(base, 2, loads, {defined}, {{base + 0x1000, 1, 1, 0}, {base + 0x1000, 1, 1, 0}}, false), "overlapping");
    Throws(MakeElf(base, 2, loads, {defined}, {{0x10, 1, 1, 0}}, false), "outside");
    Throws(MakeElf(base, 2, loads, {defined}, {{base + 0x1000, 1, 1, 0}}, true), "REL relocations");
    Throws(MakeElf(base, 2, loads, {defined}, {{base + 0x1000, 6, 1, 5}}, false), "nonzero");
    Throws(MakeElf(base, 2, loads, {defined}, {{base + 0x1000, 12, 1, 0}}, false), "unsupported");
    Throws(MakeElf(base, 2, loads, {far}, {{base + 0x1000, 2, 1, 0}}, false), "overflow");
}

void Flags() {
    constexpr std::uint64_t base = 0x200000;
    Asm code;
    auto movEax = [&](std::uint32_t value) { code.B({0xb8}); code.U32(value); };
    auto pushf = [&] { code.B({0x9c}); };
    movEax(0);
    code.B({0x83, 0xc0, 0x01});
    pushf();
    movEax(0);
    code.B({0x05});
    code.U32(0xffffffffu);
    pushf();
    movEax(0x7fffffff);
    code.B({0x83, 0xc0, 0x01});
    pushf();
    movEax(0);
    code.B({0x83, 0xe8, 0x01});
    pushf();
    movEax(5);
    code.B({0x83, 0xf8, 0x05});
    pushf();
    movEax(1);
    code.B({0xff, 0xc8});
    pushf();
    movEax(0);
    code.B({0x83, 0xe8, 0x01});
    code.B({0xff, 0xc0});
    pushf();
    movEax(0x0f);
    code.B({0x21, 0xc0});
    pushf();
    movEax(0);
    code.B({0x85, 0xc0});
    pushf();
    code.B({0x48, 0xb8});
    code.U64(0x8000000000000000ull);
    code.B({0x48, 0x01, 0xc0});
    pushf();
    code.B({0xbb});
    code.U32(static_cast<std::uint32_t>(base + 0x200));
    code.B({0xc7, 0x03});
    code.U32(5);
    movEax(3);
    code.B({0x03, 0x03});
    pushf();
    const auto ud2 = base + code.Bytes.size();
    code.B({0x0f, 0x0b});
    auto image = code.Bytes;
    image.resize(0x210);
    const auto run = RunGuest(Elf(base, base, image));
    Require(run.Native.Signal == SIGILL, "flags signal " + std::to_string(run.Native.Signal));
    ExpectEqual("flags status", run.Status, 0);
    ExpectEqual("flags rip", run.Rip, ud2);
    ExpectEqual("native flags rip", run.Native.Rip, ud2);
    for (int reg = 0; reg < 16; ++reg) ExpectEqual("flags gpr " + std::to_string(reg), run.Gpr[reg], run.Native.Gpr[reg]);
    ExpectEqual("flags eax", run.Gpr[0], 8);
    ExpectEqual("flags ebx", run.Gpr[3], base + 0x200);
    ExpectEqual("flags rsp", run.Gpr[4], run.Plan.StackTop - 11 * 8);
    ExpectFlags("flags register", run.Native.Flags, run.Flags, false);
    const std::uint64_t expect[] = {0x202, 0x286, 0xa96, 0x297, 0x246, 0x246, 0x257, 0x206, 0x246, 0xa47, 0x202};
    const bool logical[] = {false, false, false, false, false, false, false, true, true, false, false};
    for (int index = 0; index < 11; ++index) {
        const auto address = run.Plan.StackTop - static_cast<std::uint64_t>(index + 1) * 8;
        const auto native = At(run.NativeMemory, run.Plan.GuestBase, address);
        const auto wasm = At(run.WasmMemory, run.Plan.GuestBase, address);
        ExpectFlags("pushed flags " + std::to_string(index), native, wasm, logical[index]);
        ExpectEqual("native pushed flags " + std::to_string(index), native & (logical[index] ? kLogicalMask : kFlagMask), expect[index]);
    }
    ExpectEqual("native five", Read32(run.NativeMemory.data() + 0x200), 5);
    ExpectEqual("wasm five", Read32(run.WasmMemory.data() + 0x200), 5);
}

void GuestMemory() {
    constexpr std::uint64_t codeVA = 0x100000000ull;
    constexpr std::uint64_t dataVA = 0x100001000ull;
    constexpr std::uint64_t hole = 0x100001040ull;
    Asm code;
    code.B({0xe8});
    const auto callAt = code.Bytes.size();
    code.U32(0);
    code.B({0x49, 0x89, 0xc6});
    code.B({0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x20, 0x48, 0xb8});
    code.U64(0xaabbccddeeff0011ull);
    code.B({0x48, 0x89, 0x45, 0xf8, 0x48, 0x8b, 0x5d, 0xf8, 0x48, 0x89, 0xec, 0x5d});
    code.B({0x48, 0xbf});
    code.U64(dataVA);
    code.B({0x48, 0x8b, 0x07, 0x48, 0x8b, 0x08, 0x48, 0x8d, 0x50, 0x18});
    code.B({0x48, 0xbe});
    code.U64(0x99);
    code.B({0x48, 0x89, 0x32, 0x48, 0x83, 0xc0, 0x20, 0x4c, 0x8b, 0x00});
    code.B({0x41, 0xba});
    code.U32(2);
    code.B({0x4e, 0x8d, 0x3c, 0xd7});
    code.B({0x49, 0xb9});
    code.U64(hole);
    code.B({0x49, 0xba});
    code.U64(0x0807060504030201ull);
    code.B({0x4d, 0x89, 0x11});
    code.B({0x49, 0xbb});
    code.U64(dataVA + 0x18);
    code.B({0x48, 0xb8});
    code.U64(~std::uint64_t{0});
    code.B({0x49, 0x89, 0x03, 0x41, 0xc7, 0x03});
    code.U32(0x12345678);
    code.B({0x41, 0x8b, 0x03, 0x4d, 0x8b, 0x23, 0x4c, 0x8d, 0x2d});
    const auto leaAt = code.Bytes.size();
    code.U32(0);
    const auto ud2 = codeVA + code.Bytes.size();
    code.B({0x0f, 0x0b});
    const auto func = codeVA + code.Bytes.size();
    code.B({0xb8});
    code.U32(0x2a);
    code.B({0xc3});
    std::vector<std::uint8_t> data(0x30);
    const std::uint64_t literal = 0x1122334455667788ull;
    Put(data, 0x10, literal, 8);
    const std::vector<ElfLoad> loads = {{codeVA, 5, code.Bytes.size(), code.Bytes}, {dataVA, 6, 0x38, data}};
    const std::vector<ElfSym> symbols = {{dataVA + 0x10, 2, 0x10}, {func, 1, 0x10}};
    const std::vector<ElfRel> relocs = {
        {dataVA, 1, 1, 0},
        {dataVA + 8, 8, 0, static_cast<std::int64_t>(dataVA + 0x10)},
        {codeVA + callAt, 4, 2, -4},
        {codeVA + leaAt, 2, 1, -4},
    };
    const auto run = RunGuest(MakeElf(codeVA, 2, loads, symbols, relocs, false));
    Require(run.Native.Signal == SIGILL, "memory signal " + std::to_string(run.Native.Signal));
    ExpectEqual("memory status", run.Status, 0);
    ExpectEqual("memory rip", run.Rip, ud2);
    ExpectEqual("native memory rip", run.Native.Rip, ud2);
    ExpectSameMemory(run);
    const std::uint64_t expect[16] = {0x12345678, literal, dataVA + 0x28, 0xaabbccddeeff0011ull, run.Plan.StackTop, 0, 0x99, dataVA, 0, hole, 0x0807060504030201ull, dataVA + 0x18, 0xffffffff12345678ull, dataVA + 0x10, 0x2a, dataVA + 0x10};
    for (int reg = 0; reg < 16; ++reg) {
        ExpectEqual("native gpr " + std::to_string(reg), run.Native.Gpr[reg], expect[reg]);
        ExpectEqual("wasm gpr " + std::to_string(reg), run.Gpr[reg], expect[reg]);
    }
    ExpectEqual("native rcx value", run.Native.Gpr[1], literal);
    ExpectEqual("relocated pointer", At(run.WasmMemory, run.Plan.GuestBase, dataVA), dataVA + 0x10);
    ExpectEqual("relative pointer", At(run.WasmMemory, run.Plan.GuestBase, dataVA + 8), dataVA + 0x10);
    ExpectEqual("frame slot", At(run.WasmMemory, run.Plan.GuestBase, run.Plan.StackTop - 16), 0xaabbccddeeff0011ull);
    ExpectEqual("bss", At(run.WasmMemory, run.Plan.GuestBase, dataVA + 0x30), 0);
    ExpectFlags("memory flags", run.Native.Flags, run.Flags, false);
}

void Fault() {
    constexpr std::uint64_t base = 0x50000000;
    Asm code;
    code.B({0xb8});
    code.U32(1);
    code.B({0x48, 0xc7, 0xc3, 0x00, 0x00, 0x00, 0x00});
    const auto at = base + code.Bytes.size();
    code.B({0x48, 0x8b, 0x0b, 0x0f, 0x0b});
    const auto run = RunGuest(Elf(base, base, code.Bytes));
    Require(run.Native.Signal == SIGSEGV, "fault signal " + std::to_string(run.Native.Signal));
    ExpectEqual("fault status", run.Status, 2);
    ExpectEqual("fault rax", run.Native.Gpr[0], 1);
    ExpectEqual("wasm fault rax", run.Gpr[0], 1);
    ExpectEqual("fault rbx", run.Gpr[3], 0);
    ExpectEqual("fault rcx", run.Gpr[2], 0);
    ExpectEqual("fault rip", run.Rip, at);
    ExpectEqual("native fault rip", run.Native.Rip, at);
    ExpectFlags("fault flags", run.Native.Flags, run.Flags, false);
    ExpectSameMemory(run);
}

struct DynImport {
    std::string Name;
    std::uint64_t Slot = 0;
    std::uint32_t Type = 7;
};

std::vector<std::uint8_t> DynamicElf(std::uint64_t codeVA, const std::vector<std::uint8_t>& code, std::uint64_t dataVA, std::vector<std::uint8_t> data, const std::vector<std::string>& needed, const std::vector<DynImport>& imports) {
    std::string strings(1, '\0');
    auto addString = [&](const std::string& text) {
        const auto offset = strings.size();
        strings += text;
        strings.push_back('\0');
        return static_cast<std::uint32_t>(offset);
    };
    std::vector<std::uint32_t> neededOffsets;
    for (const auto& name : needed) neededOffsets.push_back(addString(name));
    std::vector<std::uint32_t> symbolOffsets;
    for (const auto& import : imports) symbolOffsets.push_back(addString(import.Name));
    const auto append = [&](std::size_t size) {
        const auto at = data.size();
        data.resize(at + size);
        return at;
    };
    const auto stringAt = append(strings.size());
    std::memcpy(data.data() + stringAt, strings.data(), strings.size());
    const auto symbolAt = append((imports.size() + 1) * 24);
    for (std::size_t index = 0; index < imports.size(); ++index) {
        const auto at = symbolAt + (index + 1) * 24;
        Put(data, at, symbolOffsets[index], 4);
        data[at + 4] = 0x12;
    }
    const auto relaAt = append(imports.size() * 24);
    for (std::size_t index = 0; index < imports.size(); ++index) {
        const auto at = relaAt + index * 24;
        Put(data, at, imports[index].Slot, 8);
        Put(data, at + 8, (static_cast<std::uint64_t>(index + 1) << 32) | imports[index].Type, 8);
    }
    const auto dynamicAt = append((needed.size() + 8) * 16);
    std::size_t tag = 0;
    auto dyn = [&](std::uint64_t kind, std::uint64_t value) {
        const auto at = dynamicAt + tag * 16;
        ++tag;
        Put(data, at, kind, 8);
        Put(data, at + 8, value, 8);
    };
    for (const auto offset : neededOffsets) dyn(1, offset);
    dyn(5, dataVA + stringAt);
    dyn(10, strings.size());
    dyn(6, dataVA + symbolAt);
    dyn(11, 24);
    dyn(23, dataVA + relaAt);
    dyn(2, imports.size() * 24);
    dyn(20, 7);
    dyn(0, 0);
    const std::size_t phOff = 64;
    const std::size_t codeAt = phOff + 56 * 3;
    const std::size_t dataAt = codeAt + code.size();
    std::vector<std::uint8_t> elf(dataAt + data.size());
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    Put(elf, 16, 3, 2);
    Put(elf, 18, 62, 2);
    Put(elf, 20, 1, 4);
    Put(elf, 24, codeVA, 8);
    Put(elf, 32, phOff, 8);
    Put(elf, 52, 64, 2);
    Put(elf, 54, 56, 2);
    Put(elf, 56, 3, 2);
    auto phdr = [&](std::size_t index, std::uint32_t type, std::uint32_t flags, std::uint64_t offset, std::uint64_t address, std::uint64_t filesz, std::uint64_t memsz) {
        const auto header = phOff + index * 56;
        Put(elf, header, type, 4);
        Put(elf, header + 4, flags, 4);
        Put(elf, header + 8, offset, 8);
        Put(elf, header + 16, address, 8);
        Put(elf, header + 24, address, 8);
        Put(elf, header + 32, filesz, 8);
        Put(elf, header + 40, memsz, 8);
        Put(elf, header + 48, 0x1000, 8);
    };
    phdr(0, 1, 5, codeAt, codeVA, code.size(), code.size());
    phdr(1, 1, 6, dataAt, dataVA, data.size(), data.size());
    phdr(2, 2, 6, dataAt + dynamicAt, dataVA + dynamicAt, (needed.size() + 8) * 16, (needed.size() + 8) * 16);
    std::memcpy(elf.data() + codeAt, code.data(), code.size());
    std::memcpy(elf.data() + dataAt, data.data(), data.size());
    return elf;
}

GuestRun RunWasm(const std::vector<std::uint8_t>& elf) {
    GuestRun run;
    run.Plan = Relinker::Wasm::PlanWasmImage(elf);
    static int serial = 0;
    const auto directory = std::filesystem::temp_directory_path() / ("anyps5-wasm-import-" + std::to_string(getpid()) + "-" + std::to_string(serial++));
    std::filesystem::create_directories(directory);
    const auto input = directory / "guest.elf";
    const auto output = directory / "guest.wasm";
    const auto script = directory / "run.js";
    {
        std::ofstream elfFile(input, std::ios::binary);
        elfFile.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
        std::ofstream scriptFile(script);
        scriptFile << kNodeScript;
    }
    const auto linked = Run({ANYPS5_RELINKER, "--wasm", input.string(), output.string()});
    if (linked.Status != 0) {
        std::filesystem::remove_all(directory);
        Require(false, "relinker --wasm failed: " + linked.Err + linked.Out);
    }
    const auto span = static_cast<std::size_t>(run.Plan.GuestLimit - run.Plan.GuestBase);
    const auto node = Run({ANYPS5_NODE, script.string(), output.string(), std::to_string(span)});
    std::filesystem::remove_all(directory);
    Require(node.Status == 0, "node failed: " + node.Err + node.Out);
    std::istringstream lines(node.Out);
    std::string statusText, stateText, baseText, limitText, memoryBaseText, stateHex, memoryHex;
    Require(static_cast<bool>(std::getline(lines, statusText) && std::getline(lines, stateText) && std::getline(lines, baseText) && std::getline(lines, limitText) && std::getline(lines, memoryBaseText) && std::getline(lines, stateHex) && std::getline(lines, memoryHex)), "node dump is incomplete");
    run.Status = static_cast<int>(std::stoll(statusText));
    ExpectEqual("state", std::stoull(stateText), Relinker::Wasm::kStateAddress);
    ExpectEqual("guest base", std::stoull(baseText), run.Plan.GuestBase);
    ExpectEqual("guest limit", std::stoull(limitText), run.Plan.GuestLimit);
    run.MemoryBase = std::stoull(memoryBaseText);
    run.State = FromHex(stateHex);
    run.WasmMemory = FromHex(memoryHex);
    Require(run.State.size() == Relinker::Wasm::kStateBytes, "state size");
    Require(run.WasmMemory.size() == span, "guest memory size");
    for (int index = 0; index < 16; ++index) run.Gpr[index] = Read64(run.State.data() + index * 8);
    run.Rip = Read64(run.State.data() + Relinker::Wasm::kRipOffset);
    run.Flags = Read64(run.State.data() + Relinker::Wasm::kRflagsOffset);
    return run;
}

const Relinker::Wasm::WasmImport* FindImport(const Relinker::Wasm::WasmImage& image, const std::string& name) {
    for (const auto& import : image.Imports) if (import.Name == name) return &import;
    return nullptr;
}

std::size_t CountText(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
    return count;
}

std::size_t CountGprLoads(const std::string& text) {
    std::size_t count = 0;
    const std::string key = "load i64, ptr inttoptr (i32 ";
    for (std::size_t at = 0; (at = text.find(key, at)) != std::string::npos; at += key.size()) {
        const auto value = std::stoull(text.substr(at + key.size()));
        if (value >= Relinker::Wasm::kStateAddress && value < Relinker::Wasm::kStateAddress + Relinker::Wasm::kGprBytes) ++count;
    }
    return count;
}

std::string ModuleText(const std::vector<std::uint8_t>& elf) {
    const auto image = Relinker::Wasm::PlanWasmImage(elf);
    return Relinker::Wasm::EmitWasmModule(image, Relinker::Wasm::DiscoverWasmBlocks(image));
}

void Optimized() {
    {
        Asm code;
        code.B({0xbb});
        code.U32(7);
        code.B({0x89, 0xd8, 0x01, 0xd8, 0x89, 0xc1, 0x83, 0xc1, 0x01, 0x0f, 0x0b});
        const auto elf = Elf(0x340000, 0x340000, code.Bytes);
        const auto ir = ModuleText(elf);
        ExpectEqual("promoted flag calls", CountText(ir, "call i64 @anyps5_flags"), 1);
        ExpectEqual("promoted gpr loads", CountGprLoads(ir), 0);
        ExpectEqual("promoted dispatch edges", CountText(ir, "br label %dispatch"), 0);
        const auto run = RunGuest(elf);
        Require(run.Native.Signal == SIGILL, "promoted signal");
        ExpectEqual("promoted status", run.Status, 0);
        ExpectEqual("promoted eax", run.Gpr[0], 14);
        ExpectEqual("promoted ebx", run.Gpr[3], 7);
        ExpectEqual("promoted ecx", run.Gpr[1], 15);
        ExpectEqual("native promoted eax", run.Native.Gpr[0], 14);
        ExpectEqual("native promoted ecx", run.Native.Gpr[1], 15);
        ExpectFlags("promoted flags", run.Native.Flags, run.Flags, false);
    }
    {
        Asm code;
        code.B({0x48, 0xb8});
        code.U64(0x100000001ull);
        code.B({0x83, 0xc0, 0x01, 0x48, 0x89, 0xc1});
        code.B({0x48, 0xb8});
        code.U64(0x100000001ull);
        code.B({0x89, 0xc2, 0x48, 0x83, 0xc0, 0x01, 0x0f, 0x0b});
        const auto run = RunGuest(Elf(0x330000, 0x330000, code.Bytes));
        Require(run.Native.Signal == SIGILL, "extend signal");
        ExpectEqual("extend status", run.Status, 0);
        ExpectEqual("extend rax", run.Gpr[0], 0x100000002ull);
        ExpectEqual("extend rcx", run.Gpr[1], 2);
        ExpectEqual("extend rdx", run.Gpr[2], 1);
        ExpectEqual("native extend rax", run.Native.Gpr[0], run.Gpr[0]);
        ExpectEqual("native extend rcx", run.Native.Gpr[1], run.Gpr[1]);
        ExpectEqual("native extend rdx", run.Native.Gpr[2], run.Gpr[2]);
    }
    {
        constexpr std::uint64_t base = 0x310000;
        Asm code;
        code.B({0xb8});
        code.U32(1);
        code.B({0xe8, 0x05, 0x00, 0x00, 0x00, 0x83, 0xc0, 0x01, 0x0f, 0x0b, 0x83, 0xc0, 0x01, 0xc3});
        const auto elf = Elf(base, base, code.Bytes);
        const auto ir = ModuleText(elf);
        ExpectEqual("direct switches", CountText(ir, "switch i64"), 2);
        ExpectEqual("direct dispatch edges", CountText(ir, "br label %dispatch"), 1);
        Require(ir.find("br label %g") != std::string::npos, "known call did not branch directly");
        const auto run = RunGuest(elf);
        Require(run.Native.Signal == SIGILL, "direct signal");
        ExpectEqual("direct status", run.Status, 0);
        ExpectEqual("direct eax", run.Gpr[0], 3);
        ExpectEqual("native direct eax", run.Native.Gpr[0], 3);
        ExpectEqual("direct rip", run.Rip, base + 13);
        ExpectFlags("direct flags", run.Native.Flags, run.Flags, false);
    }
    {
        constexpr std::uint64_t base = 0x360000;
        Asm code;
        code.B({0xb8});
        code.U32(0);
        code.B({0x83, 0xf8, 0x01});
        const auto jeAt = code.Bytes.size();
        code.B({0x74, 0x00});
        const auto callAt = code.Bytes.size();
        code.B({0xe8, 0, 0, 0, 0});
        const auto normal = code.Bytes.size();
        code.B({0xb8});
        code.U32(2);
        code.B({0x0f, 0x0b});
        const auto land = code.Bytes.size();
        code.B({0xb8});
        code.U32(1);
        const auto landUd2 = code.Bytes.size();
        code.B({0x0f, 0x0b});
        const auto foo = code.Bytes.size();
        code.B({0x48, 0x83, 0x04, 0x24, static_cast<std::uint8_t>(land - normal), 0xc3});
        code.Bytes[jeAt + 1] = static_cast<std::uint8_t>(land - (jeAt + 2));
        const auto displacement = static_cast<std::uint32_t>(foo - (callAt + 5));
        for (int index = 0; index < 4; ++index) code.Bytes[callAt + 1 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(displacement >> (8 * index));
        const auto run = RunGuest(Elf(base, base, code.Bytes));
        Require(run.Native.Signal == SIGILL, "fallback signal");
        ExpectEqual("fallback status", run.Status, 0);
        ExpectEqual("fallback eax", run.Gpr[0], 1);
        ExpectEqual("native fallback eax", run.Native.Gpr[0], 1);
        ExpectEqual("fallback rip", run.Rip, base + landUd2);
        ExpectEqual("native fallback rip", run.Native.Rip, base + landUd2);
    }
    {
        auto image = DecodeHex("b905000000b80000000089c789cee84a000000ffc975f383f80f753aeb020f0b83e8017c3183c00189c2660f6ec266480f7ec048c7c300010100488903488b034883c001488943084c8d43084c8d0c13660fefc90f0bb8ee0000000f0b89f801f0c3");
        image.resize(0x120);
        const auto ir = ModuleText(Elf(0x10000, 0x10000, image));
        ExpectEqual("milestone flag calls", CountText(ir, "call i64 @anyps5_flags"), 5);
        ExpectEqual("milestone rbx state", CountText(ir, "983064"), 2);
        ExpectEqual("milestone dispatch edges", CountText(ir, "br label %dispatch"), 1);
        ExpectEqual("milestone switches", CountText(ir, "switch i64"), 2);
        Require(ir.find("br label %g1005d") != std::string::npos, "milestone call was not direct");
        Require(ir.find("i64 65555, label %g10013") != std::string::npos, "milestone return was not direct");
    }
    bool threw = false;
    try {
        (void)Relinker::Wasm::CompileWasm(Elf(0x10000, 0x10000, {0xff, 0xd0}));
    } catch (const std::exception& error) {
        threw = true;
        Require(std::string(error.what()).find("indirect") != std::string::npos, std::string("unexpected reject: ") + error.what());
    }
    Require(threw, "indirect call was lifted");
}

void Imports() {
    constexpr std::uint64_t codeVA = 0x10000;
    constexpr std::uint64_t dataVA = 0x11000;
    constexpr std::uint64_t src = dataVA;
    constexpr std::uint64_t dest = dataVA + 0x10;
    constexpr std::uint64_t dest2 = dataVA + 0x20;
    constexpr std::uint64_t results = dataVA + 0x30;
    constexpr std::uint64_t got = dataVA + 0x60;
    Asm code;
    auto movabs = [&](std::uint8_t reg, std::uint64_t value) {
        code.B({static_cast<std::uint8_t>(reg < 8 ? 0x48 : 0x49), static_cast<std::uint8_t>(0xb8 + (reg & 7))});
        code.U64(value);
    };
    auto storeRax = [&] { code.B({0x48, 0x89, 0x01}); };
    auto callGot = [&](std::uint64_t slot) {
        code.B({0xff, 0x15});
        const auto next = codeVA + code.Bytes.size() + 4;
        code.U32(static_cast<std::uint32_t>(static_cast<std::int64_t>(slot) - static_cast<std::int64_t>(next)));
    };
    movabs(7, dest);
    movabs(6, src);
    movabs(2, 2);
    callGot(got);
    movabs(1, results);
    storeRax();
    movabs(7, dest2);
    movabs(6, 0x61);
    movabs(2, 4);
    callGot(got + 8);
    movabs(1, results + 8);
    storeRax();
    movabs(7, src);
    callGot(got + 16);
    movabs(1, results + 16);
    storeRax();
    const auto firstCall = code.Bytes.size();
    code.B({0xe8, 0, 0, 0, 0});
    movabs(1, results + 24);
    storeRax();
    const auto secondCall = code.Bytes.size();
    code.B({0xe8, 0, 0, 0, 0});
    movabs(1, results + 32);
    storeRax();
    const auto ud2 = codeVA + code.Bytes.size();
    code.B({0x0f, 0x0b});
    const auto plt = codeVA + code.Bytes.size();
    auto patch = [&](std::size_t at) {
        const auto next = codeVA + at + 5;
        const auto displacement = static_cast<std::uint32_t>(static_cast<std::int64_t>(plt) - static_cast<std::int64_t>(next));
        for (int index = 0; index < 4; ++index) code.Bytes[at + 1 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(displacement >> (8 * index));
    };
    patch(firstCall);
    patch(secondCall);
    code.B({0xff, 0x25});
    const auto jmpNext = codeVA + code.Bytes.size() + 4;
    code.U32(static_cast<std::uint32_t>(static_cast<std::int64_t>(got + 24) - static_cast<std::int64_t>(jmpNext)));
    std::vector<std::uint8_t> data(0x80);
    data[0] = 'h';
    data[1] = 'i';
    const std::vector<std::string> needed = {"libSceLibcInternal.sprx", "libkernel.sprx"};
    const std::vector<DynImport> imports = {{"memcpy", got}, {"memset", got + 8}, {"strlen", got + 16}, {"sceKernelGetProcessTime", got + 24}};
    const auto elf = DynamicElf(codeVA, code.Bytes, dataVA, data, needed, imports);
    const auto plan = Relinker::Wasm::PlanWasmImage(elf);
    Require(plan.Imports.size() == 4, "import count");
    for (const auto& import : imports) {
        const auto* bound = FindImport(plan, import.Name);
        Require(bound != nullptr, "missing import " + import.Name);
        Require(bound->Nid == Nid::ComputeNid(import.Name, ""), "nid of " + import.Name);
        const auto& segment = plan.Segments[1];
        ExpectEqual(import.Name + " slot", Read64(segment.Bytes.data() + static_cast<std::size_t>(import.Slot - dataVA)), bound->Entry);
    }
    const auto run = RunWasm(elf);
    ExpectEqual("import status", run.Status, 0);
    ExpectEqual("import rip", run.Rip, ud2);
    ExpectEqual("import rsp", run.Gpr[4], run.Plan.StackTop);
    const auto memoryAt = dataVA - run.Plan.GuestBase;
    Require(run.WasmMemory[memoryAt + 0x10] == 'h' && run.WasmMemory[memoryAt + 0x11] == 'i' && run.WasmMemory[memoryAt + 0x12] == 0, "memcpy bytes");
    Require(run.WasmMemory[memoryAt + 0x20] == 'a' && run.WasmMemory[memoryAt + 0x21] == 'a' && run.WasmMemory[memoryAt + 0x22] == 'a' && run.WasmMemory[memoryAt + 0x23] == 'a', "memset bytes");
    ExpectEqual("memcpy result", At(run.WasmMemory, run.Plan.GuestBase, results), dest);
    ExpectEqual("memset result", At(run.WasmMemory, run.Plan.GuestBase, results + 8), dest2);
    ExpectEqual("strlen result", At(run.WasmMemory, run.Plan.GuestBase, results + 16), 2);
    const auto first = At(run.WasmMemory, run.Plan.GuestBase, results + 24);
    const auto second = At(run.WasmMemory, run.Plan.GuestBase, results + 32);
    Require(second >= first && second < 10000000, "process time was not a short monotonic microsecond count");
    const auto memcpyNid = Nid::ComputeNid("memcpy", "");
    const auto strlenNid = Nid::ComputeNid("strlen", "");
    const auto nidElf = DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {}, {{memcpyNid, dataVA, 6}});
    const auto nidPlan = Relinker::Wasm::PlanWasmImage(nidElf);
    const auto* nidImport = FindImport(nidPlan, memcpyNid);
    Require(nidImport != nullptr && nidImport->Kind == static_cast<std::uint16_t>(Relinker::Wasm::WasmImportKind::Memcpy), "raw NID import");
    ExpectEqual("glob dat", Read64(nidPlan.Segments[1].Bytes.data()), nidImport->Entry);
    const auto qualified = strlenNid + "#libSceLibcInternal#A";
    const auto qualifiedElf = DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {}, {{qualified, dataVA}});
    const auto* qualifiedImport = FindImport(Relinker::Wasm::PlanWasmImage(qualifiedElf), qualified);
    Require(qualifiedImport != nullptr && qualifiedImport->Nid == strlenNid && qualifiedImport->Kind == static_cast<std::uint16_t>(Relinker::Wasm::WasmImportKind::Strlen), "qualified NID import");
    Throws(DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {"libc.so.6"}, {{"memcpy", dataVA}}), "undefined");
    Throws(DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {"libSceLibcInternal.sprx"}, {{"malloc", dataVA}}), "undefined");
    Throws(DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {}, {{"memcpy#only", dataVA}}), "qualified");
    Throws(DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {}, {{"memcpy#lib#*", dataVA}}), "module ID");
}

const char* kGraphicsScript = R"(
const fs = require('fs');
const bytes = fs.readFileSync(process.argv[2]);
const result = process.argv[3] === undefined ? 0 : Number(process.argv[3]);
const slot = { memory: null, seen: [] };
const imports = { env: { anyps5_webgpu_submit(ptr, len) {
  const view = new Uint8Array(slot.memory.buffer, ptr, len);
  let text = '';
  for (let index = 0; index < view.length; index += 1) text += view[index].toString(16).padStart(2, '0');
  slot.seen.push(text);
  return result;
}}};
const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), imports);
slot.memory = instance.exports.memory;
const status = instance.exports.anyps5_run();
const state = instance.exports.anyps5_state();
const memory = new Uint8Array(instance.exports.memory.buffer);
function hex(start, length) {
  let text = '';
  for (let index = 0; index < length; index += 1) text += memory[start + index].toString(16).padStart(2, '0');
  return text;
}
process.stdout.write(String(status) + '\n' + slot.seen.join(',') + '\n' + hex(state, 400) + '\n');
)";

struct GraphicsGuest {
    std::vector<std::uint8_t> Elf;
    std::vector<std::uint8_t> Command;
    std::uint64_t Ud2 = 0;
};

GraphicsGuest MakeGraphics(std::uint32_t opcode, std::uint32_t width, std::uint32_t height, std::uint32_t rgba, std::uint32_t shader, std::uint64_t address, std::uint64_t bytes) {
    constexpr std::uint64_t codeVA = 0x10000;
    constexpr std::uint64_t dataVA = 0x11000;
    constexpr std::uint64_t got = dataVA + 0x20;
    Asm code;
    auto movabs = [&](std::uint8_t reg, std::uint64_t value) {
        code.B({static_cast<std::uint8_t>(reg < 8 ? 0x48 : 0x49), static_cast<std::uint8_t>(0xb8 + (reg & 7))});
        code.U64(value);
    };
    movabs(7, address);
    movabs(6, bytes);
    code.B({0xff, 0x15});
    const auto next = codeVA + code.Bytes.size() + 4;
    code.U32(static_cast<std::uint32_t>(static_cast<std::int64_t>(got) - static_cast<std::int64_t>(next)));
    GraphicsGuest guest;
    guest.Ud2 = codeVA + code.Bytes.size();
    code.B({0x0f, 0x0b});
    std::vector<std::uint8_t> data(0x28);
    Put(data, 0, opcode, 4);
    Put(data, 4, width, 4);
    Put(data, 8, height, 4);
    Put(data, 12, rgba, 4);
    Put(data, 16, shader, 4);
    guest.Command.assign(data.begin(), data.begin() + 20);
    guest.Elf = DynamicElf(codeVA, code.Bytes, dataVA, data, {"libSceLibcInternal.sprx"}, {{"anyps5_webgpu_submit", got}});
    return guest;
}

struct HostSeen {
    int Status = 0;
    std::string Seen;
    std::uint64_t Rax = 0;
    std::uint64_t Rip = 0;
    std::uint64_t Rsp = 0;
};

HostSeen RunHost(const std::filesystem::path& directory, const std::vector<std::uint8_t>& elf, const std::string& result) {
    const auto input = directory / "guest.elf";
    const auto output = directory / "guest.wasm";
    const auto script = directory / "run.js";
    {
        std::ofstream elfFile(input, std::ios::binary);
        elfFile.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
        std::ofstream scriptFile(script);
        scriptFile << kGraphicsScript;
    }
    const auto linked = Run({ANYPS5_RELINKER, "--wasm", input.string(), output.string()});
    Require(linked.Status == 0, "relinker --wasm failed: " + linked.Err + linked.Out);
    const auto node = Run({ANYPS5_NODE, script.string(), output.string(), result});
    Require(node.Status == 0, "graphics host failed: " + node.Err + node.Out);
    std::istringstream lines(node.Out);
    std::string statusText, seen, stateHex;
    Require(static_cast<bool>(std::getline(lines, statusText) && std::getline(lines, seen) && std::getline(lines, stateHex)), "graphics dump is incomplete");
    const auto state = FromHex(stateHex);
    Require(state.size() == Relinker::Wasm::kStateBytes, "graphics state size");
    HostSeen host;
    host.Status = static_cast<int>(std::stoll(statusText));
    host.Seen = seen;
    host.Rax = Read64(state.data());
    host.Rsp = Read64(state.data() + 32);
    host.Rip = Read64(state.data() + Relinker::Wasm::kRipOffset);
    return host;
}

std::filesystem::path CompileGraphics(const std::filesystem::path& directory, const std::string& name, const std::vector<std::uint8_t>& elf) {
    const auto input = directory / (name + ".elf");
    const auto output = directory / (name + ".wasm");
    std::ofstream elfFile(input, std::ios::binary);
    elfFile.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
    elfFile.close();
    const auto linked = Run({ANYPS5_RELINKER, "--wasm", input.string(), output.string()});
    Require(linked.Status == 0, "relinker --wasm failed: " + linked.Err + linked.Out);
    return output;
}

std::string FindChrome() {
    const char* names[] = {"google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "chrome"};
    for (const char* name : names) {
        const auto found = Run({"/usr/bin/env", "sh", "-c", std::string("command -v ") + name});
        if (found.Status != 0) continue;
        std::string path = found.Out;
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ')) path.pop_back();
        if (!path.empty()) return path;
    }
    return {};
}

void ExpectPixels(const std::string& out, const std::string& rgba) {
    Require(out.find("status 0") != std::string::npos, out);
    Require(out.find("off " + rgba) != std::string::npos, out);
    Require(out.find("corner " + rgba) != std::string::npos, out);
    Require(out.find("canvas presented") != std::string::npos, out);
    Require(out.find("err none") != std::string::npos, out);
}

void WebGpu() {
    const auto checked = Run({ANYPS5_NODE, ANYPS5_WEBGPU_RUN, "check"});
    Require(checked.Status == 0 && checked.Out.find("ok") != std::string::npos, "shader check failed: " + checked.Err + checked.Out);
    constexpr std::uint64_t codeVA = 0x10000;
    constexpr std::uint64_t dataVA = 0x11000;
    const auto clear = MakeGraphics(1, 2, 2, 0xff0000ffu, 0, dataVA, 20);
    const auto plan = Relinker::Wasm::PlanWasmImage(clear.Elf);
    const auto* bound = FindImport(plan, "anyps5_webgpu_submit");
    Require(bound != nullptr && bound->Kind == static_cast<std::uint16_t>(Relinker::Wasm::WasmImportKind::Graphics), "graphics import");
    Require(bound->Nid == Nid::ComputeNid("anyps5_webgpu_submit", ""), "graphics nid");
    ExpectEqual("graphics slot", Read64(plan.Segments[1].Bytes.data() + 0x20), bound->Entry);
    const auto graphicsNid = Nid::ComputeNid("anyps5_webgpu_submit", "");
    const auto nidElf = DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {}, {{graphicsNid, dataVA, 6}});
    const auto* nidImport = FindImport(Relinker::Wasm::PlanWasmImage(nidElf), graphicsNid);
    Require(nidImport != nullptr && nidImport->Kind == static_cast<std::uint16_t>(Relinker::Wasm::WasmImportKind::Graphics), "raw graphics NID");
    const auto qualified = graphicsNid + "#libSceLibcInternal#A";
    const auto* qualifiedImport = FindImport(Relinker::Wasm::PlanWasmImage(DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {}, {{qualified, dataVA}})), qualified);
    Require(qualifiedImport != nullptr && qualifiedImport->Kind == static_cast<std::uint16_t>(Relinker::Wasm::WasmImportKind::Graphics), "qualified graphics import");
    Throws(DynamicElf(codeVA, {0x0f, 0x0b}, dataVA, std::vector<std::uint8_t>(8), {"libSceLibcInternal.sprx"}, {{"anyps5_webgpu_draw", dataVA}}), "undefined");
    const auto directory = std::filesystem::temp_directory_path() / ("anyps5-wasm-webgpu-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto accepted = RunHost(directory, clear.Elf, "0");
    ExpectEqual("graphics status", accepted.Status, 0);
    Require(accepted.Seen == Hex(clear.Command.data(), clear.Command.size()), "graphics command " + accepted.Seen);
    ExpectEqual("graphics rip", accepted.Rip, clear.Ud2);
    ExpectEqual("graphics rax", accepted.Rax, 0);
    ExpectEqual("graphics rsp", accepted.Rsp, plan.StackTop);
    const auto rejected = RunHost(directory, clear.Elf, "-1");
    ExpectEqual("graphics reject", rejected.Status, 2);
    Require(rejected.Seen == accepted.Seen, "rejected command was not submitted");
    ExpectEqual("graphics reject rip", rejected.Rip, bound->Entry);
    ExpectEqual("graphics reject rax", rejected.Rax, 0);
    const auto shortGuest = MakeGraphics(1, 2, 2, 0xff0000ffu, 0, dataVA, 4);
    const auto shortPlan = Relinker::Wasm::PlanWasmImage(shortGuest.Elf);
    const auto shortRun = RunHost(directory, shortGuest.Elf, "0");
    ExpectEqual("graphics short", shortRun.Status, 2);
    Require(shortRun.Seen.empty(), "short command reached the host");
    ExpectEqual("graphics short rip", shortRun.Rip, shortPlan.Imports[0].Entry);
    const auto outside = MakeGraphics(1, 2, 2, 0xff0000ffu, 0, 0, 20);
    const auto outsidePlan = Relinker::Wasm::PlanWasmImage(outside.Elf);
    const auto outsideRun = RunHost(directory, outside.Elf, "0");
    ExpectEqual("graphics outside", outsideRun.Status, 2);
    Require(outsideRun.Seen.empty(), "outside command reached the host");
    ExpectEqual("graphics outside rip", outsideRun.Rip, outsidePlan.Imports[0].Entry);
    const auto chrome = FindChrome();
    if (chrome.empty()) {
        std::filesystem::remove_all(directory);
        std::cerr << "webgpu browser test skipped: chrome not found\n";
        return;
    }
    const auto draw = MakeGraphics(2, 4, 4, 0xffff0000u, 1, dataVA, 20);
    const auto unknown = MakeGraphics(9, 2, 2, 0xff0000ffu, 0, dataVA, 20);
    const auto clearWasm = CompileGraphics(directory, "clear", clear.Elf);
    const auto drawWasm = CompileGraphics(directory, "draw", draw.Elf);
    const auto unknownWasm = CompileGraphics(directory, "unknown", unknown.Elf);
    const auto cleared = Run({ANYPS5_NODE, ANYPS5_WEBGPU_RUN, "browser", chrome, clearWasm.string()});
    Require(cleared.Status == 0, "clear browser failed: " + cleared.Err + cleared.Out);
    ExpectPixels(cleared.Out, "255,0,0,255");
    const auto drawn = Run({ANYPS5_NODE, ANYPS5_WEBGPU_RUN, "browser", chrome, drawWasm.string()});
    Require(drawn.Status == 0, "draw browser failed: " + drawn.Err + drawn.Out);
    ExpectPixels(drawn.Out, "0,0,255,255");
    const auto faulted = Run({ANYPS5_NODE, ANYPS5_WEBGPU_RUN, "browser", chrome, unknownWasm.string()});
    Require(faulted.Status == 0, "unknown browser failed: " + faulted.Err + faulted.Out);
    Require(faulted.Out.find("fault 2") != std::string::npos && faulted.Out.find("unknown webgpu opcode") != std::string::npos, faulted.Out);
    std::filesystem::remove_all(directory);
}

}

int main() {
    try {
        ArmSignals();
        Unsupported();
        Milestone();
        RelocationBytes();
        RelocationRejects();
        Flags();
        GuestMemory();
        Fault();
        Optimized();
        Imports();
        WebGpu();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
