#include "retail_log_enqueue_observer.h"
#include "channel_name_repetition.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../diagnostics/module_range.h"
#include "../../executable/image.h"
#include "../../targets/game.h"
#include "../bootflow/internal.h"
#include "../graphics/renderer/graphics_splash_invert.h"
#include "../graphics/renderer/graphics_title_filigree.h"

namespace dawn::client::hooks::retail_log {
namespace {

using SetCategoryVerbosity = void(__fastcall*)(std::int32_t, std::uint32_t) noexcept;

/** The game copies exactly this many bytes out of the caller's text buffer. */
constexpr std::size_t kNativeTextSize = 320;
/** Site id the game uses for an unregistered line. */
constexpr std::int32_t kUnregisteredSite = -1;
/** Line storage holds the cleaned text plus its fixed key prefix. */
constexpr std::size_t kEventCapacity = kNativeTextSize + 160;
/** A late config load resets the thresholds, so set them again on this period. A count will not
 *  do: a closed category emits fewer lines, so it advances slower and stays closed. */
constexpr std::uint64_t kReassertIntervalMs = 2'000;
/** How many categories the game's own verbosity table holds. */
constexpr std::uint32_t kCategoryCount = 26;
/** 0 is the game's loosest category threshold. A higher value logs less. */
constexpr std::uint32_t kMostVerbose = 0;
/** Enough frames to cross the observer and retail-log plumbing into the activity caller. */
constexpr std::size_t kActivityStackDepth = 16;
/** One compact diagnostic line holding main-image RVAs only. */
constexpr std::size_t kStackLineCapacity = 384;
/** The nearest frames contain the activity selection and its state-machine callers. */
constexpr std::size_t kActivityDumpFrameCount = 4;
/** Refuse an implausibly large unwind entry before copying executable memory. */
constexpr std::size_t kMaximumFunctionBytes = 1U << 20;
/** Runtime-only identifiers retained even when the content cache skips descriptor extraction. */
constexpr std::array<std::uint32_t, 12> kMissionSchemaMarkers{
    0x808099BDU,
    0x808099BFU,
    0x80809917U,
    0x80809919U,
    0x808099C4U,
    0x80808652U,
    0x80808654U,
    0x80804FBCU,
    0x80809C42U,
    0x80809132U,
    0x8080911CU,
    0x80805F3FU};
/** Enough room for component, sense, and auth identifiers from the selected roster slot types. */
constexpr std::size_t kSchemaMarkerCapacity = 32;
/** One bounded read keeps the activity-start log hook from allocating a whole image copy. */
constexpr std::size_t kSchemaScanChunkBytes = 1U << 20;
/** Enough neighboring data to expose a schema-table row and its adjacent function pointers. */
constexpr std::size_t kSchemaReferenceRadius = 512;
/** A bad or overly broad marker cannot fill the analysis directory without a hard ceiling. */
constexpr std::size_t kSchemaReferenceLimit = 64;
/** Registry-declared decoded sizes of the two initial group-session messages. */
constexpr std::array<std::uint32_t, 2> kGameplayMessageSizeMarkers{0x00007980U, 0x0000AC20U};
/** A broad runtime scan stays useful without filling the log or analysis directory. */
constexpr std::size_t kGameplayMessageSizeMatchLimit = 64;
/** Only the first few candidate functions need preserving for offline disassembly. */
constexpr std::size_t kGameplayMessageSizeDumpLimit = 16;
/** Opaque activity-host descriptor retained by the server-side advertisement builder. */
constexpr std::size_t kGameplayJoinDescriptorSize = 128;
/** NetAddr begins after the descriptor's machine id. */
constexpr std::size_t kGameplayJoinNetAddrOffset = 8;
/** Native NetAddr width. */
constexpr std::size_t kGameplayJoinNetAddrSize = 86;
/** Join/security key follows the NetAddr. */
constexpr std::size_t kGameplayJoinKeyOffset = 94;
/** Join/security key width. */
constexpr std::size_t kGameplayJoinKeySize = 16;
/** Online-session identity and opaque tail close the descriptor. */
constexpr std::size_t kGameplayJoinTailOffset = 110;
constexpr std::size_t kGameplayJoinTailSize = 18;
/** One stack/pointee scan is deliberately bounded inside the synchronous retail log callback. */
constexpr std::size_t kDescriptorStackScanLimit = 1U << 20;
constexpr std::size_t kDescriptorPointeeRadius = 4096;
constexpr std::size_t kDescriptorPointeeLimit = 96;
/** Return from the retail-log call immediately before the state-2 virtual-method block. */
constexpr std::uintptr_t kGameplaySecureStateReturnRva = 0x1803184U;
/** The channel interface is retained in this state-function stack slot. */
constexpr std::uintptr_t kGameplaySecureChannelStackOffset = 0x68U;

thread_local bool g_inObserver{};
SRWLOCK g_channelNameReportLock = SRWLOCK_INIT;
ChannelNameRepetition g_channelNameReports{};
/** Tick at which the next re-assert is due. Zero makes the first call assert. */
volatile LONG64 g_nextAssertTick{};
volatile LONG g_schemaScanDone{};
volatile LONG g_missionTargetDumpDone{};
volatile LONG g_matchmakingFailureStackDone{};
volatile LONG g_matchmakingConfigurationSendStackDone{};
volatile LONG g_matchmakingConfigurationResponseStackDone{};
volatile LONG g_matchmakingConfigurationTaskStartStackDone{};
volatile LONG g_matchmakingConfigurationTaskCompleteStackDone{};
volatile LONG g_gameplayJoinTimeoutStackDone{};
volatile LONG g_managedSessionZeroIdentityStackDone{};
volatile LONG g_activityMembershipRegionStackDone{};
volatile LONG g_gameplaySecureKickoffStackDone{};
volatile LONG g_selectionLaunchStackDone{};
volatile LONG g_gameplayMessageSizeScanDone{};
SRWLOCK g_schemaMarkerLock = SRWLOCK_INIT;
SRWLOCK g_gameplayJoinDescriptorLock = SRWLOCK_INIT;
std::array<std::byte, kGameplayJoinDescriptorSize> g_gameplayJoinDescriptor{};
std::uint64_t g_gameplayJoinDescriptorHash{};
std::uint64_t g_gameplayJoinHostSession{};
std::int32_t g_gameplayJoinRegion{-1};
std::uint32_t g_gameplayJoinDescriptorGeneration{};
bool g_gameplayJoinDescriptorValid{};
std::array<std::uint32_t, kSchemaMarkerCapacity> g_schemaMarkers = [] {
    std::array<std::uint32_t, kSchemaMarkerCapacity> markers{};
    std::copy(kMissionSchemaMarkers.begin(), kMissionSchemaMarkers.end(), markers.begin());
    return markers;
}();
std::size_t g_schemaMarkerCount = kMissionSchemaMarkers.size();

/** Advances one x64 unwind context to its caller. */
[[nodiscard]] bool unwind_one(CONTEXT& context) noexcept {
    DWORD64 imageBase = 0;
    const PRUNTIME_FUNCTION function =
        RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
    if (function != nullptr) {
        void* handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                               imageBase,
                               context.Rip,
                               function,
                               &context,
                               &handlerData,
                               &establisherFrame,
                               nullptr);
        return context.Rip != 0;
    }
    // A leaf has no unwind metadata; its caller is the return address on top of the stack.
    __try {
        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
        context.Rsp += sizeof(DWORD64);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        context.Rip = 0;
    }
    return context.Rip != 0;
}

/**
 * Recovers the channel interface retained by the parent state function at the synchronous secure
 * kickoff log boundary. No native method is invoked here; the pointer only selects detour targets.
 */
[[nodiscard]] void* secure_channel_object(const diagnostics::ModuleRange& game) noexcept {
    CONTEXT context{};
    RtlCaptureContext(&context);
    const std::uintptr_t target = game.base + kGameplaySecureStateReturnRva;
    for (std::size_t frame = 0; frame < 32U && context.Rip != 0; ++frame) {
        if (context.Rip == target) {
            void* object = nullptr;
            __try {
                object = *reinterpret_cast<void* const*>(
                    context.Rsp + kGameplaySecureChannelStackOffset);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                object = nullptr;
            }
            return object;
        }
        if (!unwind_one(context)) {
            break;
        }
    }
    return nullptr;
}

/** @param protect VirtualQuery protection flags. @return True when a normal read is permitted. */
[[nodiscard]] bool readable_protection(DWORD protect) noexcept {
    if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protect & 0xFFU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

/** Saves one bounded main-image data window around a runtime schema identifier. */
void dump_schema_reference(std::uint32_t marker,
                           std::uintptr_t address,
                           const diagnostics::ModuleRange& game) noexcept {
    const std::uintptr_t begin =
        address - game.base < kSchemaReferenceRadius ? game.base
                                                     : address - kSchemaReferenceRadius;
    const std::uintptr_t wantedEnd = address + sizeof marker + kSchemaReferenceRadius;
    const std::uintptr_t end = wantedEnd < address || wantedEnd > game.end ? game.end : wantedEnd;
    if (end <= begin) {
        return;
    }
    std::vector<std::byte> bytes(end - begin);
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<const void*>(begin),
                          bytes.data(),
                          bytes.size(),
                          &copied)
            == FALSE
        || copied != bytes.size()) {
        return;
    }
    const HMODULE dawn = GetModuleHandleW(L"steam_api64.dll");
    core::path::Buffer path{};
    if (dawn == nullptr || !core::path::artifact_directory(dawn, path)
        || !core::path::append(path, L"\\analysis")) {
        return;
    }
    if (CreateDirectoryW(path.chars.data(), nullptr) == FALSE
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }
    std::array<wchar_t, 128> filename{};
    const int nameLength = std::swprintf(filename.data(),
                                         filename.size(),
                                         L"\\schema_ref.%08X.rva_%08llX.bin",
                                         marker,
                                         static_cast<unsigned long long>(address - game.base));
    if (nameLength <= 0 || !core::path::append(path, filename.data())) {
        return;
    }
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    const bool complete =
        WriteFile(file,
                  bytes.data(),
                  static_cast<DWORD>(bytes.size()),
                  &written,
                  nullptr)
            != FALSE
        && written == bytes.size() && FlushFileBuffers(file) != FALSE;
    (void)CloseHandle(file);
    std::array<char, 192> line{};
    const int length = std::snprintf(
        line.data(),
        line.size(),
        "ev=schema_ref marker=0x%08X rva=0x%llX window=0x%llX bytes=%lu result=%s",
        marker,
        static_cast<unsigned long long>(address - game.base),
        static_cast<unsigned long long>(begin - game.base),
        static_cast<unsigned long>(written),
        complete ? "ok" : "write");
    if (length > 0) {
        core::log::write(core::log::Channel::client,
                         complete ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(length)});
    }
}

