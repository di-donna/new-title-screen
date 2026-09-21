#include "bootflow_texture_override.h"
#include "bootflow_texture_recolour.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../../resources/resource.h"
#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace dawn::client::hooks::bootflow::texture_override {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Resourcerer's GPU-entry dispatcher, whose switch follows this prologue.
 * Arguments are the resource class, the TagHash, and the decoded entry pointer and byte count.
 */
constexpr std::string_view kGpuEntryDispatcherText =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
    "49 8B F9 49 8B F0 8B DA 8B E9 E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? "
    "8D 45 FF 83 F8 12";
/** Compiled form of that prologue, scanned for as a unique site in the main image. */
constexpr auto kGpuEntryDispatcher =
    signature<signature_length(kGpuEntryDispatcherText)>(kGpuEntryDispatcherText);

/** Bytes of one Tiger texture descriptor; the dispatcher reads exactly this much. */
constexpr std::size_t kTigerTextureHeaderSize = 0x28;
/** DDS magic plus its 124-byte header, which is where a legacy file's pixels start. */
constexpr std::size_t kDdsLegacyHeaderSize = 4 + 124;
/** A DX10 file adds a 20-byte extension header before its pixels. */
constexpr std::size_t kDdsDx10HeaderSize = kDdsLegacyHeaderSize + 20;
/** "DDS " in file order. */
constexpr std::uint32_t kDdsMagic = 0x20534444U;
/** "DX10" in the FourCC field, which marks the extension header. */
constexpr std::uint32_t kDx10FourCc = 0x30315844U;
/** DDS header size the format fixes at 124 bytes. */
constexpr std::uint32_t kDdsHeaderSizeValue = 124U;
/** DDS pixel-format block size the format fixes at 32 bytes. */
constexpr std::uint32_t kDdsPixelFormatSize = 32U;
/** DDPF_FOURCC: the pixel format is named by the FourCC field. */
constexpr std::uint32_t kDdsPixelFourCc = 0x4U;
/** DDPF_RGB: the pixel format is named by the channel masks. */
constexpr std::uint32_t kDdsPixelRgb = 0x40U;
/** D3D10_RESOURCE_DIMENSION_TEXTURE2D, the only shape this override accepts. */
constexpr std::uint32_t kDdsResourceTexture2d = 3U;
/** D3D10_RESOURCE_MISC_TEXTURECUBE, which this override refuses. */
constexpr std::uint32_t kDdsResourceMiscCube = 0x4U;
/** DXGI_FORMAT_R8G8B8A8_UNORM, the one legacy layout the embedded assets use (the two BC7 marbles carry a DX10
 * header, which names their format itself). */
constexpr std::uint32_t kDxgiR8G8B8A8Unorm = 28U;
/** Bits per pixel of that layout. */
constexpr std::uint32_t kRgba8BitCount = 32U;
/** Channel masks of that layout, in DDS field order. */
constexpr std::uint32_t kRgba8RedMask = 0x000000FFU;
constexpr std::uint32_t kRgba8GreenMask = 0x0000FF00U;
constexpr std::uint32_t kRgba8BlueMask = 0x00FF0000U;
constexpr std::uint32_t kRgba8AlphaMask = 0xFF000000U;
/** Largest width, height or array size a Tiger descriptor field can hold. */
constexpr std::uint32_t kTigerDimensionLimit = 0xFFFFU;
/** A Tiger texture descriptor carries this marker; anything else is not one. */
constexpr std::uint16_t kTigerTextureMarker = 0xCAFEU;
/** Resource class the dispatcher uses for GPU textures. */
constexpr std::uint32_t kGpuTextureClass = 1U;
/** The dispatcher's own bad-argument result, returned when the trampoline is gone. */
constexpr std::uint64_t kDispatchUnavailable = 7U;

