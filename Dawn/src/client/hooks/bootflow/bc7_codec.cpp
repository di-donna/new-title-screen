#include "bc7_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dawn::client::hooks::bootflow::bc7 {
namespace {

/** How a mode lays its block out; every field is the Direct3D BC7 specification's. */
struct Mode final {
    std::uint8_t subsets;
    std::uint8_t partitionBits;
    std::uint8_t rotationBits;
    std::uint8_t indexSelectionBits;
    std::uint8_t colourBits;
    std::uint8_t alphaBits;
    /** 0: no p-bits; 1: one per subset shared by its two endpoints; 2: one per endpoint. */
    std::uint8_t pBitsPerSubset;
    std::uint8_t indexBits;
    std::uint8_t indexBits2;
};

constexpr std::array<Mode, 8> kModes{{
    {3, 4, 0, 0, 4, 0, 2, 3, 0},
    {2, 6, 0, 0, 6, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 2, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 2, 4, 0},
    {2, 6, 0, 0, 5, 5, 2, 2, 0},
}};

constexpr std::size_t kTexels = 16;
constexpr std::size_t kChannels = 4;
constexpr std::size_t kPartitions = 64;
constexpr unsigned kMaxValue = 255U;

/** Subset of each texel for the 64 two-subset partitions. */
constexpr std::array<std::array<std::uint8_t, kTexels>, kPartitions> kPartition2{{
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1}, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1}, {0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0}, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0}, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0}, {0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0}, {0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1}, {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0}, {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0}, {0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1}, {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1},
    {0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0}, {0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0}, {0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1},
    {0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1},
    {0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1}, {0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1},
    {0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0}, {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1},
}};

/** Subset of each texel for the 64 three-subset partitions. */
constexpr std::array<std::array<std::uint8_t, kTexels>, kPartitions> kPartition3{{
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0}, {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0}, {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1}, {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2}, {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
}};

/** Anchor texel of the second subset, per two-subset partition. */
constexpr std::array<std::uint8_t, kPartitions> kAnchor2{
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};
/** Anchor texel of the second subset, per three-subset partition. */
constexpr std::array<std::uint8_t, kPartitions> kAnchor3Second{
    3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,  3,  3,  8,  15, 3,  3,
    6,  10, 5,  8,  8,  6,  8,  5,  15, 15, 8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,
    15, 15, 15, 15, 3,  15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3,
};
/** Anchor texel of the third subset, per three-subset partition. */
constexpr std::array<std::uint8_t, kPartitions> kAnchor3Third{
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

constexpr std::array<std::uint8_t, 4> kWeights2{0, 21, 43, 64};
constexpr std::array<std::uint8_t, 8> kWeights3{0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<std::uint8_t, 16> kWeights4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

/** Reads bit fields least-significant first from the block bytes in order. */
class BitReader final {
public:
    explicit BitReader(const std::uint8_t* block) noexcept : block_(block) {}

    [[nodiscard]] unsigned read(unsigned count) noexcept {
        unsigned value = 0;
        for (unsigned bit = 0; bit < count; ++bit) {
            const unsigned byte = block_[position_ >> 3U];
            value |= ((byte >> (position_ & 7U)) & 1U) << bit;
            ++position_;
        }
        return value;
    }

private:
    const std::uint8_t* block_;
    unsigned position_{};
};

/** Writes bit fields least-significant first into zeroed block bytes in order. */
class BitWriter final {
public:
    explicit BitWriter(std::uint8_t* block) noexcept : block_(block) {}

    void write(unsigned value, unsigned count) noexcept {
        for (unsigned bit = 0; bit < count; ++bit) {
            if (((value >> bit) & 1U) != 0U) {
                block_[position_ >> 3U] |= static_cast<std::uint8_t>(1U << (position_ & 7U));
            }
            ++position_;
        }
    }

private:
    std::uint8_t* block_;
    unsigned position_{};
};

[[nodiscard]] unsigned weight_of(unsigned bits, unsigned index) noexcept {
    switch (bits) {
    case 2:
        return kWeights2[index & 3U];
    case 3:
        return kWeights3[index & 7U];
    default:
        return kWeights4[index & 15U];
    }
}

[[nodiscard]] std::uint8_t interpolate(unsigned low, unsigned high, unsigned weight) noexcept {
    return static_cast<std::uint8_t>(((64U - weight) * low + weight * high + 32U) >> 6U);
}

/** @return The subset the texel belongs to under this partition. */
[[nodiscard]] unsigned subset_of(unsigned subsets, unsigned partition, unsigned texel) noexcept {
    if (subsets == 2) {
        return kPartition2[partition][texel];
    }
    if (subsets == 3) {
        return kPartition3[partition][texel];
    }
    return 0;
}

/** @return True when the texel is a subset's anchor, whose index carries one bit fewer. */
[[nodiscard]] bool is_anchor(unsigned subsets, unsigned partition, unsigned texel) noexcept {
    if (texel == 0) {
        return true;
    }
    if (subsets == 2) {
        return texel == kAnchor2[partition];
    }
    if (subsets == 3) {
        return texel == kAnchor3Second[partition] || texel == kAnchor3Third[partition];
    }
    return false;
}

/** Expands a quantised endpoint channel (with its p-bit already appended) to 8 bits by bit replication. */
[[nodiscard]] unsigned expand(unsigned value, unsigned bits) noexcept {
    if (bits >= 8) {
        return value & kMaxValue;
    }
    return ((value << (8U - bits)) | (value >> (2U * bits - 8U))) & kMaxValue;
}

// ----------------------------------------------------------------------------------------------- the encoder

constexpr float kEpsilon = 1e-6F;
constexpr float kHalf = 0.5F;
constexpr float kIndexScale = 64.0F;
constexpr unsigned kEndpointMax = 127U;
constexpr unsigned kIndexCount = 16U;
constexpr unsigned kAnchorLimit = 8U;

using Texel = std::array<float, kChannels>;
using Block = std::array<Texel, kTexels>;

/** numpy's pairwise sum of exactly 16 values: eight lanes of two, then a balanced tree. */
[[nodiscard]] float pairwise16(const std::array<float, kTexels>& values) noexcept {
    std::array<float, 8> lanes{};
    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
        lanes[lane] = values[lane] + values[lane + 8];
    }
    const float left = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
    const float right = (lanes[4] + lanes[5]) + (lanes[6] + lanes[7]);
    return left + right;
}

/** Nearest 4-bit index of every texel on the segment low..high. */
void nearest_indices(const Block& texels, const Texel& low, const Texel& high, std::array<unsigned, kTexels>& indices) noexcept {
    Texel direction{};
    float length2 = 0.0F;
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
        direction[channel] = high[channel] - low[channel];
        length2 += direction[channel] * direction[channel];
    }
    const float denominator = (std::max)(length2, kEpsilon);
    for (std::size_t texel = 0; texel < kTexels; ++texel) {
        float t = 0.0F;
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            t += (texels[texel][channel] - low[channel]) * direction[channel];
        }
        t /= denominator;
        const float target = std::clamp(t, 0.0F, 1.0F) * kIndexScale;
        unsigned best = 0;
        float bestDistance = std::fabs(target - static_cast<float>(kWeights4[0]));
        for (unsigned index = 1; index < kIndexCount; ++index) {
            const float distance = std::fabs(target - static_cast<float>(kWeights4[index]));
            if (distance < bestDistance) {
                best = index;
                bestDistance = distance;
            }
        }
        indices[texel] = best;
    }
}

