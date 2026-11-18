#include <wasm/Codegen.hpp>
#include <wasm/WasmTarget.hpp>
#include <domain/Types.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Relinker::Wasm {
namespace {

[[noreturn]] void Fail(const std::string& message) {
    throw Domain::RelinkerException(message);
}

std::string ReadTail(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.size() > 2000) text.erase(0, text.size() - 2000);
    return text;
}

#ifdef _WIN32
std::string Quote(const std::string& argument) {
    if (argument.find_first_of(" \t\"") == std::string::npos) return argument;
    std::string quoted = "\"";
    unsigned backslashes = 0;
    for (const char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        if (character == '"') quoted.append(backslashes * 2 + 1, '\\');
        else if (backslashes != 0) quoted.append(backslashes, '\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

int Run(const std::vector<std::string>& arguments, const std::filesystem::path& log) {
    std::string command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command.push_back(' ');
        command += Quote(argument);
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE file = CreateFileA(log.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) Fail("wasm: cannot record the backend log");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = file;
    startup.hStdError = file;
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(file);
        Fail("wasm: cannot run " + arguments.front());
    }
    CloseHandle(file);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(status);
}
#else
int Run(const std::vector<std::string>& arguments, const std::filesystem::path& log) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) Fail("wasm: cannot start the backend");
    const auto path = log.string();
    const bool opened = posix_spawn_file_actions_addopen(&actions, 1, path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0
        && posix_spawn_file_actions_adddup2(&actions, 1, 2) == 0;
    pid_t pid = 0;
    const int spawned = opened ? posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ) : -1;
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) Fail("wasm: cannot run " + arguments.front() + " (" + std::strerror(spawned > 0 ? spawned : errno) + ")");
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) Fail("wasm: " + arguments.front() + " did not exit");
    return WEXITSTATUS(status);
}
#endif

std::string FindTool(std::initializer_list<const char*> names) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) Fail("wasm: PATH is not set");
    const std::string paths(path);
#ifdef _WIN32
    const char separator = ';';
    const char* suffix = ".exe";
#else
    const char separator = ':';
    const char* suffix = "";
#endif
    for (const char* name : names) {
        std::size_t start = 0;
        while (start <= paths.size()) {
            const auto end = paths.find(separator, start);
            const auto directory = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!directory.empty()) {
                const auto candidate = std::filesystem::path(directory) / (std::string(name) + suffix);
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate, error)) return candidate.string();
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    std::string list;
    for (const char* name : names) {
        if (!list.empty()) list += " or ";
        list += name;
    }
    Fail("wasm: cannot find " + list + " on PATH");
    return {};
}

std::filesystem::path MakeDirectory() {
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto path = std::filesystem::temp_directory_path() / ("anyps5-wasm-" + std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) return path;
    }
    Fail("wasm: cannot create a temporary directory");
    return {};
}

struct TemporaryDirectory {
    std::filesystem::path Path;
    explicit TemporaryDirectory(std::filesystem::path path) : Path(std::move(path)) {}
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(Path, error);
    }
};

void Require(int status, const std::string& tool, const std::filesystem::path& log) {
    if (status == 0) return;
    const auto detail = ReadTail(log);
    Fail("wasm: " + tool + " failed" + (detail.empty() ? "" : ": " + detail));
}

}

std::vector<std::uint8_t> LinkWasmModule(const std::string& ir) {
    const TemporaryDirectory directory(MakeDirectory());
    const auto source = directory.Path / "module.ll";
    const auto object = directory.Path / "module.o";
    const auto binary = directory.Path / "module.wasm";
    const auto log = directory.Path / "backend.log";
    {
        std::ofstream output(source, std::ios::binary);
        if (!output) Fail("wasm: cannot write LLVM IR");
        output << ir;
    }
    const auto clang = FindTool({"clang", "clang-18"});
    const auto linker = FindTool({"wasm-ld", "wasm-ld-18"});
    Require(Run({clang, "--target=wasm32", "-O0", "-c", "-o", object.string(), source.string()}, log), "clang", log);
    Require(Run({linker, "--no-entry", "--export=anyps5_run", "--export=anyps5_state", "--export=anyps5_guest_base",
        "--export=anyps5_guest_limit", "--export=anyps5_memory_base", "--export-memory", "--initial-memory=" + std::to_string(kMemoryBytes),
        "--global-base=" + std::to_string(kGlobalBase), "-o", binary.string(), object.string()}, log), "wasm-ld", log);
    std::ifstream input(binary, std::ios::binary);
    if (!input) Fail("wasm: cannot read the WebAssembly module");
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}