/** Fields of a DDS file header, as byte offsets from the magic. */
struct DdsLayout {
    // The DDS file format fixes every offset below.
    static constexpr std::size_t magic = 0x00;
    static constexpr std::size_t headerSize = 0x04;
    static constexpr std::size_t height = 0x0C;
    static constexpr std::size_t width = 0x10;
    static constexpr std::size_t depth = 0x18;
    static constexpr std::size_t pixelFormatSize = 0x4C;
    static constexpr std::size_t pixelFlags = 0x50;
    static constexpr std::size_t fourCc = 0x54;
    static constexpr std::size_t bitCount = 0x58;
    static constexpr std::size_t redMask = 0x5C;
    static constexpr std::size_t greenMask = 0x60;
    static constexpr std::size_t blueMask = 0x64;
    static constexpr std::size_t alphaMask = 0x68;
    static constexpr std::size_t dxgiFormat = 0x80;
    static constexpr std::size_t resourceDimension = 0x84;
    static constexpr std::size_t miscFlag = 0x88;
    static constexpr std::size_t arraySize = 0x8C;
};

/** Fields of a Tiger texture descriptor, as byte offsets from its base. */
struct TigerLayout {
    // The client's descriptor layout fixes every offset below.
    static constexpr std::size_t dataSize = 0x00;
    static constexpr std::size_t format = 0x04;
    static constexpr std::size_t marker = 0x0C;
    static constexpr std::size_t width = 0x0E;
    static constexpr std::size_t height = 0x10;
    static constexpr std::size_t depth = 0x12;
    static constexpr std::size_t arraySize = 0x14;
};

/** What a row does with the stock bytes. */
enum class Kind : unsigned char {
    /** The embedded DDS resource is handed over in place of the stock pixels. */
    replace,
    /** The stock pixels are recoloured in memory and the result is handed over: nothing derived is embedded. */
    transform,
};

/** Exact stock texture-header/data pairs selected from package 0x010A (and the Bungie wordmark of 0x0109). */
struct AssetSpec final {
    std::uint32_t headerTag{};
    std::uint32_t dataTag{};
    int resourceId{};
    Kind kind{Kind::replace};
    recolour::Rule rule{recolour::Rule::darken};
    /** transform rows: the stock payload's size, which the rule expects exactly. */
    std::uint32_t expectedSize{};
};

/** Stock payload sizes of the transformed textures (RGBA8: four bytes per texel; BC7: one byte per texel). */
constexpr std::uint32_t kMarbleBytes = 1920U * 1200U;
constexpr std::uint32_t kHazeBytes = 1920U * 1080U * 4U;
constexpr std::uint32_t kSquareBytes = 912U * 912U * 4U;
/** 495 x 135 x 4 = 267,300, which the package pads to a multiple of 128 (the padding is recoloured along). */
constexpr std::uint32_t kWordmarkBytes = 267392U;
constexpr std::uint32_t kLineArtBytes = 824U * 824U * 4U;