/** 7-bit values and the shared p-bit whose 8-bit reconstruction is nearest per channel (ties keep p = 0). */
void quantise_endpoint(const Texel& endpoint, std::array<unsigned, kChannels>& quantised, unsigned& pBit) noexcept {
    float bestError = 0.0F;
    for (unsigned candidate = 0; candidate < 2; ++candidate) {
        std::array<unsigned, kChannels> values{};
        float error = 0.0F;
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            const float scaled = (endpoint[channel] - static_cast<float>(candidate)) / 2.0F;
            const float rounded = std::clamp(std::nearbyint(scaled), 0.0F, static_cast<float>(kEndpointMax));
            values[channel] = static_cast<unsigned>(rounded);
            const float difference = rounded * 2.0F + static_cast<float>(candidate) - endpoint[channel];
            error += difference * difference;
        }
        if (candidate == 0 || error < bestError) {
            bestError = error;
            quantised = values;
            pBit = candidate;
        }
    }
}

[[nodiscard]] Texel dequantise(const std::array<unsigned, kChannels>& quantised, unsigned pBit) noexcept {
    Texel result{};
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
        result[channel] = static_cast<float>(quantised[channel]) * 2.0F + static_cast<float>(pBit);
    }
    return result;
}

} // namespace

void decode_block(const std::uint8_t* block, std::uint8_t* rgba) noexcept {
    for (std::size_t i = 0; i < kBlockTexelBytes; ++i) {
        rgba[i] = 0;
    }
    BitReader reader(block);
    unsigned mode = 0;
    while (mode < kModes.size() && reader.read(1) == 0) {
        ++mode;
    }
    if (mode >= kModes.size()) {
        return;
    }
    const Mode& layout = kModes[mode];
    const unsigned partition = layout.partitionBits != 0 ? reader.read(layout.partitionBits) : 0;
    const unsigned rotation = layout.rotationBits != 0 ? reader.read(layout.rotationBits) : 0;
    const unsigned indexSelection = layout.indexSelectionBits != 0 ? reader.read(1) : 0;
    std::array<std::array<std::array<unsigned, kChannels>, 2>, 3> endpoints{};
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (unsigned subset = 0; subset < layout.subsets; ++subset) {
            for (unsigned end = 0; end < 2; ++end) {
                endpoints[subset][end][channel] = reader.read(layout.colourBits);
            }
        }
    }
    if (layout.alphaBits != 0) {
        for (unsigned subset = 0; subset < layout.subsets; ++subset) {
            for (unsigned end = 0; end < 2; ++end) {
                endpoints[subset][end][3] = reader.read(layout.alphaBits);
            }
        }
    }
    std::array<std::array<unsigned, 2>, 3> pBits{};
    if (layout.pBitsPerSubset == 2) {
        for (unsigned subset = 0; subset < layout.subsets; ++subset) {
            for (unsigned end = 0; end < 2; ++end) {
                pBits[subset][end] = reader.read(1);
            }
        }
    } else if (layout.pBitsPerSubset == 1) {
        for (unsigned subset = 0; subset < layout.subsets; ++subset) {
            const unsigned shared = reader.read(1);
            pBits[subset][0] = shared;
            pBits[subset][1] = shared;
        }
    }
    for (unsigned subset = 0; subset < layout.subsets; ++subset) {
        for (unsigned end = 0; end < 2; ++end) {
            for (unsigned channel = 0; channel < kChannels; ++channel) {
                unsigned bits = channel < 3 ? layout.colourBits : layout.alphaBits;
                if (bits == 0) {
                    endpoints[subset][end][channel] = kMaxValue;
                    continue;
                }
                unsigned value = endpoints[subset][end][channel];
                if (layout.pBitsPerSubset != 0) {
                    value = (value << 1U) | pBits[subset][end];
                    ++bits;
                }
                endpoints[subset][end][channel] = expand(value, bits);
            }
        }
    }
    std::array<unsigned, kTexels> indices{};
    std::array<unsigned, kTexels> indices2{};
    for (unsigned texel = 0; texel < kTexels; ++texel) {
        const unsigned bits = layout.indexBits - (is_anchor(layout.subsets, partition, texel) ? 1U : 0U);
        indices[texel] = reader.read(bits);
    }
    if (layout.indexBits2 != 0) {
        for (unsigned texel = 0; texel < kTexels; ++texel) {
            const unsigned bits = layout.indexBits2 - (texel == 0 ? 1U : 0U);
            indices2[texel] = reader.read(bits);
        }
    }
    for (unsigned texel = 0; texel < kTexels; ++texel) {
        const unsigned subset = subset_of(layout.subsets, partition, texel);
        unsigned colourWeight = weight_of(layout.indexBits, indices[texel]);
        unsigned alphaWeight = colourWeight;
        if (layout.indexBits2 != 0) {
            const unsigned secondary = weight_of(layout.indexBits2, indices2[texel]);
            if (indexSelection != 0) {
                alphaWeight = colourWeight;
                colourWeight = secondary;
            } else {
                alphaWeight = secondary;
            }
        }
        std::array<std::uint8_t, kChannels> out{};
        for (unsigned channel = 0; channel < 3; ++channel) {
            out[channel] = interpolate(endpoints[subset][0][channel], endpoints[subset][1][channel], colourWeight);
        }
        out[3] = interpolate(endpoints[subset][0][3], endpoints[subset][1][3], alphaWeight);
        if (rotation >= 1 && rotation <= 3) {
            std::swap(out[3], out[rotation - 1]);
        }
        for (unsigned channel = 0; channel < kChannels; ++channel) {
            rgba[texel * kChannels + channel] = out[channel];
        }
    }
}

