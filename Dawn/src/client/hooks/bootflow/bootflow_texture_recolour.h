#pragma once

#include <cstddef>
#include <span>

namespace dawn::client::hooks::bootflow::recolour {

/**
 * How a stock texture's own bytes are recoloured before the game uploads them. The rules are the title-screen
 * generator's splash_recolour: every texel becomes (1 - c) * tint * gain with its alpha kept, which turns the
 * white Bungie splash into the near-black navy of the inverted boot screens.
 */
enum class Rule : unsigned char {
    /** RGBA8 texels, every one darkened. */
    darken,
    /** RGBA8 texels of the BUNGiE wordmark: darkened, except the tittle, found by its blue-minus-red, which keeps its stock hue at a fraction of its brightness. */
    darkenWordmark,
    /** BC7 blocks: decoded, darkened, and encoded again in mode 6. */
    darkenBc7,
};

/** @return A short name for logs. */
[[nodiscard]] const char* rule_name(Rule rule) noexcept;

/**
 * Recolours a stock payload into a buffer of the same size.
 * @param rule Which rule applies.
 * @param stock The bytes the game decoded from its package: RGBA8 texels or BC7 blocks.
 * @param output Receives the recoloured bytes; must be the same size as the input.
 * @return False when the sizes do not fit the rule (not a whole number of texels or blocks); nothing is written then.
 */
[[nodiscard]] bool apply(Rule rule, std::span<const std::byte> stock, std::span<std::byte> output) noexcept;

} // namespace dawn::client::hooks::bootflow::recolour