/** One row per replaced texture; the dispatcher is intercepted on both of its tags. */
constexpr std::array kAssetSpecs{
    AssetSpec{0x80A145FEU, 0x80A145FFU, IDR_BOOTFLOW_TEXTURE_80A145FF},
    AssetSpec{0x80A14602U, 0x80A14601U, IDR_BOOTFLOW_TEXTURE_80A14601},
    AssetSpec{0x80A14608U, 0x80A14607U, IDR_BOOTFLOW_TEXTURE_80A14607},
    AssetSpec{0x80A1460DU, 0x80A1460EU, IDR_BOOTFLOW_TEXTURE_80A1460E},
    AssetSpec{0x80A1461CU, 0x80A1461DU, IDR_BOOTFLOW_TEXTURE_80A1461D},
    AssetSpec{0x80A14620U, 0x80A1461FU, IDR_BOOTFLOW_TEXTURE_80A1461F},
    AssetSpec{0x80A14622U, 0x80A14621U, IDR_BOOTFLOW_TEXTURE_80A14621},
    AssetSpec{0x80A14623U, 0x80A14624U, IDR_BOOTFLOW_TEXTURE_80A14624},
    AssetSpec{0x80A14626U, 0x80A14625U, IDR_BOOTFLOW_TEXTURE_80A14625},
    AssetSpec{0x80A14627U, 0x80A14628U, IDR_BOOTFLOW_TEXTURE_80A14628},
    AssetSpec{0x80A1462AU, 0x80A14629U, IDR_BOOTFLOW_TEXTURE_80A14629},
    AssetSpec{0x80A1462CU, 0x80A1462BU, IDR_BOOTFLOW_TEXTURE_80A1462B},
    AssetSpec{0x80A1462DU, 0x80A1462EU, IDR_BOOTFLOW_TEXTURE_80A1462E},
    AssetSpec{0x80A14630U, 0x80A1462FU, IDR_BOOTFLOW_TEXTURE_80A1462F},
    AssetSpec{0x80A14632U, 0x80A14631U, IDR_BOOTFLOW_TEXTURE_80A14631},
    AssetSpec{0x80A14634U, 0x80A14633U, IDR_BOOTFLOW_TEXTURE_80A14633},
    AssetSpec{0x80A14635U, 0x80A14636U, IDR_BOOTFLOW_TEXTURE_80A14636},
    AssetSpec{0x80A146D4U, 0x80A146D5U, 0, Kind::transform, recolour::Rule::darken, kSquareBytes},
    AssetSpec{0x80A14611U, 0x80A14610U, IDR_BOOTFLOW_TEXTURE_80A14610},
    AssetSpec{0x80A14614U, 0x80A14613U, IDR_BOOTFLOW_TEXTURE_80A14613},
    AssetSpec{0x80A14616U, 0x80A14617U, IDR_BOOTFLOW_TEXTURE_80A14617},
    AssetSpec{0x80A1461AU, 0x80A14619U, IDR_BOOTFLOW_TEXTURE_80A14619},
    AssetSpec{0x80A14605U, 0x80A14604U, IDR_BOOTFLOW_TEXTURE_80A14604},
    AssetSpec{0x80A146D2U, 0x80A146D3U, IDR_BOOTFLOW_TEXTURE_80A146D3},
    // The Bungie splash (2026-09-21): the two marbles (BC7), the cloud haze, the wordmark, and the wordmark
    // again under its startup-package (0x0109) tags; both copies are dispatched, so both are recoloured. Nothing
    // of Bungie's is embedded: the stock bytes are recoloured in memory as they go past (recolour::apply), with
    // the rule the generator applied when these were DDS files, so the screen is the same.
    AssetSpec{0x80A146CDU, 0x80A146CCU, 0, Kind::transform, recolour::Rule::darkenBc7, kMarbleBytes},
    AssetSpec{0x80A146D0U, 0x80A146CFU, 0, Kind::transform, recolour::Rule::darkenBc7, kMarbleBytes},
    AssetSpec{0x80A14638U, 0x80A14639U, 0, Kind::transform, recolour::Rule::darken, kHazeBytes},
    AssetSpec{0x80A146A8U, 0x80A146A9U, 0, Kind::transform, recolour::Rule::darkenWordmark, kWordmarkBytes},
    AssetSpec{0x80A125B6U, 0x80A125B7U, 0, Kind::transform, recolour::Rule::darkenWordmark, kWordmarkBytes},
    // The screens after the title (2026-09-21, "on character selection just the background needs to be
    // inverted"): the loading screen's marble and character select draw byte-identical copies of the marble,
    // the square and the 824 line art under their own tags, dispatched only after the title. Their payload
    // tags are not known and not needed: the dispatcher hands the pixels over under the descriptor tag
    // (a header tag of 0 is never matched). A copy finds the recoloured bytes of the first payload by hash.
    AssetSpec{0U, 0x80B46A3BU, 0, Kind::transform, recolour::Rule::darkenBc7, kMarbleBytes},
    AssetSpec{0U, 0x80B46A3EU, 0, Kind::transform, recolour::Rule::darkenBc7, kMarbleBytes},
    AssetSpec{0U, 0x80B46A41U, 0, Kind::transform, recolour::Rule::darkenBc7, kMarbleBytes},
    AssetSpec{0U, 0x80B46A44U, 0, Kind::transform, recolour::Rule::darkenBc7, kMarbleBytes},
    AssetSpec{0U, 0x80B47F54U, 0, Kind::transform, recolour::Rule::darken, kSquareBytes},
    AssetSpec{0U, 0x80B4615AU, 0, Kind::transform, recolour::Rule::darken, kSquareBytes},
    AssetSpec{0U, 0x80B47F58U, 0, Kind::transform, recolour::Rule::darken, kLineArtBytes},
    AssetSpec{0U, 0x80B4615DU, 0, Kind::transform, recolour::Rule::darken, kLineArtBytes},
};

