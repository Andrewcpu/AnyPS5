#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <wasm/WasmTarget.hpp>
#include <domain/Types.hpp>
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
const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes));
const status = instance.exports.anyps5_run();
const state = instance.exports.anyps5_state();
const base = instance.exports.anyps5_guest_base();
const limit = instance.exports.anyps5_guest_limit();
const memory = new Uint8Array(instance.exports.memory.buffer);
function hex(start, length) {
  let text = '';
  for (let index = 0; index < length; index += 1) text += memory[start + index].toString(16).padStart(2, '0');
  return text;
}
process.stdout.write(String(status) + '\n' + String(state) + '\n' + String(base) + '\n' + String(limit) + '\n' + hex(state, 400) + '\n' + hex(base, limit - base) + '\n');
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

    ArmSignals();
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

    const auto node = Run({ANYPS5_NODE, script.string(), output.string()});
    Require(node.Status == 0, "node failed: " + node.Err);
    std::istringstream lines(node.Out);
    std::string statusText, stateText, baseText, limitText, stateHex, memoryHex;
    Require(static_cast<bool>(std::getline(lines, statusText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, stateText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, baseText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, limitText)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, stateHex)), "node dump is incomplete");
    Require(static_cast<bool>(std::getline(lines, memoryHex)), "node dump is incomplete");
    ExpectEqual("wasm status", std::stoull(statusText), 0);
    ExpectEqual("state", std::stoull(stateText), Relinker::Wasm::kStateAddress);
    ExpectEqual("guest base", std::stoull(baseText), plan.GuestBase);
    ExpectEqual("guest limit", std::stoull(limitText), plan.GuestLimit);
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

}

int main() {
    try {
        Unsupported();
        Milestone();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
