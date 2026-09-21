#pragma once

#include <cstddef>
#include <cstdint>

namespace dawn::client::hooks::bootflow::bc7 {

/** Bytes of one BC7 block, which holds a 4x4 texel tile. */
inline constexpr std::size_t kBlockBytes = 16;
/** Bytes of the 16 RGBA8 texels of one block, row-major. */
inline constexpr std::size_t kBlockTexelBytes = 64;

/**
 * Decodes one BC7 block of any of the eight modes to its 16 RGBA8 texels, row-major, exactly as Direct3D
 * specifies the format (a block with no mode bit decodes to zeros).
 * @param block The 16 block bytes.
 * @param rgba Receives 64 bytes.
 */
void decode_block(const std::uint8_t* block, std::uint8_t* rgba) noexcept;

/**
 * Encodes 16 RGBA8 texels as one mode-6 block (one subset, 7-bit RGBA endpoints with a p-bit each, 4-bit
 * indices) the way the title-screen generator's bc7.py does: the endpoints from the block's principal axis,
 * two least-squares refits, then the nearest quantised pair.
 * @param rgba 64 bytes, row-major.
 * @param block Receives the 16 block bytes.
 */
void encode_block_mode6(const std::uint8_t* rgba, std::uint8_t* block) noexcept;

} // namespace dawn::client::hooks::bootflow::bc7