struct DdsView final {
    const std::byte* pixels{};
    std::uint32_t pixelSize{};
    std::uint32_t format{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t depth{};
    std::uint16_t arraySize{};
};

struct Asset final {
    AssetSpec spec{};
    DdsView dds{};
    std::array<std::byte, kTigerTextureHeaderSize> header{};
    bool headerReady{};
    bool reported{};
};

using GpuEntryDispatcher = std::uint64_t(__fastcall*)(std::uint32_t resourceClass,
                                                      std::uint32_t tag,
                                                      const void* decoded,
                                                      std::uint64_t decodedSize) noexcept;

hooking::detour::Handle g_handle{};
SRWLOCK g_assetLock{SRWLOCK_INIT};
std::array<Asset, kAssetSpecs.size()> g_assets{};

template <typename Value>
[[nodiscard]] bool
load_value(const std::byte* bytes, std::size_t size, std::size_t offset, Value& output) noexcept {
    if (bytes == nullptr || offset > size || sizeof(Value) > size - offset) {
        return false;
    }
    std::memcpy(&output, bytes + offset, sizeof output);
    return true;
}

template <typename Value>
void store_value(std::span<std::byte> bytes, std::size_t offset, Value value) noexcept {
    if (offset <= bytes.size() && sizeof(Value) <= bytes.size() - offset) {
        std::memcpy(bytes.data() + offset, &value, sizeof value);
    }
}

/**
 * Names the DXGI format of a legacy DDS pixel block.
 * Only R8G8B8A8_UNORM is accepted; any other authored layout is refused rather than converted.
 * @param bytes DDS file bytes.
 * @param size Bytes available.
 * @param output Receives the DXGI format value.
 * @return False when the file is not the accepted layout.
 */
[[nodiscard]] bool
legacy_format(const std::byte* bytes, std::size_t size, std::uint32_t& output) noexcept {
    std::uint32_t flags = 0;
    std::uint32_t bits = 0;
    std::uint32_t red = 0;
    std::uint32_t green = 0;
    std::uint32_t blue = 0;
    std::uint32_t alpha = 0;
    if (!load_value(bytes, size, DdsLayout::pixelFlags, flags)
        || !load_value(bytes, size, DdsLayout::bitCount, bits)
        || !load_value(bytes, size, DdsLayout::redMask, red)
        || !load_value(bytes, size, DdsLayout::greenMask, green)
        || !load_value(bytes, size, DdsLayout::blueMask, blue)
        || !load_value(bytes, size, DdsLayout::alphaMask, alpha)) {
        return false;
    }
    if ((flags & kDdsPixelRgb) == 0 || bits != kRgba8BitCount || red != kRgba8RedMask
        || green != kRgba8GreenMask || blue != kRgba8BlueMask || alpha != kRgba8AlphaMask) {
        return false;
    }
    output = kDxgiR8G8B8A8Unorm;
    return true;
}

/**
 * Parses one embedded 2D DDS without allocating or copying its pixel payload.
 * @param bytes DDS file bytes, which outlive the view.
 * @param size Bytes available.
 * @param output Receives a view over the file's pixels; cleared on failure.
 * @return False when the file is not an accepted 2D texture.
 */
[[nodiscard]] bool parse_dds(const std::byte* bytes, std::size_t size, DdsView& output) noexcept {
    output = {};
    std::uint32_t magic = 0;
    std::uint32_t headerSize = 0;
    std::uint32_t pixelHeaderSize = 0;
    std::uint32_t pixelFlags = 0;
    std::uint32_t formatFourCc = 0;
    if (size < kDdsLegacyHeaderSize || !load_value(bytes, size, DdsLayout::magic, magic)
        || !load_value(bytes, size, DdsLayout::headerSize, headerSize)
        || !load_value(bytes, size, DdsLayout::pixelFormatSize, pixelHeaderSize)
        || !load_value(bytes, size, DdsLayout::pixelFlags, pixelFlags)
        || !load_value(bytes, size, DdsLayout::fourCc, formatFourCc) || magic != kDdsMagic
        || headerSize != kDdsHeaderSizeValue || pixelHeaderSize != kDdsPixelFormatSize) {
        return false;
    }
    const bool dx10 = (pixelFlags & kDdsPixelFourCc) != 0 && formatFourCc == kDx10FourCc;
    const std::size_t pixelOffset = dx10 ? kDdsDx10HeaderSize : kDdsLegacyHeaderSize;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t arraySize = 1;
    std::uint32_t resourceDimension = kDdsResourceTexture2d;
    std::uint32_t miscFlag = 0;
    std::uint32_t format = 0;
    if (size <= pixelOffset || !load_value(bytes, size, DdsLayout::width, width)
        || !load_value(bytes, size, DdsLayout::height, height)) {
        return false;
    }
    if (dx10) {
        if (!load_value(bytes, size, DdsLayout::dxgiFormat, format)
            || !load_value(bytes, size, DdsLayout::resourceDimension, resourceDimension)
            || !load_value(bytes, size, DdsLayout::miscFlag, miscFlag)
            || !load_value(bytes, size, DdsLayout::arraySize, arraySize)) {
            return false;
        }
    } else if (!legacy_format(bytes, size, format)) {
        return false;
    }
    const std::size_t pixelSize = size - pixelOffset;
    if (width == 0 || height == 0 || width > kTigerDimensionLimit || height > kTigerDimensionLimit
        || pixelSize == 0 || format == 0 || resourceDimension != kDdsResourceTexture2d
        || (miscFlag & kDdsResourceMiscCube) != 0 || arraySize == 0
        || arraySize > kTigerDimensionLimit) {
        return false;
    }
    output = DdsView{bytes + pixelOffset,
                     static_cast<std::uint32_t>(pixelSize),
                     format,
                     static_cast<std::uint16_t>(width),
                     static_cast<std::uint16_t>(height),
                     1,
                     static_cast<std::uint16_t>(arraySize)};
    return true;
}

/** Loads and validates every embedded DDS before the detour can expose any of them. */
[[nodiscard]] bool load_assets(HMODULE module) noexcept {
    if (module == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < kAssetSpecs.size(); ++index) {
        const AssetSpec& spec = kAssetSpecs[index];
        if (spec.kind == Kind::transform) {
            Asset asset{};
            asset.spec = spec;
            g_assets[index] = asset;
            continue;
        }
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(spec.resourceId), RT_RCDATA);
        if (resource == nullptr) {
            return false;
        }
        const HGLOBAL loaded = LoadResource(module, resource);
        const DWORD size = SizeofResource(module, resource);
        const auto* bytes = static_cast<const std::byte*>(LockResource(loaded));
        Asset asset{};
        asset.spec = spec;
        if (loaded == nullptr || bytes == nullptr || size == 0
            || !parse_dds(bytes, static_cast<std::size_t>(size), asset.dds)) {
            return false;
        }
        g_assets[index] = asset;
    }
    return true;
}

