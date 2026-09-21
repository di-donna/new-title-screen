// Checks for the title screen's runtime splash recolour: the per-texel rule against values worked out by hand,
// the BC7 codec's round trip, and the transform's size checks. With arguments it is a tool for the local
// comparison against the game's own textures (see docs/TITLE-SCREEN.md):
//   bootflow_recolour_tests decode <blocks.bc7> <out.rgba>
//   bootflow_recolour_tests transform <darken|wordmark|bc7> <in.bin> <out.bin>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "../src/client/hooks/bootflow/bc7_codec.h"
#include "../src/client/hooks/bootflow/bootflow_texture_recolour.h"

namespace {

using namespace dawn::client::hooks::bootflow;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

std::array<std::uint8_t, 4> darken_one(std::array<std::uint8_t, 4> texel, recolour::Rule rule) {
    std::array<std::byte, 4> in{};
    std::array<std::byte, 4> out{};
    std::memcpy(in.data(), texel.data(), in.size());
    check(recolour::apply(rule, in, out), "a single texel transforms");
    std::array<std::uint8_t, 4> result{};
    std::memcpy(result.data(), out.data(), out.size());
    return result;
}

bool same(const std::array<std::uint8_t, 4>& a, const std::array<std::uint8_t, 4>& b) {
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

void test_rule() {
    // White inverts to black; the alpha is kept.
    check(same(darken_one({255, 255, 255, 255}, recolour::Rule::darken), {0, 0, 0, 255}), "white -> black");
    check(same(darken_one({255, 255, 255, 40}, recolour::Rule::darken), {0, 0, 0, 40}), "alpha kept");
    // Black inverts to the tint at the gain: (0.2475, 0.324, 0.45) * 255 + 0.5 -> (63, 83, 115).
    check(same(darken_one({0, 0, 0, 255}, recolour::Rule::darken), {63, 83, 115, 255}), "black -> the tint at the gain");
    // The wordmark's tittle (the stock sky blue) keeps its hue at 55 %: (0, 164, 228) -> (0, 90, 125).
    check(same(darken_one({0, 164, 228, 255}, recolour::Rule::darkenWordmark), {0, 90, 125, 255}), "tittle keeps its blue");
    // A wordmark letter (white) darkens like everything else.
    check(same(darken_one({255, 255, 255, 255}, recolour::Rule::darkenWordmark), {0, 0, 0, 255}), "wordmark letters darken");
    // Under the plain rule the same blue would darken.
    check(!same(darken_one({0, 164, 228, 255}, recolour::Rule::darken), {0, 90, 125, 255}), "plain rule does not keep the blue");
}

void test_codec() {
    // A flat block whose channels share a p-bit (all odd) round-trips exactly; mode 6 cannot do better than one
    // level when they do not (even colours next to an odd alpha), which is the generator's encoder's limit too.
    std::array<std::uint8_t, bc7::kBlockTexelBytes> flat{};
    for (std::size_t i = 0; i < flat.size(); i += 4) {
        flat[i] = 41;
        flat[i + 1] = 57;
        flat[i + 2] = 81;
        flat[i + 3] = 255;
    }
    std::array<std::uint8_t, bc7::kBlockBytes> block{};
    std::array<std::uint8_t, bc7::kBlockTexelBytes> back{};
    bc7::encode_block_mode6(flat.data(), block.data());
    check((block[0] & 0x7F) == 0x40, "the block is mode 6");
    bc7::decode_block(block.data(), back.data());
    check(std::memcmp(flat.data(), back.data(), flat.size()) == 0, "flat block round-trips exactly");
    for (std::size_t i = 0; i < flat.size(); i += 4) {
        flat[i] = 40;
        flat[i + 1] = 56;
        flat[i + 2] = 80;
    }
    bc7::encode_block_mode6(flat.data(), block.data());
    bc7::decode_block(block.data(), back.data());
    int flatWorst = 0;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        flatWorst = (std::max)(flatWorst, std::abs(static_cast<int>(flat[i]) - static_cast<int>(back[i])));
    }
    check(flatWorst <= 1, "flat block with a mixed p-bit round-trips within one level");
    // A gradient round-trips within the 8-bit endpoints and 4-bit indices of mode 6.
    std::array<std::uint8_t, bc7::kBlockTexelBytes> gradient{};
    for (std::size_t texel = 0; texel < 16; ++texel) {
        gradient[texel * 4] = static_cast<std::uint8_t>(10 + texel * 3);
        gradient[texel * 4 + 1] = static_cast<std::uint8_t>(20 + texel * 2);
        gradient[texel * 4 + 2] = static_cast<std::uint8_t>(30 + texel * 4);
        gradient[texel * 4 + 3] = 255;
    }
    bc7::encode_block_mode6(gradient.data(), block.data());
    bc7::decode_block(block.data(), back.data());
    int worst = 0;
    for (std::size_t i = 0; i < gradient.size(); ++i) {
        worst = (std::max)(worst, std::abs(static_cast<int>(gradient[i]) - static_cast<int>(back[i])));
    }
    check(worst <= 3, "gradient block round-trips within 3 levels");
    // A block without a mode bit decodes to zeros.
    std::array<std::uint8_t, bc7::kBlockBytes> empty{};
    bc7::decode_block(empty.data(), back.data());
    bool zero = true;
    for (const std::uint8_t value : back) {
        zero = zero && value == 0;
    }
    check(zero, "a block with no mode decodes to zeros");
}

void test_sizes() {
    std::array<std::byte, 6> odd{};
    std::array<std::byte, 6> out{};
    check(!recolour::apply(recolour::Rule::darken, odd, out), "not a whole number of texels is refused");
    check(!recolour::apply(recolour::Rule::darkenBc7, odd, out), "not a whole number of blocks is refused");
    std::array<std::byte, 8> eight{};
    check(!recolour::apply(recolour::Rule::darken, eight, out), "a mismatched output size is refused");
}

std::vector<std::byte> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

bool write_file(const char* path, const std::vector<std::byte>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

int tool(int argc, char** argv) {
    const std::string command = argv[1];
    if (command == "decode" && argc == 4) {
        const std::vector<std::byte> blocks = read_file(argv[2]);
        std::vector<std::byte> texels(blocks.size() * 4);
        for (std::size_t block = 0; block < blocks.size() / bc7::kBlockBytes; ++block) {
            bc7::decode_block(reinterpret_cast<const std::uint8_t*>(blocks.data()) + block * bc7::kBlockBytes,
                              reinterpret_cast<std::uint8_t*>(texels.data()) + block * bc7::kBlockTexelBytes);
        }
        return write_file(argv[3], texels) ? 0 : 2;
    }
    if (command == "transform" && argc == 5) {
        const std::string name = argv[2];
        recolour::Rule rule = recolour::Rule::darken;
        if (name == "wordmark") {
            rule = recolour::Rule::darkenWordmark;
        } else if (name == "bc7") {
            rule = recolour::Rule::darkenBc7;
        } else if (name != "darken") {
            std::printf("unknown rule %s\n", name.c_str());
            return 2;
        }
        const std::vector<std::byte> stock = read_file(argv[3]);
        std::vector<std::byte> output(stock.size());
        if (!recolour::apply(rule, stock, output)) {
            std::printf("transform refused: %zu bytes do not fit rule %s\n", stock.size(), name.c_str());
            return 2;
        }
        return write_file(argv[4], output) ? 0 : 2;
    }
    std::printf("usage: decode <blocks.bc7> <out.rgba> | transform <darken|wordmark|bc7> <in.bin> <out.bin>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        return tool(argc, argv);
    }
    test_rule();
    test_codec();
    test_sizes();
    std::printf("%d of %d bootflow recolour checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