/** Finds runtime references to the mission roster schemas after their client path has executed. */
void dump_mission_schema_references(const diagnostics::ModuleRange& game) noexcept {
    if (game.end <= game.base) {
        return;
    }
    std::array<std::uint32_t, kSchemaMarkerCapacity> markers{};
    std::size_t markerCount = 0;
    AcquireSRWLockShared(&g_schemaMarkerLock);
    markerCount = g_schemaMarkerCount;
    std::copy_n(g_schemaMarkers.begin(), markerCount, markers.begin());
    ReleaseSRWLockShared(&g_schemaMarkerLock);
    if (InterlockedCompareExchange(&g_schemaScanDone, 1, 0) != 0) {
        return;
    }
    std::array<std::size_t, kSchemaMarkerCapacity> counts{};
    std::array<std::uintptr_t, kSchemaMarkerCapacity> last{};
    std::vector<std::byte> bytes(kSchemaScanChunkBytes);
    std::uintptr_t cursor = game.base;
    while (cursor < game.end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof region) == 0) {
            break;
        }
        const auto regionBase = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const std::uintptr_t uncheckedEnd = regionBase + region.RegionSize;
        const std::uintptr_t regionEnd =
            uncheckedEnd < regionBase || uncheckedEnd > game.end ? game.end : uncheckedEnd;
        const std::uintptr_t scanBegin = (std::max)(cursor, regionBase);
        if (region.State == MEM_COMMIT && readable_protection(region.Protect)) {
            for (std::uintptr_t chunk = scanBegin; chunk < regionEnd;) {
                const std::size_t available = static_cast<std::size_t>(regionEnd - chunk);
                const std::size_t size = (std::min)(available, bytes.size());
                SIZE_T copied = 0;
                if (ReadProcessMemory(GetCurrentProcess(),
                                      reinterpret_cast<const void*>(chunk),
                                      bytes.data(),
                                      size,
                                      &copied)
                        != FALSE
                    && copied == size) {
                    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= size; ++offset) {
                        std::uint32_t value = 0;
                        std::memcpy(&value, bytes.data() + offset, sizeof value);
                        for (std::size_t marker = 0; marker < markerCount; ++marker) {
                            const std::uintptr_t address = chunk + offset;
                            if (value == markers[marker]
                                && counts[marker] < kSchemaReferenceLimit
                                && last[marker] != address) {
                                last[marker] = address;
                                ++counts[marker];
                                dump_schema_reference(value, address, game);
                            }
                        }
                    }
                }
                chunk += size;
            }
        }
        if (regionEnd <= cursor) {
            break;
        }
        cursor = regionEnd;
    }
    std::size_t references = 0;
    for (std::size_t marker = 0; marker < markerCount; ++marker) {
        references += counts[marker];
    }
    std::array<char, 192> line{};
    const int length = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=schema_scan markers=%zu references=%zu",
                                     markerCount,
                                     references);
    if (length > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(length)});
    }
}