/** @return The asset owning this exact header or data TagHash. */
[[nodiscard]] Asset* find_asset(std::uint32_t tag, bool& header) noexcept {
    for (Asset& asset : g_assets) {
        if (asset.spec.headerTag != 0 && asset.spec.headerTag == tag) {
            header = true;
            return &asset;
        }
        if (asset.spec.dataTag == tag) {
            header = false;
            return &asset;
        }
    }
    return nullptr;
}

/** Builds one replacement Tiger descriptor from its stock descriptor and embedded DDS. */
[[nodiscard]] const void*
prepare_header(Asset& asset, const void* stock, std::uint64_t stockSize, bool& report) noexcept {
    report = false;
    if (stock == nullptr || stockSize < kTigerTextureHeaderSize) {
        return stock;
    }
    AcquireSRWLockExclusive(&g_assetLock);
    if (!asset.headerReady) {
        std::memcpy(asset.header.data(), stock, asset.header.size());
        std::uint16_t marker = 0;
        std::memcpy(&marker, asset.header.data() + TigerLayout::marker, sizeof marker);
        if (marker == kTigerTextureMarker) {
            const std::span header(asset.header);
            store_value(header, TigerLayout::dataSize, asset.dds.pixelSize);
            store_value(header, TigerLayout::format, asset.dds.format);
            store_value(header, TigerLayout::width, asset.dds.width);
            store_value(header, TigerLayout::height, asset.dds.height);
            store_value(header, TigerLayout::depth, asset.dds.depth);
            store_value(header, TigerLayout::arraySize, asset.dds.arraySize);
            asset.headerReady = true;
        }
    }
    if (asset.headerReady && !asset.reported) {
        asset.reported = true;
        report = true;
    }
    const void* result = asset.headerReady ? asset.header.data() : stock;
    ReleaseSRWLockExclusive(&g_assetLock);
    return result;
}

