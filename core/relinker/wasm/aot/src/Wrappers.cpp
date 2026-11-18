#include <wasm/Emit.hpp>
#include <string>

namespace Relinker::Wasm {
namespace {

std::string Number(std::uint64_t value) {
    return std::to_string(static_cast<std::int64_t>(value));
}

std::string State(std::uint64_t offset) {
    return "inttoptr (i32 " + std::to_string(kStateAddress + offset) + " to ptr)";
}

std::string Gpr(int reg) {
    return State(static_cast<std::uint64_t>(reg) * 8);
}

std::string Scratch(std::uint64_t offset) {
    return "inttoptr (i32 " + std::to_string(kScratchAddress + offset) + " to ptr)";
}

std::string Helpers(std::uint64_t guestBase, std::uint64_t guestLimit) {
    const auto base = Number(guestBase);
    const auto limit = Number(guestLimit);
    std::string text;
    text += "define internal i32 @anyps5_span_ok(i64 %addr, i64 %size) {\n";
    text += "entry:\n  %zero = icmp eq i64 %size, 0\n  br i1 %zero, label %ok, label %check\n";
    text += "check:\n  %end = add i64 %addr, %size\n  %wrap = icmp ult i64 %end, %addr\n";
    text += "  %below = icmp ult i64 %addr, " + base + "\n  %above = icmp ugt i64 %end, " + limit + "\n";
    text += "  %low = or i1 %wrap, %below\n  %image = or i1 %low, %above\n";
    text += "  %direct = load i64, ptr " + Scratch(kHostDirectEnd) + ", align 1\n";
    text += "  %heap = icmp ult i64 %addr, " + Number(guestBase + kMemoryBytes) + "\n";
    text += "  %past = icmp ugt i64 %end, %direct\n  %dlow = or i1 %wrap, %heap\n  %outside = or i1 %dlow, %past\n";
    text += "  %bad = and i1 %image, %outside\n  br i1 %bad, label %fail, label %ok\n";
    text += "ok:\n  ret i32 0\nfail:\n  ret i32 1\n}\n";
    text += "define internal i32 @anyps5_guest_return() {\nentry:\n";
    text += "  %rsp = load i64, ptr " + Gpr(kRsp) + ", align 1\n";
    text += "  %bad = call i32 @anyps5_span_ok(i64 %rsp, i64 8)\n  %faulted = icmp ne i32 %bad, 0\n";
    text += "  br i1 %faulted, label %fail, label %ok\nok:\n";
    text += "  %wasm = call i32 @anyps5_translate(i64 %rsp)\n  %next = call i64 @anyps5_read64(i32 %wasm)\n";
    text += "  %raised = add i64 %rsp, 8\n  store i64 %raised, ptr " + Gpr(kRsp) + ", align 1\n";
    text += "  store i64 %next, ptr " + State(kRipOffset) + ", align 1\n  ret i32 0\nfail:\n  ret i32 1\n}\n";
    text += "define internal i32 @anyps5_copy(i64 %dst, i64 %src, i64 %n) {\nentry:\n";
    text += "  %dstbad = call i32 @anyps5_span_ok(i64 %dst, i64 %n)\n  %srcbad = call i32 @anyps5_span_ok(i64 %src, i64 %n)\n";
    text += "  %db = icmp ne i32 %dstbad, 0\n  %sb = icmp ne i32 %srcbad, 0\n  %bad = or i1 %db, %sb\n";
    text += "  br i1 %bad, label %fail, label %loop\n";
    text += "loop:\n  %index = phi i64 [ 0, %entry ], [ %next, %body ]\n  %done = icmp uge i64 %index, %n\n";
    text += "  br i1 %done, label %ok, label %body\nbody:\n  %from = add i64 %src, %index\n  %to = add i64 %dst, %index\n";
    text += "  %fromw = call i32 @anyps5_translate(i64 %from)\n  %tow = call i32 @anyps5_translate(i64 %to)\n";
    text += "  %byte = call i32 @anyps5_read8(i32 %fromw)\n  call void @anyps5_write8(i32 %tow, i32 %byte)\n";
    text += "  %next = add i64 %index, 1\n  br label %loop\nok:\n  ret i32 0\nfail:\n  ret i32 1\n}\n";
    text += "define internal i32 @anyps5_fill(i64 %dst, i32 %byte, i64 %n) {\nentry:\n";
    text += "  %bad = call i32 @anyps5_span_ok(i64 %dst, i64 %n)\n  %faulted = icmp ne i32 %bad, 0\n";
    text += "  br i1 %faulted, label %fail, label %loop\n";
    text += "loop:\n  %index = phi i64 [ 0, %entry ], [ %next, %body ]\n  %done = icmp uge i64 %index, %n\n";
    text += "  br i1 %done, label %ok, label %body\nbody:\n  %to = add i64 %dst, %index\n";
    text += "  %tow = call i32 @anyps5_translate(i64 %to)\n  call void @anyps5_write8(i32 %tow, i32 %byte)\n";
    text += "  %next = add i64 %index, 1\n  br label %loop\nok:\n  ret i32 0\nfail:\n  ret i32 1\n}\n";
    text += "define internal i64 @anyps5_length(i64 %addr) {\nentry:\n  br label %loop\n";
    text += "loop:\n  %cursor = phi i64 [ %addr, %entry ], [ %next, %more ]\n";
    text += "  %span = call i32 @anyps5_span_ok(i64 %cursor, i64 1)\n  %bad = icmp ne i32 %span, 0\n";
    text += "  br i1 %bad, label %fail, label %read\nread:\n  %wasm = call i32 @anyps5_translate(i64 %cursor)\n";
    text += "  %byte = call i32 @anyps5_read8(i32 %wasm)\n  %zero = icmp eq i32 %byte, 0\n";
    text += "  br i1 %zero, label %done, label %more\nmore:\n  %next = add i64 %cursor, 1\n";
    text += "  %wrap = icmp eq i64 %next, 0\n  br i1 %wrap, label %fail, label %loop\n";
    text += "done:\n  %len = sub i64 %cursor, %addr\n  ret i64 %len\nfail:\n  ret i64 -1\n}\n";
    return text;
}

std::string LoadGpr(int reg, const char* name) {
    return std::string("  %") + name + " = load i64, ptr " + Gpr(reg) + ", align 1\n";
}

std::string StoreRax(const std::string& value) {
    return "  store i64 " + value + ", ptr " + Gpr(0) + ", align 1\n";
}

std::string ReturnTail() {
    return "  %returned = call i32 @anyps5_guest_return()\n  ret i32 %returned\nfail:\n  ret i32 1\n}\n";
}

}

std::string EmitWasmImports(const WasmImage& image) {
    if (image.Imports.empty()) return {};
    std::string text = Helpers(image.GuestBase, image.GuestLimit);
    if (WasmImportsUse(image, WasmImportKind::Memcpy)) {
        text += "define internal i32 @anyps5_import_1() {\nentry:\n";
        text += LoadGpr(7, "dst");
        text += LoadGpr(6, "src");
        text += LoadGpr(2, "count");
        text += "  %copied = call i32 @anyps5_copy(i64 %dst, i64 %src, i64 %count)\n";
        text += "  %bad = icmp ne i32 %copied, 0\n  br i1 %bad, label %fail, label %store\nstore:\n";
        text += StoreRax("%dst");
        text += ReturnTail();
    }
    if (WasmImportsUse(image, WasmImportKind::Memset)) {
        text += "define internal i32 @anyps5_import_2() {\nentry:\n";
        text += LoadGpr(7, "dst");
        text += LoadGpr(6, "value");
        text += LoadGpr(2, "count");
        text += "  %byte64 = and i64 %value, 255\n  %byte = trunc i64 %byte64 to i32\n";
        text += "  %filled = call i32 @anyps5_fill(i64 %dst, i32 %byte, i64 %count)\n";
        text += "  %bad = icmp ne i32 %filled, 0\n  br i1 %bad, label %fail, label %store\nstore:\n";
        text += StoreRax("%dst");
        text += ReturnTail();
    }
    if (WasmImportsUse(image, WasmImportKind::Strlen)) {
        text += "define internal i32 @anyps5_import_3() {\nentry:\n";
        text += LoadGpr(7, "text");
        text += "  %len = call i64 @anyps5_length(i64 %text)\n  %bad = icmp eq i64 %len, -1\n";
        text += "  br i1 %bad, label %fail, label %store\nstore:\n";
        text += StoreRax("%len");
        text += ReturnTail();
    }
    if (WasmImportsUse(image, WasmImportKind::ProcessTime)) {
        text += "@anyps5_time_origin = internal global i64 0\n";
        text += "declare i64 @anyps5_host_monotonic_us() #0\n";
        text += "attributes #0 = { \"wasm-import-module\"=\"env\" \"wasm-import-name\"=\"anyps5_host_monotonic_us\" }\n";
        text += "define internal i64 @anyps5_process_time() {\nentry:\n";
        text += "  %now = call i64 @anyps5_host_monotonic_us()\n  %origin = load i64, ptr @anyps5_time_origin, align 8\n";
        text += "  %delta = sub i64 %now, %origin\n  ret i64 %delta\n}\n";
        text += "define internal i32 @anyps5_import_4() {\nentry:\n";
        text += "  %ticks = call i64 @anyps5_process_time()\n";
        text += StoreRax("%ticks");
        text += "  %returned = call i32 @anyps5_guest_return()\n  ret i32 %returned\n}\n";
    }
    if (WasmImportsUse(image, WasmImportKind::Graphics)) {
        const char* group = WasmImportsUse(image, WasmImportKind::ProcessTime) ? "1" : "0";
        text += "declare i32 @anyps5_webgpu_submit(i32, i32) #";
        text += group;
        text += "\nattributes #";
        text += group;
        text += " = { \"wasm-import-module\"=\"env\" \"wasm-import-name\"=\"anyps5_webgpu_submit\" }\n";
        text += "define internal i32 @anyps5_import_5() {\nentry:\n";
        text += LoadGpr(7, "command");
        text += LoadGpr(6, "bytes");
        text += "  %sized = icmp eq i64 %bytes, 20\n  br i1 %sized, label %span, label %fail\nspan:\n";
        text += "  %badspan = call i32 @anyps5_span_ok(i64 %command, i64 20)\n  %faulted = icmp ne i32 %badspan, 0\n";
        text += "  br i1 %faulted, label %fail, label %call\ncall:\n";
        text += "  %wasm = call i32 @anyps5_translate(i64 %command)\n";
        text += "  %accepted = call i32 @anyps5_webgpu_submit(i32 %wasm, i32 20)\n";
        text += "  %bad = icmp slt i32 %accepted, 0\n  br i1 %bad, label %fail, label %store\nstore:\n";
        text += "  %wide = zext i32 %accepted to i64\n";
        text += StoreRax("%wide");
        text += ReturnTail();
    }
    if (WasmImportsUse(image, WasmImportKind::Puts)) {
        int group = 0;
        if (WasmImportsUse(image, WasmImportKind::ProcessTime)) ++group;
        if (WasmImportsUse(image, WasmImportKind::Graphics)) ++group;
        text += "declare void @anyps5_host_putc(i32) #";
        text += std::to_string(group);
        text += "\nattributes #";
        text += std::to_string(group);
        text += " = { \"wasm-import-module\"=\"env\" \"wasm-import-name\"=\"anyps5_host_putc\" }\n";
        text += "define internal i32 @anyps5_import_6() {\nentry:\n";
        text += LoadGpr(7, "text");
        text += "  %len = call i64 @anyps5_length(i64 %text)\n  %bad = icmp eq i64 %len, -1\n";
        text += "  br i1 %bad, label %fail, label %loop\n";
        text += "loop:\n  %index = phi i64 [ 0, %entry ], [ %next, %write ]\n  %done = icmp uge i64 %index, %len\n";
        text += "  br i1 %done, label %nl, label %write\nwrite:\n  %addr = add i64 %text, %index\n";
        text += "  %wasm = call i32 @anyps5_translate(i64 %addr)\n  %byte = call i32 @anyps5_read8(i32 %wasm)\n";
        text += "  call void @anyps5_host_putc(i32 %byte)\n  %next = add i64 %index, 1\n  br label %loop\nnl:\n";
        text += "  call void @anyps5_host_putc(i32 10)\n  %count = add i64 %len, 1\n";
        text += StoreRax("%count");
        text += ReturnTail();
    }
    if (WasmUsesHost(image)) {
        text += WasmHostDeclare(WasmHostGroup(image));
        for (int kind = static_cast<int>(WasmImportKind::Error); kind <= static_cast<int>(WasmImportKind::VideoSubmit); ++kind) {
            if (!WasmImportsUse(image, static_cast<WasmImportKind>(kind))) continue;
            text += "define internal i32 @anyps5_import_" + std::to_string(kind) + "() {\nentry:\n";
            if (kind == static_cast<int>(WasmImportKind::Fmodf)) {
                text += "  %xmm0 = load i64, ptr " + State(kXmmOffset) + ", align 1\n";
                text += "  %xmm1 = load i64, ptr " + State(kXmmOffset + 16) + ", align 1\n";
            }
            text += LoadGpr(7, "rdi");
            text += LoadGpr(6, "rsi");
            text += LoadGpr(2, "rdx");
            text += LoadGpr(1, "rcx");
            text += LoadGpr(8, "r8");
            text += LoadGpr(9, "r9");
            text += LoadGpr(4, "rsp");
            text += LoadGpr(0, "rax");
            text += "  %status = call i32 @anyps5_host_call(i32 " + std::to_string(kind);
            if (kind == static_cast<int>(WasmImportKind::Fmodf)) {
                text += ", i64 %xmm0, i64 %xmm1, i64 0, i64 0, i64 0, i64 0, i64 0, i64 0)\n";
                text += "  %bad = icmp ne i32 %status, 0\n  br i1 %bad, label %fail, label %store\nstore:\n";
                text += "  %result = load i64, ptr " + Scratch(kHostResult) + ", align 1\n";
                text += "  store i64 %result, ptr " + State(kXmmOffset) + ", align 1\n";
                text += "  store i64 0, ptr " + State(kXmmOffset + 8) + ", align 1\n";
                text += "  %returned = call i32 @anyps5_guest_return()\n  ret i32 %returned\nfail:\n  ret i32 1\n}\n";
                continue;
            }
            text += ", i64 %rdi, i64 %rsi, i64 %rdx, i64 %rcx, i64 %r8, i64 %r9, i64 %rsp, i64 %rax)\n";
            text += "  %bad = icmp ne i32 %status, 0\n  br i1 %bad, label %fail, label %store\nstore:\n";
            text += "  %result = load i64, ptr " + Scratch(kHostResult) + ", align 1\n";
            text += StoreRax("%result");
            text += ReturnTail();
        }
    }
    return text;
}

bool WasmUsesHost(const WasmImage& image) {
    for (int kind = static_cast<int>(WasmImportKind::Error); kind <= static_cast<int>(WasmImportKind::VideoSubmit); ++kind) {
        if (WasmImportsUse(image, static_cast<WasmImportKind>(kind))) return true;
    }
    return false;
}

int WasmHostGroup(const WasmImage& image) {
    int group = 0;
    if (WasmImportsUse(image, WasmImportKind::ProcessTime)) ++group;
    if (WasmImportsUse(image, WasmImportKind::Graphics)) ++group;
    if (WasmImportsUse(image, WasmImportKind::Puts)) ++group;
    return group;
}

std::string WasmHostDeclare(int group) {
    const auto index = std::to_string(group);
    return "declare i32 @anyps5_host_call(i32, i64, i64, i64, i64, i64, i64, i64, i64) #" + index + "\nattributes #" + index + " = { \"wasm-import-module\"=\"env\" \"wasm-import-name\"=\"anyps5_host_call\" }\n";
}

}