/**
 * Saves one runtime-decrypted main-image function for offline disassembly.
 * @param phase Stable filename token identifying request or start.
 * @param frame Phase-local main-image frame number.
 * @param address Return address whose unwind entry identifies the containing function.
 * @param game Validated main-image range.
 */
void dump_activity_function(const wchar_t* phase,
                            std::size_t frame,
                            std::uintptr_t address,
                            const diagnostics::ModuleRange& game,
                            std::size_t fixedBytes = 0) noexcept {
    DWORD64 imageBase = 0;
    const PRUNTIME_FUNCTION function =
        RtlLookupFunctionEntry(static_cast<DWORD64>(address), &imageBase, nullptr);
    // Leaf functions do not need an x64 unwind entry. Preserve a small bounded window in that
    // case so tiny readiness predicates and tail-jump thunks are still available to inspect.
    const auto begin = fixedBytes != 0
                           ? address
                           : (function != nullptr
                                  ? static_cast<std::uintptr_t>(imageBase + function->BeginAddress)
                                  : address);
    const auto end = fixedBytes != 0
                         ? (address <= game.end - fixedBytes ? address + fixedBytes : game.end)
                         : (function != nullptr
                                ? static_cast<std::uintptr_t>(imageBase + function->EndAddress)
                                : (address <= game.end - 0x100U ? address + 0x100U : game.end));
    if (!diagnostics::contains(game, begin) || end <= begin || end > game.end
        || end - begin > kMaximumFunctionBytes) {
        return;
    }
    const HMODULE dawn = GetModuleHandleW(L"steam_api64.dll");
    core::path::Buffer path{};
    if (dawn == nullptr || !core::path::artifact_directory(dawn, path)
        || !core::path::append(path, L"\\analysis")) {
        return;
    }
    if (CreateDirectoryW(path.chars.data(), nullptr) == FALSE
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }
    std::array<wchar_t, 160> filename{};
    const int nameLength = std::swprintf(filename.data(),
                                         filename.size(),
                                         L"\\activity_code.%ls.frame_%zu.ret_%08llX.begin_%08llX.end_%08llX.bin",
                                         phase,
                                         frame,
                                         static_cast<unsigned long long>(address - game.base),
                                         static_cast<unsigned long long>(begin - game.base),
                                         static_cast<unsigned long long>(end - game.base));
    if (nameLength <= 0 || !core::path::append(path, filename.data())) {
        return;
    }
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const DWORD size = static_cast<DWORD>(end - begin);
    DWORD written = 0;
    const bool complete = WriteFile(file,
                                    reinterpret_cast<const void*>(begin),
                                    size,
                                    &written,
                                    nullptr)
                          != FALSE
                          && written == size && FlushFileBuffers(file) != FALSE;
    (void)CloseHandle(file);
    std::array<char, 224> line{};
    const int length = std::snprintf(
        line.data(),
        line.size(),
        "ev=retail_activity_code phase=%ls frame=%zu return=0x%llX begin=0x%llX end=0x%llX bytes=%lu result=%s",
        phase,
        frame,
        static_cast<unsigned long long>(address - game.base),
        static_cast<unsigned long long>(begin - game.base),
        static_cast<unsigned long long>(end - game.base),
        static_cast<unsigned long>(written),
        complete ? "ok" : "write");
    if (length > 0) {
        core::log::write(core::log::Channel::client,
                         complete ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(length)});
    }
}

/** Stable compact fingerprint used to correlate the server copy with native memory. */
[[nodiscard]] std::uint64_t hash_bytes(const std::byte* bytes, std::size_t size) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= std::to_integer<std::uint8_t>(bytes[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/** Reads one little-endian integer out of an already-bounded descriptor. */
[[nodiscard]] std::uint64_t read_memory_order(const std::byte* bytes,
                                              std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
                 << (index * 8U);
    }
    return value;
}

struct DescriptorMemoryMatch {
    std::uintptr_t address{};
    const char* relation{"none"};
};

/** Finds a byte pattern in one copied readable window. */
[[nodiscard]] std::uintptr_t find_pattern(std::uintptr_t base,
                                          const std::vector<std::byte>& bytes,
                                          const std::byte* pattern,
                                          std::size_t patternSize) noexcept {
    if (patternSize == 0 || bytes.size() < patternSize) {
        return 0;
    }
    const auto found = std::search(bytes.begin(),
                                   bytes.end(),
                                   pattern,
                                   pattern + patternSize);
    return found == bytes.end()
               ? 0
               : base + static_cast<std::uintptr_t>(found - bytes.begin());
}

/** Copies and searches one already-clamped readable memory window. */
void scan_descriptor_window(std::uintptr_t begin,
                            std::uintptr_t end,
                            const char* relation,
                            const std::byte* descriptor,
                            DescriptorMemoryMatch& full,
                            DescriptorMemoryMatch& netAddr) noexcept {
    if (end <= begin || end - begin < kGameplayJoinNetAddrSize) {
        return;
    }
    std::vector<std::byte> bytes(end - begin);
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<const void*>(begin),
                          bytes.data(),
                          bytes.size(),
                          &copied)
            == FALSE
        || copied != bytes.size()) {
        return;
    }
    if (full.address == 0) {
        full.address = find_pattern(begin, bytes, descriptor, kGameplayJoinDescriptorSize);
        if (full.address != 0) {
            full.relation = relation;
            netAddr.address = full.address + kGameplayJoinNetAddrOffset;
            netAddr.relation = relation;
            return;
        }
    }
    if (netAddr.address == 0) {
        netAddr.address = find_pattern(begin,
                                       bytes,
                                       descriptor + kGameplayJoinNetAddrOffset,
                                       kGameplayJoinNetAddrSize);
        if (netAddr.address != 0) {
            netAddr.relation = relation;
        }
    }
}