void encode_block_mode6(const std::uint8_t* rgba, std::uint8_t* block) noexcept {
    Block texels{};
    for (std::size_t texel = 0; texel < kTexels; ++texel) {
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            texels[texel][channel] = static_cast<float>(rgba[texel * kChannels + channel]);
        }
    }
    // The block mean and the covariance of the texels about it.
    Texel mean{};
    for (std::size_t texel = 0; texel < kTexels; ++texel) {
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            mean[channel] += texels[texel][channel];
        }
    }
    for (float& value : mean) {
        value /= static_cast<float>(kTexels);
    }
    Block centred{};
    for (std::size_t texel = 0; texel < kTexels; ++texel) {
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            centred[texel][channel] = texels[texel][channel] - mean[channel];
        }
    }
    std::array<Texel, kChannels> covariance{};
    for (std::size_t a = 0; a < kChannels; ++a) {
        for (std::size_t b = 0; b < kChannels; ++b) {
            float sum = 0.0F;
            for (std::size_t texel = 0; texel < kTexels; ++texel) {
                sum += centred[texel][a] * centred[texel][b];
            }
            covariance[a][b] = sum;
        }
    }
    // Power iteration: the principal axis of the block.
    Texel axis{1.0F, 1.0F, 1.0F, 1.0F};
    for (int iteration = 0; iteration < 8; ++iteration) {
        Texel next{};
        for (std::size_t a = 0; a < kChannels; ++a) {
            for (std::size_t b = 0; b < kChannels; ++b) {
                next[a] += covariance[a][b] * axis[b];
            }
        }
        float length2 = 0.0F;
        for (const float value : next) {
            length2 += value * value;
        }
        const float length = std::sqrt(length2);
        for (std::size_t a = 0; a < kChannels; ++a) {
            axis[a] = length > kEpsilon ? next[a] / (std::max)(length, kEpsilon) : kHalf;
        }
    }
    std::array<float, kTexels> projection{};
    for (std::size_t texel = 0; texel < kTexels; ++texel) {
        float value = 0.0F;
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            value += centred[texel][channel] * axis[channel];
        }
        projection[texel] = value;
    }
    const float lowest = *std::min_element(projection.begin(), projection.end());
    const float highest = *std::max_element(projection.begin(), projection.end());
    Texel low{};
    Texel high{};
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
        low[channel] = std::clamp(mean[channel] + axis[channel] * lowest, 0.0F, static_cast<float>(kMaxValue));
        high[channel] = std::clamp(mean[channel] + axis[channel] * highest, 0.0F, static_cast<float>(kMaxValue));
    }
    // Two least-squares refits of the endpoints from the indices they imply.
    std::array<unsigned, kTexels> indices{};
    for (int refit = 0; refit < 2; ++refit) {
        nearest_indices(texels, low, high, indices);
        std::array<float, kTexels> weights{};
        std::array<float, kTexels> inverse{};
        std::array<float, kTexels> aa{};
        std::array<float, kTexels> bb{};
        std::array<float, kTexels> ab{};
        for (std::size_t texel = 0; texel < kTexels; ++texel) {
            weights[texel] = static_cast<float>(kWeights4[indices[texel]]) / kIndexScale;
            inverse[texel] = 1.0F - weights[texel];
            aa[texel] = inverse[texel] * inverse[texel];
            bb[texel] = weights[texel] * weights[texel];
            ab[texel] = inverse[texel] * weights[texel];
        }
        const float sumAa = pairwise16(aa);
        const float sumBb = pairwise16(bb);
        const float sumAb = pairwise16(ab);
        Texel ax{};
        Texel bx{};
        for (std::size_t texel = 0; texel < kTexels; ++texel) {
            for (std::size_t channel = 0; channel < kChannels; ++channel) {
                ax[channel] += inverse[texel] * texels[texel][channel];
                bx[channel] += weights[texel] * texels[texel][channel];
            }
        }
        const float determinant = sumAa * sumBb - sumAb * sumAb;
        if (determinant > kEpsilon) {
            for (std::size_t channel = 0; channel < kChannels; ++channel) {
                low[channel] = std::clamp((sumBb * ax[channel] - sumAb * bx[channel]) / determinant,
                                          0.0F,
                                          static_cast<float>(kMaxValue));
                high[channel] = std::clamp((sumAa * bx[channel] - sumAb * ax[channel]) / determinant,
                                           0.0F,
                                           static_cast<float>(kMaxValue));
            }
        }
    }
    std::array<unsigned, kChannels> quantisedLow{};
    std::array<unsigned, kChannels> quantisedHigh{};
    unsigned pLow = 0;
    unsigned pHigh = 0;
    quantise_endpoint(low, quantisedLow, pLow);
    quantise_endpoint(high, quantisedHigh, pHigh);
    nearest_indices(texels, dequantise(quantisedLow, pLow), dequantise(quantisedHigh, pHigh), indices);
    if (indices[0] >= kAnchorLimit) {
        // The anchor texel has three index bits, so its index must be below 8: swap the endpoints.
        std::swap(quantisedLow, quantisedHigh);
        std::swap(pLow, pHigh);
        for (unsigned& index : indices) {
            index = (kIndexCount - 1U) - index;
        }
    }
    for (std::size_t i = 0; i < kBlockBytes; ++i) {
        block[i] = 0;
    }
    BitWriter writer(block);
    writer.write(1U << 6U, 7);
    for (unsigned channel = 0; channel < kChannels; ++channel) {
        writer.write(quantisedLow[channel], 7);
        writer.write(quantisedHigh[channel], 7);
    }
    writer.write(pLow, 1);
    writer.write(pHigh, 1);
    writer.write(indices[0], 3);
    for (unsigned texel = 1; texel < kTexels; ++texel) {
        writer.write(indices[texel], 4);
    }
}

} // namespace dawn::client::hooks::bootflow::bc7