/** Reports one asset the first time its descriptor is handed to the dispatcher. */
void report_override(const Asset& asset) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow_texture stage=entry tag=0x%08X size=%u "
                                      "width=%u height=%u result=override",
                                      static_cast<unsigned>(asset.spec.dataTag),
                                      static_cast<unsigned>(asset.dds.pixelSize),
                                      static_cast<unsigned>(asset.dds.width),
                                      static_cast<unsigned>(asset.dds.height));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
}

/**
 * Research aid (client.dump_gpu_entries): every dispatch is logged (class, tag, size, first bytes) and the stock
 * bytes of every GPU texture entry are written once to <artifacts>\exports\texdump\<class>_<tag>.bin, so one
 * launch shows which textures a screen is drawn from (texdump_report.py in .dawn\title-screen reads a run). The
 * bootflow package, for one, arrives in a single batch about 20 s after launch under the descriptor tags.
 */
constexpr std::uint64_t kDumpEntryLimit = 64ULL * 1024 * 1024;
constexpr std::uint64_t kDumpTotalLimit = 1024ULL * 1024 * 1024;
constexpr unsigned kDumpLogLimit = 12000;
constexpr std::size_t kDumpHeadBytes = 40;
bool g_dumpEnabled{};
SRWLOCK g_dumpLock{SRWLOCK_INIT};
std::uint64_t g_dumpTotal{};
unsigned g_dumpLogged{};
core::path::Buffer g_dumpPath{};