/**
 * Correlates the published descriptor with the native state synchronously visible at one retail
 * log boundary. It searches the current stack and one bounded memory window around unique
 * pointers retained on that stack; it never writes native memory.
 */
void trace_gameplay_join_descriptor(const char* phase) noexcept {
    AcquireSRWLockShared(&g_gameplayJoinDescriptorLock);
    if (!g_gameplayJoinDescriptorValid) {
        ReleaseSRWLockShared(&g_gameplayJoinDescriptorLock);
        std::array<char, 192> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=gameplay_descriptor_trace phase=%s registered=0",
                                          phase);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }

    DescriptorMemoryMatch full{};
    DescriptorMemoryMatch netAddr{};
    const auto* const descriptor = g_gameplayJoinDescriptor.data();
    const std::uintptr_t retainedAddress = reinterpret_cast<std::uintptr_t>(descriptor);
    // NT_TIB is the first member of TEB on Windows. The SDK leaves _TEB opaque in this build,
    // while NtCurrentTeb still returns its address, so use the public prefix directly.
    const NT_TIB* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    std::uintptr_t stackBegin = reinterpret_cast<std::uintptr_t>(&tib);
    const std::uintptr_t stackLimit = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
    const std::uintptr_t stackEnd = reinterpret_cast<std::uintptr_t>(tib->StackBase);
    if (stackBegin < stackLimit) {
        stackBegin = stackLimit;
    }
    if (stackEnd > stackBegin && stackEnd - stackBegin > kDescriptorStackScanLimit) {
        stackBegin = stackEnd - kDescriptorStackScanLimit;
    }

    std::vector<std::byte> stackBytes;
    if (stackEnd > stackBegin) {
        stackBytes.resize(stackEnd - stackBegin);
        SIZE_T copied = 0;
        if (ReadProcessMemory(GetCurrentProcess(),
                              reinterpret_cast<const void*>(stackBegin),
                              stackBytes.data(),
                              stackBytes.size(),
                              &copied)
                == FALSE
            || copied != stackBytes.size()) {
            stackBytes.clear();
        }
    }
    if (!stackBytes.empty()) {
        const std::uintptr_t found =
            find_pattern(stackBegin, stackBytes, descriptor, kGameplayJoinDescriptorSize);
        if (found != 0) {
            full = {found, "stack"};
            netAddr = {found + kGameplayJoinNetAddrOffset, "stack"};
        } else {
            const std::uintptr_t foundNetAddr = find_pattern(stackBegin,
                                                             stackBytes,
                                                             descriptor
                                                                 + kGameplayJoinNetAddrOffset,
                                                             kGameplayJoinNetAddrSize);
            if (foundNetAddr != 0) {
                netAddr = {foundNetAddr, "stack"};
            }
        }
    }

    std::array<std::uintptr_t, kDescriptorPointeeLimit> scanned{};
    std::size_t scannedCount = 0;
    for (std::size_t offset = 0;
         full.address == 0 && offset + sizeof(std::uintptr_t) <= stackBytes.size()
         && scannedCount < scanned.size();
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t pointer = 0;
        std::memcpy(&pointer, stackBytes.data() + offset, sizeof(pointer));
        if (pointer < 0x10000U || pointer == retainedAddress) {
            continue;
        }
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(pointer), &memory, sizeof(memory)) == 0
            || memory.State != MEM_COMMIT || !readable_protection(memory.Protect)) {
            continue;
        }
        const std::uintptr_t regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const std::uintptr_t regionEnd = regionBegin + memory.RegionSize;
        std::uintptr_t begin = pointer > kDescriptorPointeeRadius
                                   ? pointer - kDescriptorPointeeRadius
                                   : regionBegin;
        begin = std::max(begin, regionBegin);
        const std::uintptr_t wantedEnd = pointer + kDescriptorPointeeRadius;
        const std::uintptr_t end = wantedEnd < pointer ? regionEnd : std::min(wantedEnd, regionEnd);
        if (end <= begin || (begin <= retainedAddress && retainedAddress < end)
            || std::find(scanned.begin(), scanned.begin() + scannedCount, begin)
                   != scanned.begin() + scannedCount) {
            continue;
        }
        scanned[scannedCount++] = begin;
        scan_descriptor_window(begin, end, "pointee", descriptor, full, netAddr);
    }

    const std::uint32_t generation = g_gameplayJoinDescriptorGeneration;
    const std::int32_t region = g_gameplayJoinRegion;
    const std::uint64_t hostSession = g_gameplayJoinHostSession;
    const std::uint64_t hash = g_gameplayJoinDescriptorHash;
    ReleaseSRWLockShared(&g_gameplayJoinDescriptorLock);

    std::array<char, 384> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay_descriptor_trace phase=%s registered=1 generation=%u region=%d "
        "host_session=0x%016llX hash=0x%016llX full=%s full_address=0x%llX "
        "netaddr=%s netaddr_address=0x%llX pointee_windows=%zu",
        phase,
        generation,
        region,
        static_cast<unsigned long long>(hostSession),
        static_cast<unsigned long long>(hash),
        full.relation,
        static_cast<unsigned long long>(full.address),
        netAddr.relation,
        static_cast<unsigned long long>(netAddr.address),
        scannedCount);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Finds runtime-decrypted code references to the initial membership/parameter registry sizes.
 * These constants distinguish the message decoder from the transport and reassembly plumbing.
 */
