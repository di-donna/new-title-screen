#include "bootflow_texture_recolour.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "bc7_codec.h"

namespace dawn::client::hooks::bootflow::recolour {
namespace {

// The generator's P['splash_gain'], P['splash_tint'], P['splash_tittle_chroma'] and P['splash_tittle_gain']; the
// arithmetic below follows its float32 numpy expressions operation for operation, so the bytes come out the same.
/** = kBootGain of the boot-screen inversion: the splash and the inverted loading screen match. */
constexpr float kGain = 0.45F;
/** = kBootTintRed/Green/Blue of the inversion. */
constexpr float kTintRed = 0.55F;
constexpr float kTintGreen = 0.72F;
constexpr float kTintBlue = 1.0F;
/** A wordmark texel is the tittle as its blue minus red (8-bit) rises between these. */
constexpr float kTittleChromaLow = 10.0F;
constexpr float kTittleChromaHigh = 50.0F;
/** The tittle keeps its stock sky blue at this brightness ("the bungie blue tittle is too bright" at 1.0). */
constexpr float kTittleGain = 0.55F;
constexpr float kByteScale = 255.0F;
constexpr float kRoundingOffset = 0.5F;
constexpr std::size_t kTexelBytes = 4;
constexpr std::size_t kColourChannels = 3;
constexpr unsigned kMaxThreads = 8;
constexpr std::size_t kBlocksPerThreadMinimum = 2048;

constexpr std::array<float, kColourChannels> kTint{kTintRed * kGain, kTintGreen * kGain, kTintBlue * kGain};

/** numpy's to_u8: clip to 0..1, scale, add a half and truncate. */
[[nodiscard]] std::uint8_t to_byte(float value) noexcept {
    const float clipped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(clipped * kByteScale + kRoundingOffset);
}

/** The rule on one RGBA8 texel. */
void darken_texel(const std::uint8_t* in, std::uint8_t* out, bool wordmark) noexcept {
    std::array<float, kTexelBytes> c{};
    for (std::size_t channel = 0; channel < kTexelBytes; ++channel) {
        c[channel] = static_cast<float>(in[channel]) / kByteScale;
    }
    std::array<float, kColourChannels> dark{};
    for (std::size_t channel = 0; channel < kColourChannels; ++channel) {
        dark[channel] = (1.0F - c[channel]) * kTint[channel];
    }
    if (wordmark) {
        const float chroma = static_cast<float>(in[2]) - static_cast<float>(in[0]) - kTittleChromaLow;
        const float t = std::clamp(chroma / (kTittleChromaHigh - kTittleChromaLow), 0.0F, 1.0F);
        for (std::size_t channel = 0; channel < kColourChannels; ++channel) {
            const float kept = c[channel] * kTittleGain;
            dark[channel] = dark[channel] * (1.0F - t) + kept * t;
        }
    }
    for (std::size_t channel = 0; channel < kColourChannels; ++channel) {
        out[channel] = to_byte(dark[channel]);
    }
    out[3] = to_byte(c[3]);
}

void darken_texels(const std::uint8_t* in, std::uint8_t* out, std::size_t texels, bool wordmark) noexcept {
    for (std::size_t texel = 0; texel < texels; ++texel) {
        darken_texel(in + texel * kTexelBytes, out + texel * kTexelBytes, wordmark);
    }
}

/** Decodes, darkens and re-encodes the blocks first..last. */
void darken_blocks(const std::uint8_t* in, std::uint8_t* out, std::size_t first, std::size_t last) noexcept {
    std::array<std::uint8_t, bc7::kBlockTexelBytes> texels{};
    std::array<std::uint8_t, bc7::kBlockTexelBytes> darkened{};
    for (std::size_t block = first; block < last; ++block) {
        bc7::decode_block(in + block * bc7::kBlockBytes, texels.data());
        darken_texels(texels.data(), darkened.data(), bc7::kBlockTexelBytes / kTexelBytes, false);
        bc7::encode_block_mode6(darkened.data(), out + block * bc7::kBlockBytes);
    }
}

/** The BC7 rule over all blocks, on a few threads when there are enough of them. */
void darken_bc7(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks) noexcept {
    unsigned threads = std::thread::hardware_concurrency() / 2U;
    threads = std::clamp(threads, 1U, kMaxThreads);
    while (threads > 1U && blocks / threads < kBlocksPerThreadMinimum) {
        --threads;
    }
    if (threads == 1U) {
        darken_blocks(in, out, 0, blocks);
        return;
    }
    const std::size_t share = blocks / threads;
    std::vector<std::thread> workers;
    try {
        workers.reserve(threads - 1U);
        for (unsigned worker = 1; worker < threads; ++worker) {
            const std::size_t first = share * worker;
            const std::size_t last = worker + 1U == threads ? blocks : share * (worker + 1U);
            workers.emplace_back(darken_blocks, in, out, first, last);
        }
    } catch (...) {
        // A thread that could not start: its share is done here after the others are joined.
    }
    const std::size_t started = workers.size() + 1U;
    darken_blocks(in, out, 0, share);
    for (std::thread& worker : workers) {
        worker.join();
    }
    if (started < threads) {
        darken_blocks(in, out, share * started, blocks);
    }
}

} // namespace

const char* rule_name(Rule rule) noexcept {
    switch (rule) {
    case Rule::darken:
        return "darken";
    case Rule::darkenWordmark:
        return "wordmark";
    case Rule::darkenBc7:
        return "bc7";
    default:
        return "unknown";
    }
}

bool apply(Rule rule, std::span<const std::byte> stock, std::span<std::byte> output) noexcept {
    if (stock.empty() || output.size() != stock.size()) {
        return false;
    }
    const auto* in = reinterpret_cast<const std::uint8_t*>(stock.data());
    auto* out = reinterpret_cast<std::uint8_t*>(output.data());
    switch (rule) {
    case Rule::darken:
    case Rule::darkenWordmark:
        if (stock.size() % kTexelBytes != 0) {
            return false;
        }
        darken_texels(in, out, stock.size() / kTexelBytes, rule == Rule::darkenWordmark);
        return true;
    case Rule::darkenBc7:
        if (stock.size() % bc7::kBlockBytes != 0) {
            return false;
        }
        darken_bc7(in, out, stock.size() / bc7::kBlockBytes);
        return true;
    default:
        return false;
    }
}

} // namespace dawn::client::hooks::bootflow::recolour