void dump_entry(std::uint32_t resourceClass,
                std::uint32_t tag,
                const void* decoded,
                std::uint64_t size) noexcept {
    AcquireSRWLockExclusive(&g_dumpLock);
    if (g_dumpLogged < kDumpLogLimit) {
        ++g_dumpLogged;
        std::array<char, 2 * kDumpHeadBytes + 1> head{};
        const auto* bytes = static_cast<const unsigned char*>(decoded);
        const std::size_t count =
            bytes != nullptr
                ? static_cast<std::size_t>((std::min)(size, static_cast<std::uint64_t>(kDumpHeadBytes)))
                : 0;
        for (std::size_t i = 0; i < count; ++i) {
            (void)std::snprintf(head.data() + 2 * i, 3, "%02X", static_cast<unsigned>(bytes[i]));
        }
        core::log::writef(core::log::Channel::client,
                          core::log::Level::info,
                          "ev=texdump class=%u tag=0x%08X size=%llu head=%s",
                          static_cast<unsigned>(resourceClass),
                          static_cast<unsigned>(tag),
                          static_cast<unsigned long long>(size),
                          head.data());
    }
    if (resourceClass == kGpuTextureClass && decoded != nullptr && size != 0 && size <= kDumpEntryLimit
        && g_dumpTotal + size <= kDumpTotalLimit) {
        std::array<wchar_t, 64> relative{};
        (void)std::swprintf(relative.data(),
                            relative.size(),
                            L"exports\\texdump\\%u_%08X.bin",
                            static_cast<unsigned>(resourceClass),
                            static_cast<unsigned>(tag));
        if (core::path::artifact_file(relative.data(), g_dumpPath)) {
            const HANDLE file = CreateFileW(g_dumpPath.chars.data(),
                                            GENERIC_WRITE,
                                            0,
                                            nullptr,
                                            CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                (void)WriteFile(file, decoded, static_cast<DWORD>(size), &written, nullptr);
                (void)CloseHandle(file);
                g_dumpTotal += size;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_dumpLock);
}

/**
 * Textures recoloured from the game's own bytes (2026-09-21), so that none of Bungie's art has to be embedded:
 * the splash marbles (BC7), the cloud haze, the turning square, the BUNGiE wordmark and character select's
 * line art. Each distinct stock payload is transformed once and kept for the process lifetime, like an
 * embedded resource, and handed to the dispatcher in place of the stock bytes; the post-title copies are
 * byte-identical and find the first result by its hash.
 */
constexpr std::size_t kTransformedLimit = 8;

struct Transformed final {
    std::uint64_t hash{};
    std::size_t size{};
    std::vector<std::byte> bytes{};
};

std::array<Transformed, kTransformedLimit> g_transformed{};
std::size_t g_transformedCount{};

/** FNV-1a over the payload: the post-title copies are byte-identical to the splash textures. */
[[nodiscard]] std::uint64_t fnv1a(const std::byte* bytes, std::size_t size) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[index]));
        hash *= 1099511628211ULL;
    }
    return hash;
}

/** Under g_assetLock. @return The recoloured bytes already made for this payload, or null. */
[[nodiscard]] const std::byte* find_transformed(std::uint64_t hash, std::size_t size) noexcept {
    for (std::size_t index = 0; index < g_transformedCount; ++index) {
        const Transformed& entry = g_transformed[index];
        if (entry.hash == hash && entry.size == size) {
            return entry.bytes.data();
        }
    }
    return nullptr;
}

/**
 * @return The recoloured bytes for this stock payload, made once per distinct payload, or null when the stock
 * bytes should go through untouched (an unexpected size, or no memory for the result).
 */
[[nodiscard]] const std::byte*
transformed_payload(const Asset& asset, const void* decoded, std::uint64_t decodedSize) noexcept {
    const auto* stock = static_cast<const std::byte*>(decoded);
    const auto tag = static_cast<unsigned>(asset.spec.dataTag);
    const char* const rule = recolour::rule_name(asset.spec.rule);
    if (stock == nullptr || decodedSize != asset.spec.expectedSize) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=bootflow_texture stage=transform tag=0x%08X rule=%s size=%llu expected=%u result=skip",
                          tag,
                          rule,
                          static_cast<unsigned long long>(decodedSize),
                          static_cast<unsigned>(asset.spec.expectedSize));
        return nullptr;
    }
    const auto size = static_cast<std::size_t>(decodedSize);
    const std::uint64_t hash = fnv1a(stock, size);
    AcquireSRWLockExclusive(&g_assetLock);
    const std::byte* const found = find_transformed(hash, size);
    ReleaseSRWLockExclusive(&g_assetLock);
    if (found != nullptr) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::info,
                          "ev=bootflow_texture stage=transform tag=0x%08X rule=%s size=%zu result=reuse",
                          tag,
                          rule,
                          size);
        return found;
    }
    std::vector<std::byte> output;
    try {
        output.resize(size);
    } catch (...) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::error,
                          "ev=bootflow_texture stage=transform tag=0x%08X rule=%s size=%zu result=fail reason=memory",
                          tag,
                          rule,
                          size);
        return nullptr;
    }
    const auto started = std::chrono::steady_clock::now();
    if (!recolour::apply(asset.spec.rule, std::span<const std::byte>(stock, size), std::span<std::byte>(output))) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::error,
                          "ev=bootflow_texture stage=transform tag=0x%08X rule=%s size=%zu result=fail reason=rule",
                          tag,
                          rule,
                          size);
        return nullptr;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    AcquireSRWLockExclusive(&g_assetLock);
    const std::byte* result = find_transformed(hash, size);
    if (result == nullptr && g_transformedCount < kTransformedLimit) {
        Transformed& entry = g_transformed[g_transformedCount++];
        entry.hash = hash;
        entry.size = size;
        entry.bytes = std::move(output);
        result = entry.bytes.data();
    }
    ReleaseSRWLockExclusive(&g_assetLock);
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "ev=bootflow_texture stage=transform tag=0x%08X rule=%s size=%zu ms=%lld result=%s",
                      tag,
                      rule,
                      size,
                      static_cast<long long>(elapsed.count()),
                      result != nullptr ? "ok" : "skip");
    return result;
}