void scan_gameplay_message_sizes(const diagnostics::ModuleRange& game) noexcept {
    if (InterlockedCompareExchange(&g_gameplayMessageSizeScanDone, 1, 0) != 0) {
        return;
    }
    executable::ExecutableImage image{};
    if (!executable::inspect_main_module(image)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=gameplay_message_size_scan result=image");
        return;
    }
    std::array<std::size_t, kGameplayMessageSizeMarkers.size()> matches{};
    std::array<std::size_t, kGameplayMessageSizeMarkers.size()> dumps{};
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (std::size_t sectionIndex = 0; sectionIndex < image.count; ++sectionIndex) {
        const std::span<std::byte> section = image.sections[sectionIndex];
        if (section.size() < sizeof(std::uint32_t)) {
            continue;
        }
        for (std::size_t offset = 0; offset <= section.size() - sizeof(std::uint32_t); ++offset) {
            std::uint32_t value = 0;
            std::memcpy(&value, section.data() + offset, sizeof value);
            const auto marker = std::find(
                kGameplayMessageSizeMarkers.begin(), kGameplayMessageSizeMarkers.end(), value);
            if (marker == kGameplayMessageSizeMarkers.end()) {
                continue;
            }
            const std::size_t markerIndex =
                static_cast<std::size_t>(marker - kGameplayMessageSizeMarkers.begin());
            const std::size_t match = matches[markerIndex]++;
            if (match >= kGameplayMessageSizeMatchLimit) {
                continue;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(section.data() + offset);
            std::array<char, 160> line{};
            const int length = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay_message_size_scan marker=0x%08X match=%zu rva=0x%llX section=%zu",
                value,
                match,
                static_cast<unsigned long long>(address - imageBase),
                sectionIndex);
            if (length > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(length)});
            }
            if (dumps[markerIndex] < kGameplayMessageSizeDumpLimit
                && diagnostics::contains(game, address)) {
                const wchar_t* phase = value == kGameplayMessageSizeMarkers[0]
                                           ? L"gameplay_membership_size"
                                           : L"gameplay_parameters_size";
                dump_activity_function(phase, dumps[markerIndex]++, address, game);
            }
        }
    }
    std::array<char, 192> line{};
    const int length = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay_message_size_scan result=done membership=%zu parameters=%zu dumps=%zu,%zu",
        matches[0],
        matches[1],
        dumps[0],
        dumps[1]);
    if (length > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(length)});
    }
}

/** Saves the runtime-decrypted constructor/serializer thunks and their registration data. */
void dump_mission_runtime_targets(const diagnostics::ModuleRange& game) noexcept {
    if (InterlockedCompareExchange(&g_missionTargetDumpDone, 1, 0) != 0) {
        return;
    }
    struct CodeTarget final {
        const wchar_t* name;
        std::uintptr_t rva;
        std::size_t fixedBytes = 0;
    };
    constexpr std::array<CodeTarget, 17> codeTargets{{
        {L"slot35_auth", 0x1789920U},
        {L"slot35_component", 0x1789940U},
        {L"slot18_auth", 0x1789D70U},
        {L"slot18_component", 0x1789D90U},
        {L"shared_99c4", 0x17898D0U},
        {L"prologue_gate", 0xC24490U},
        {L"prologue_gate_predicate", 0xE0DC10U},
        {L"prologue_context", 0xC24080U},
        {L"prologue_arm", 0xC24340U},
        {L"prologue_ready", 0xC23EC0U},
        {L"prologue_index", 0xC23F60U},
        {L"prologue_world", 0xC02990U},
        {L"activity_host_list_consumer", 0x16D0A40U},
        {L"activity_message_54_handler", 0x4F3C10U},
        {L"activity_message_54_apply", 0x4F7140U},
        {L"activity_host_address_formatter", 0x3EA280U},
        {L"identity_lifecycle_finalize", 0x17A1AC0U, 0x8000U},
    }};
    for (const CodeTarget& target : codeTargets) {
        const std::uintptr_t address = game.base + target.rva;
        if (address >= game.base && diagnostics::contains(game, address)) {
            dump_activity_function(target.name, 0, address, game, target.fixedBytes);
        }
    }
    // The first two pointers in each runtime registration row identify per-class data. Their
    // neighboring bytes expose defaults and callbacks that do not appear in package descriptors.
    constexpr std::array<std::pair<std::uint32_t, std::uintptr_t>, 10> dataTargets{{
        {0x808099BFU, 0x312A588U}, {0x808099BFU, 0x1FB54E0U},
        {0x808099BDU, 0x3013860U}, {0x808099BDU, 0x2A02440U},
        {0x80809919U, 0x30E6288U}, {0x80809919U, 0x1FBD880U},
        {0x80809917U, 0x3007F80U}, {0x80809917U, 0x2A2AA50U},
        {0x808099C4U, 0x3101248U}, {0x808099C4U, 0x1FB1120U},
    }};
    for (const auto& [marker, rva] : dataTargets) {
        const std::uintptr_t address = game.base + rva;
        if (address >= game.base && diagnostics::contains(game, address)) {
            dump_schema_reference(marker, address, game);
        }
    }
}

/**
 * Captures main-image return addresses for the activity-selection log sites.
 * ASLR-independent RVAs let the matching functions be inspected in the on-disk executable.
 * @param siteId Registered activity log site.
 * @param text Sanitized retail message used to identify the two stable activity events.
 */
void capture_activity_stack(std::int32_t siteId, std::string_view text) noexcept {
    const bool unsafeDiagnostics =
        core::settings::get().omegaExperiments.unsafeDiagnostics;
    const bool anySelectionLaunch =
        text.find("Launching activity-selection") != std::string_view::npos;
    const bool selectionLaunch = anySelectionLaunch
                                 && text.find("mission_reunion") != std::string_view::npos;
    const bool request = text.find("activity client requesting activity host startup")
                         != std::string_view::npos;
    const bool peerSkip = text.find("As PEER, skipping activity host startup request")
                          != std::string_view::npos;
    const bool start = text.find("Starting activity '") != std::string_view::npos;
    const bool joinResult = text.find("Received join result from AH") != std::string_view::npos;
    const bool hostChanged = text.find("activity_host_changed") != std::string_view::npos;
    const bool prologueIntroLoading =
        text.find("Entering state 'setup:prologue_intro_loading'") != std::string_view::npos;
    const bool prologueTimeout =
        text.find("Timeout entering the prologue-filler") != std::string_view::npos;
    const bool matchmakingNonceMismatch =
        text.find("Received response nonce:") != std::string_view::npos;
    const bool matchmakingAdvertisementMismatch =
        text.find("Received session advertisment success that does not match")
        != std::string_view::npos;
    const bool matchmakingConfigurationSend =
        text.find("Sending MM configuration request (pt.1)") != std::string_view::npos;
    const bool matchmakingConfigurationResponse =
        text.find("Received valid config response") != std::string_view::npos;
    // ENUM(1) is reused by several world-controller states. Correlate its two lifecycle lines
    // with the configuration request/response markers so an earlier boot task cannot consume the
    // one-shot stack captures intended for the matchmaking configuration worker.
    const bool matchmakingConfigurationTaskStart =
        text.find("Started   task 'ENUM(1)'") != std::string_view::npos
        && InterlockedCompareExchange(&g_matchmakingConfigurationSendStackDone, 0, 0) != 0
        && InterlockedCompareExchange(&g_matchmakingConfigurationResponseStackDone, 0, 0) == 0;
    const bool matchmakingConfigurationTaskComplete =
        text.find("Completed task 'ENUM(1)'") != std::string_view::npos
        && InterlockedCompareExchange(&g_matchmakingConfigurationResponseStackDone, 0, 0) != 0;
    const bool gameplayJoinTimeout =
        text.find("peer join timed out waiting for initial updates") != std::string_view::npos;
    const bool managedSessionZeroIdentity =
        text.find("online session identifier isn't valid") != std::string_view::npos
        || text.find("zero session description") != std::string_view::npos;
    const bool activityMembershipRegion =
        text.find("Processing new PAH tabulated region data") != std::string_view::npos;
    const bool gameplaySecureKickoff =
        text.find("starting to secure, nat relay delay") != std::string_view::npos;
    const bool playerBroadcastFailure =
        text.find("failed to create 'player_broadcast' entity") != std::string_view::npos;
    const bool initialSliceEntry =
        text.find("Entering state 'activity:initial_slice_set_loading'") != std::string_view::npos;
    const bool initialSliceCompleted =
        text.find("Stopping transition of type 'transitioning:initial_slice_set' due to completed")
        != std::string_view::npos;
    const bool prologueProgress =
        initialSliceEntry
        || text.find("Stopping transition of type 'transitioning:initial_slice_set'")
               != std::string_view::npos;
    if (playerBroadcastFailure) {
        // This legacy timing-correction group is mutating and remains permanently quarantined.
        return;
    }
    if (initialSliceEntry) {
        // A selected override exists in orbit too. Keep the synthetic readiness bit off until the
        // destination's real initial slice has finished loading.
        bootflow::reset_prologue_filler_ready();
    }
    if (prologueProgress) {
        // Retry around the native initial-slice execution, when package code may first decrypt.
        (void)bootflow::install_prologue_filler_ready();
    }
    if (initialSliceCompleted) {
        bootflow::arm_prologue_filler_ready();
    }
    if (!unsafeDiagnostics) {
        return;
    }
    if (!selectionLaunch && !request && !peerSkip && !start && !joinResult && !hostChanged
         && !prologueIntroLoading && !prologueTimeout && !matchmakingNonceMismatch
         && !matchmakingAdvertisementMismatch && !matchmakingConfigurationSend
         && !matchmakingConfigurationResponse && !matchmakingConfigurationTaskStart
         && !matchmakingConfigurationTaskComplete && !gameplayJoinTimeout
         && !managedSessionZeroIdentity && !activityMembershipRegion
         && !gameplaySecureKickoff) {
        return;
    }
    if (selectionLaunch
        && InterlockedCompareExchange(&g_selectionLaunchStackDone, 1, 0) != 0) {
        return;
    }
    if ((matchmakingNonceMismatch || matchmakingAdvertisementMismatch)
        && InterlockedCompareExchange(&g_matchmakingFailureStackDone, 1, 0) != 0) {
        return;
    }
    if (matchmakingConfigurationSend
        && InterlockedCompareExchange(&g_matchmakingConfigurationSendStackDone, 1, 0) != 0) {
        return;
    }
    if (matchmakingConfigurationResponse
        && InterlockedCompareExchange(&g_matchmakingConfigurationResponseStackDone, 1, 0) != 0) {
        return;
    }
    if (matchmakingConfigurationTaskStart
        && InterlockedCompareExchange(&g_matchmakingConfigurationTaskStartStackDone, 1, 0) != 0) {
        return;
    }
    if (matchmakingConfigurationTaskComplete
        && InterlockedCompareExchange(&g_matchmakingConfigurationTaskCompleteStackDone, 1, 0) != 0) {
        return;
    }
    if (gameplayJoinTimeout
        && InterlockedCompareExchange(&g_gameplayJoinTimeoutStackDone, 1, 0) != 0) {
        return;
    }
    if (managedSessionZeroIdentity
        && InterlockedCompareExchange(&g_managedSessionZeroIdentityStackDone, 1, 0) != 0) {
        return;
    }
    if (activityMembershipRegion
        && InterlockedCompareExchange(&g_activityMembershipRegionStackDone, 1, 0) != 0) {
        return;
    }
    if (gameplaySecureKickoff
        && InterlockedCompareExchange(&g_gameplaySecureKickoffStackDone, 1, 0) != 0) {
        return;
    }
    if (activityMembershipRegion) {
        trace_gameplay_join_descriptor("activity_membership_region");
    }
    if (gameplaySecureKickoff) {
        trace_gameplay_join_descriptor("gameplay_secure_kickoff");
    }
    const char* phase = selectionLaunch ? "selection_launch"
                        : request ? "request"
                        : peerSkip ? "peer_skip"
                        : start ? "start"
                        : joinResult ? "join_result"
                        : hostChanged ? "host_changed"
                        : prologueIntroLoading ? "prologue_intro_loading"
                        : matchmakingNonceMismatch ? "matchmaking_nonce_mismatch"
                         : matchmakingAdvertisementMismatch ? "matchmaking_advertisement_mismatch"
                         : matchmakingConfigurationSend ? "matchmaking_configuration_send"
                         : matchmakingConfigurationResponse ? "matchmaking_configuration_response"
                         : matchmakingConfigurationTaskStart ? "matchmaking_configuration_task_start"
                         : matchmakingConfigurationTaskComplete
                             ? "matchmaking_configuration_task_complete"
                         : gameplayJoinTimeout ? "gameplay_join_timeout"
                         : managedSessionZeroIdentity ? "managed_session_zero_identity"
                         : activityMembershipRegion ? "activity_membership_region"
                         : gameplaySecureKickoff ? "gameplay_secure_kickoff"
                                       : "prologue";
    const wchar_t* widePhase = selectionLaunch ? L"selection_launch"
                               : request ? L"request"
                               : peerSkip ? L"peer_skip"
                               : start ? L"start"
                               : joinResult ? L"join_result"
                               : hostChanged ? L"host_changed"
                               : prologueIntroLoading ? L"prologue_intro_loading"
                               : matchmakingNonceMismatch ? L"matchmaking_nonce_mismatch"
                                 : matchmakingAdvertisementMismatch
                                     ? L"matchmaking_advertisement_mismatch"
                                 : matchmakingConfigurationSend ? L"matchmaking_configuration_send"
                                 : matchmakingConfigurationResponse
                                     ? L"matchmaking_configuration_response"
                                 : matchmakingConfigurationTaskStart
                                     ? L"matchmaking_configuration_task_start"
                                 : matchmakingConfigurationTaskComplete
                                     ? L"matchmaking_configuration_task_complete"
                                 : gameplayJoinTimeout ? L"gameplay_join_timeout"
                                 : managedSessionZeroIdentity
                                     ? L"managed_session_zero_identity"
                                 : activityMembershipRegion ? L"activity_membership_region"
                                 : gameplaySecureKickoff ? L"gameplay_secure_kickoff"
                                              : L"prologue";
    diagnostics::ModuleRange game{};
    if (!diagnostics::module_range(GetModuleHandleW(nullptr), game)) {
        return;
    }
    // Keep the native secure-channel vtable untouched during connection tests. The dynamic
    // observer did not exist in the archived connected build and sits directly in the candidate
    // update path, so its behavior must be excluded before changing any more wire data.
    if (gameplayJoinTimeout) {
        scan_gameplay_message_sizes(game);
    }
    std::array<void*, kActivityStackDepth> frames{};
    const USHORT count = CaptureStackBackTrace(
        0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    std::array<char, kStackLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=retail_activity_stack phase=%s site=%d rvas=",
                                phase,
                                siteId);
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(written);
    bool emitted = false;
    std::size_t gameFrame = 0;
    const std::size_t dumpFrameCount =
        matchmakingConfigurationSend || matchmakingConfigurationResponse
                || matchmakingConfigurationTaskStart || matchmakingConfigurationTaskComplete
                || managedSessionZeroIdentity || activityMembershipRegion
                || gameplaySecureKickoff
            ? 12U
            : kActivityDumpFrameCount;
    for (USHORT index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
        if (!diagnostics::contains(game, address)) {
            continue;
        }
        if (gameFrame < dumpFrameCount) {
            dump_activity_function(widePhase, gameFrame, address, game);
        }
        ++gameFrame;
        written = std::snprintf(line.data() + length,
                                line.size() - length,
                                "%s0x%llX",
                                emitted ? "," : "",
                                static_cast<unsigned long long>(address - game.base));
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size() - length) {
            break;
        }
        length += static_cast<std::size_t>(written);
        emitted = true;
    }
    if (emitted) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), length});
    }
    if (start) {
        dump_mission_schema_references(game);
        dump_mission_runtime_targets(game);
    }
    if (prologueTimeout) {
        dump_mission_runtime_targets(game);
    }
}