/** Replaces or recolours only selected decoded GPU texture entries, then preserves the native dispatcher. */
std::uint64_t __fastcall dispatch(std::uint32_t resourceClass,
                                  std::uint32_t tag,
                                  const void* decoded,
                                  std::uint64_t decodedSize) noexcept {
    const auto original = reinterpret_cast<GpuEntryDispatcher>(g_handle.original);
    if (original == nullptr) {
        return kDispatchUnavailable;
    }
    if (g_dumpEnabled) {
        dump_entry(resourceClass, tag, decoded, decodedSize);
    }
    bool header = false;
    Asset* const asset = resourceClass == kGpuTextureClass ? find_asset(tag, header) : nullptr;
    if (asset == nullptr) {
        return original(resourceClass, tag, decoded, decodedSize);
    }
    if (asset->spec.kind == Kind::transform) {
        if (header) {
            return original(resourceClass, tag, decoded, decodedSize);
        }
        const std::byte* const recoloured = transformed_payload(*asset, decoded, decodedSize);
        return original(resourceClass, tag, recoloured != nullptr ? recoloured : decoded, decodedSize);
    }
    if (!header) {
        return original(resourceClass, tag, asset->dds.pixels, asset->dds.pixelSize);
    }
    bool report = false;
    const void* const replacement = prepare_header(*asset, decoded, decodedSize, report);
    if (report) {
        report_override(*asset);
    }
    return original(resourceClass,
                    tag,
                    replacement,
                    replacement == decoded ? decodedSize : kTigerTextureHeaderSize);
}

void clear_assets() noexcept {
    AcquireSRWLockExclusive(&g_assetLock);
    g_assets = {};
    for (Transformed& entry : g_transformed) {
        entry = Transformed{};
    }
    g_transformedCount = 0;
    ReleaseSRWLockExclusive(&g_assetLock);
}

} // namespace

/** Loads embedded DDS files and attaches the decoded GPU-entry dispatcher. */
bool install(void* module) noexcept {
    if (g_handle.attached) {
        return true;
    }
    if (!core::settings::get().client.customBootflowTextures) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=bootflow_texture stage=setting enabled=0 result=skip");
        return true;
    }
    g_dumpEnabled = core::settings::get().client.dumpGpuEntries;
    if (g_dumpEnabled) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=bootflow_texture stage=dump enabled=1 result=ok");
    }
    if (!load_assets(static_cast<HMODULE>(module))) {
        clear_assets();
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootflow_texture stage=resources result=fail");
        return false;
    }
    std::byte* const target =
        scan_main_image_unique(kGpuEntryDispatcher, "bootflow_gpu_entry_dispatcher");
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&dispatch)};
    if (target == nullptr || !hooking::detour::install(spec, g_handle)) {
        clear_assets();
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootflow_texture stage=attach result=fail");
        return false;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow_texture stage=attach count=%zu result=ok",
                                      kAssetSpecs.size());
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
    return true;
}

/** Detaches before releasing the resource views and generated Tiger descriptors. */
bool uninstall() noexcept {
    if (g_handle.attached && !hooking::detour::uninstall(g_handle)) {
        return false;
    }
    clear_assets();
    return true;
}

/** @return True while the decoded GPU-entry dispatcher is attached. */
bool is_installed() noexcept {
    return g_handle.attached;
}

} // namespace dawn::client::hooks::bootflow::texture_override