/**
 * Copies the native text into fixed storage as one printable line.
 * @param text Borrowed native buffer.
 * @param output Receives the cleaned characters.
 * @return Number of characters written.
 */
[[nodiscard]] std::size_t sanitize(const char* text, std::array<char, kNativeTextSize>& output) {
    std::size_t length = 0;
    __try {
        for (; length < kNativeTextSize - 1 && text[length] != '\0'; ++length) {
            const char value = text[length];
            // One line, one event: the native text carries its own line breaks.
            output[length] = value >= ' ' && value != '\x7F' ? value : ' ';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    while (length != 0 && output[length - 1] == ' ') {
        --length;
    }
    return length;
}

/**
 * Writes one captured line.
 * @param siteId Registered site id.
 * @param text Borrowed native buffer.
 */
void capture_line(std::int32_t siteId, const char* text) noexcept {
    std::array<char, kNativeTextSize> sanitized{};
    const std::size_t textLength = sanitize(text, sanitized);
    const std::string_view nativeText{sanitized.data(), textLength};
    // The two title-screen layers follow the world-controller states by name ("bootflow:start" is
    // the title screen). They need the signal whether or not the line is mirrored, so it is taken
    // before the threshold check.
    constexpr std::string_view kEntering = "Entering state '";
    const std::size_t entering = nativeText.find(kEntering);
    if (entering != std::string_view::npos) {
        std::string_view state = nativeText.substr(entering + kEntering.size());
        state = state.substr(0, state.find('\''));
        graphics::renderer::splash_invert::note_state(state);
        graphics::renderer::title_filigree::note_state(state);
    }
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::info)) {
        return;
    }
    // Observation side effects still see every line. Only the mirrored text is summarized.
    capture_activity_stack(siteId, nativeText);
    AcquireSRWLockExclusive(&g_channelNameReportLock);
    const auto repetition = g_channelNameReports.observe(siteId, nativeText, GetTickCount64());
    ReleaseSRWLockExclusive(&g_channelNameReportLock);
    if (!repetition.emit) {
        return;
    }
    std::array<char, kEventCapacity> line{};
    const int written = repetition.suppressed != 0
                            ? std::snprintf(line.data(),
                                            line.size(),
                                            "ev=retail site=%d repetition=placeholder_name "
                                            "suppressed=%llu window_ms=%llu text=%.*s",
                                            siteId,
                                            static_cast<unsigned long long>(repetition.suppressed),
                                            static_cast<unsigned long long>(repetition.windowMs),
                                            static_cast<int>(textLength),
                                            sanitized.data())
                            : std::snprintf(line.data(),
                                      line.size(),
                                      "ev=retail site=%d text=%.*s",
                                      siteId,
                                      static_cast<int>(textLength),
                                      sanitized.data());
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
}

/**
 * Mirrors the single funnel every retail log line passes through.
 * @param siteId Registered site id.
 * @param text Native buffer holding the already-formatted line.
 */
__declspec(noinline) void __fastcall enqueue_body(std::int32_t siteId, const char* text) noexcept {
    hooking::CallGate::Scope callGate(g_callGate);
    // The verbosity setter logs through this same funnel; without this it would recurse.
    const bool outer = !g_inObserver;
    g_inObserver = true;
    const Enqueue original = hooking::await_original(g_original);
    original(siteId, text);
    if (outer && callGate.accepts_side_effects()) {
        if (siteId != kUnregisteredSite && text != nullptr) {
            capture_line(siteId, text);
        }
        assert_verbosity();
    }
    if (outer) {
        g_inObserver = false;
    }
}

} // namespace

/** Retains and summarizes the exact activity-host join descriptor the server publishes. */
void register_gameplay_join_descriptor(const std::byte* descriptor,
                                       std::size_t size,
                                       std::int32_t regionIndex,
                                       std::uint64_t hostSessionId) noexcept {
    if (descriptor == nullptr || size != kGameplayJoinDescriptorSize) {
        return;
    }
    std::array<std::byte, kGameplayJoinDescriptorSize> candidate{};
    std::memcpy(candidate.data(), descriptor, candidate.size());
    const std::uint64_t hash = hash_bytes(candidate.data(), candidate.size());

    AcquireSRWLockExclusive(&g_gameplayJoinDescriptorLock);
    const bool changed = !g_gameplayJoinDescriptorValid
                         || g_gameplayJoinDescriptorHash != hash
                         || g_gameplayJoinRegion != regionIndex
                         || g_gameplayJoinHostSession != hostSessionId;
    g_gameplayJoinDescriptor = candidate;
    g_gameplayJoinDescriptorHash = hash;
    g_gameplayJoinRegion = regionIndex;
    g_gameplayJoinHostSession = hostSessionId;
    g_gameplayJoinDescriptorValid = true;
    if (changed) {
        ++g_gameplayJoinDescriptorGeneration;
    }
    const std::uint32_t generation = g_gameplayJoinDescriptorGeneration;
    ReleaseSRWLockExclusive(&g_gameplayJoinDescriptorLock);
    if (!changed) {
        return;
    }

    const std::uint64_t machine = read_memory_order(candidate.data(), sizeof(std::uint64_t));
    const std::uint16_t localPort = static_cast<std::uint16_t>(
        read_memory_order(candidate.data() + kGameplayJoinNetAddrOffset + 4,
                          sizeof(std::uint16_t)));
    const std::uint16_t publicPort = static_cast<std::uint16_t>(
        read_memory_order(candidate.data() + kGameplayJoinNetAddrOffset + 34,
                          sizeof(std::uint16_t)));
    const std::uint64_t onlineSession =
        read_memory_order(candidate.data() + kGameplayJoinTailOffset, sizeof(std::uint64_t));
    std::size_t keyNonzero = 0;
    for (std::size_t index = 0; index < kGameplayJoinKeySize; ++index) {
        keyNonzero += candidate[kGameplayJoinKeyOffset + index] != std::byte{} ? 1U : 0U;
    }
    std::size_t tailNonzero = 0;
    for (std::size_t index = 0; index < kGameplayJoinTailSize; ++index) {
        tailNonzero += candidate[kGameplayJoinTailOffset + index] != std::byte{} ? 1U : 0U;
    }
    std::array<char, 512> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay_descriptor_source result=registered generation=%u region=%d "
        "host_session=0x%016llX hash=0x%016llX machine=0x%016llX "
        "local_ip=%u.%u.%u.%u local_port=%u public_ip=%u.%u.%u.%u public_port=%u "
        "nat=%u method=%u key_nonzero=%zu key_hash=0x%016llX "
        "lobby_identity=0x%016llX tail_nonzero=%zu tail_hash=0x%016llX",
        generation,
        regionIndex,
        static_cast<unsigned long long>(hostSessionId),
        static_cast<unsigned long long>(hash),
        static_cast<unsigned long long>(machine),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 0]),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 1]),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 2]),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 3]),
        static_cast<unsigned>(localPort),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 30]),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 31]),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 32]),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 33]),
        static_cast<unsigned>(publicPort),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset + 40]),
        std::to_integer<unsigned>(candidate[kGameplayJoinNetAddrOffset
                                             + kGameplayJoinNetAddrSize - 1]),
        keyNonzero,
        static_cast<unsigned long long>(hash_bytes(candidate.data() + kGameplayJoinKeyOffset,
                                                   kGameplayJoinKeySize)),
        static_cast<unsigned long long>(onlineSession),
        tailNonzero,
        static_cast<unsigned long long>(hash_bytes(candidate.data() + kGameplayJoinTailOffset,
                                                   kGameplayJoinTailSize)));
    if (written > 0) {
        const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                       ? static_cast<std::size_t>(written)
                                       : line.size() - 1;
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), length});
    }
}

/** Adds one descriptor-discovered identifier to the next bounded runtime reference scan. */
void register_schema_marker(std::uint32_t marker) noexcept {
    if ((marker >> 16U) != 0x8080U) {
        return;
    }
    AcquireSRWLockExclusive(&g_schemaMarkerLock);
    const auto begin = g_schemaMarkers.begin();
    const auto end = begin + g_schemaMarkerCount;
    if (std::find(begin, end, marker) == end && g_schemaMarkerCount < g_schemaMarkers.size()) {
        g_schemaMarkers[g_schemaMarkerCount++] = marker;
    }
    ReleaseSRWLockExclusive(&g_schemaMarkerLock);
}

/** @return The enqueue observer body itself, with internal linkage. */
void* enqueue_entry_point() noexcept {
    return reinterpret_cast<void*>(&enqueue_body);
}

/**
 * Opens every category in the game's own log table, once we know the block exists. Reaching the
 * enqueue funnel is the proof: without the block the native body returns early.
 */
void assert_verbosity() noexcept {
    // How much the game logs follows the client threshold, so debug is what opens its table.
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::debug)) {
        return;
    }
    const auto now = static_cast<LONG64>(GetTickCount64());
    const LONG64 due = g_nextAssertTick;
    if (now < due) {
        return;
    }
    // One claim per period, so concurrent funnel threads do not all reopen the table.
    if (InterlockedCompareExchange64(
            &g_nextAssertTick, now + static_cast<LONG64>(kReassertIntervalMs), due)
        != due) {
        return;
    }
    const auto setter = reinterpret_cast<SetCategoryVerbosity>(
        targets::game::retail_log::get().setCategoryVerbosity);
    if (setter == nullptr) {
        return;
    }
    for (std::uint32_t category = 0; category < kCategoryCount; ++category) {
        setter(static_cast<std::int32_t>(category), kMostVerbose);
    }
}

bool snapshot_gameplay_join_descriptor(
    std::array<std::byte, 128>& descriptor,
    std::int32_t& regionIndex,
    std::uint64_t& hostSessionId,
    std::uint32_t& generation) noexcept {
    AcquireSRWLockShared(&g_gameplayJoinDescriptorLock);
    const bool valid = g_gameplayJoinDescriptorValid;
    if (valid) {
        descriptor = g_gameplayJoinDescriptor;
        regionIndex = g_gameplayJoinRegion;
        hostSessionId = g_gameplayJoinHostSession;
        generation = g_gameplayJoinDescriptorGeneration;
    }
    ReleaseSRWLockShared(&g_gameplayJoinDescriptorLock);
    return valid;
}

} // namespace dawn::client::hooks::retail_log
