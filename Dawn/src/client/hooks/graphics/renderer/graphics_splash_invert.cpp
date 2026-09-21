/**
 * Boot-screen inversion. The client boots on white screens (the Bungie splash and the loading
 * screens before the title), and this layer turns them dark with light marks until the title
 * screen arrives, and paints the game's splash-to-title dissolve Dawn blue on the way.
 *
 * Every frame the finished game frame is copied and redrawn through a small pixel shader on one
 * full-screen quad in Dear ImGui's background draw list (so everything Dawn draws stays on top).
 * Per pixel, on the stored values, bright pixels are inverted and dark pixels are blended most of
 * the way towards a deep blue wash (their own colour stays under it, so the title is never a flat
 * field), and where the line between the two lies depends on the phase:
 *   - boot (a splash is up): the line sits under the Bungie letters (luma 0.27) and the tricorn
 *     (0.22), which stay inverted like the splash around them; only pixels darker than 0.16 take
 *     the wash. On a splash nothing is that dark except the title showing through the dissolve.
 *   - closing (the dissolve has begun or the title-screen state was entered): the line moves up
 *     to 0.45..0.60, so the whole title (background 0.10, filigree up to 0.45) takes the wash and
 *     only the splash remnants and the title's white text are still inverted; the inverted part
 *     is dimmed too, so the blue blob reads as the bright thing on screen.
 * The inverted boot screens are also scaled down and tinted, so they read near-black navy with the
 * white marks (the loading pattern, the tricorn, the BUNGiE letters) a faint blue. Everything is
 * inverted alike: the sky-blue tittle of the i in BUNGiE comes out orange, which is wanted.
 * The game's dissolve is the title fading in through an organic mask over the fading splash, so
 * the blob comes out as a deep blue field spreading over the dark splash, the title's shapes
 * faintly inside it. Once the frame under the effect reads as the title screen the effect fades
 * to nothing and the layer ends. Dark frames in the boot phase (a fade or a gap) get no wash, so
 * black stays black.
 *
 * The phase line, the wash gain and the strength are decided on the CPU from a grid of texels
 * read back from the frame each frame, and handed to the shader in the quad's vertex colour.
 * Without the shader (compile failure) a fixed-function inversion runs on bright frames, as the
 * first version of this layer did, and ends at the first dark frame after the title-screen state.
 *
 * Since 2026-09-21 the Bungie splash is dark by its own textures (the bootflow texture override carries the
 * marble, the haze, the turning square and the wordmark recoloured with this layer's rule, the tittle fully
 * inverted), so the strength is also gated by the frame's mean brightness (kGateDark..kGateBright): only the
 * loading screen, which no texture override reaches, is still inverted here, and a dark frame passes through.
 *
 * Nor does the layer end at the title any more: the loading screen behind Enter (before character select) is
 * the same bright screen, so once the title has been seen the layer stays ARMED, inverting bright frames under
 * the same gate, until the client enters a state outside the boot flow and character select (the world
 * loading), or a time limit. Character select itself is dark by its own textures (the splash marble again),
 * so only its loading screen is touched and its cards and text are left alone.
 */

#include "graphics_splash_invert.h"

// The settings tree names a field "interface", which the COM headers behind d3d11.h turn into a
// macro, so the settings header is read before any of them.
#include "../../../../core/settings/settings.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <string_view>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "../../../../core/logging/log.h"

namespace dawn::client::hooks::graphics::renderer::splash_invert {
namespace {

/** Texels sampled across and down the frame: 120 px apart at 1080p, so a blob is seen at once. */
constexpr UINT kSampleColumns = 16;
constexpr UINT kSampleRows = 9;
/** Texels sampled per frame, held in one row of the staging texture. */
constexpr UINT kSampleCount = kSampleColumns * kSampleRows;
/** Fixed-function fallback: mean brightness above which a frame is a white boot screen. */
constexpr float kBrightThreshold = 0.5F;
/** A sampled texel darker than this is the title showing through the dissolve (the splash has
 * nothing under the Bungie letters at 0.27). */
constexpr float kBlobSample = 0.16F;
/** A frame at least this bright... */
constexpr float kBlobFrameMean = 0.45F;
/** ...with at least this share of blob texels is the dissolve starting (the tricorn covers ~1 %). */
constexpr float kBlobShareStart = 0.02F;
/** Below this mean a boot-phase frame is a fade or a gap: no wash, dark stays dark. */
constexpr float kWashFrameMean = 0.35F;
/** The title screen reads darker than this on average (it measures 0.07..0.13; a splash fading to
 * black crosses the window below in ~150 ms, too short for the frames required)... */
constexpr float kTitleMeanMax = 0.22F;
/** ...but brighter than this (a black gap is not the title)... */
constexpr float kTitleMeanMin = 0.04F;
/** ...with at most this share of bright texels (splash remnants are large, the title's text is not). */
constexpr float kBrightSample = 0.65F;
constexpr float kTitleBrightShare = 0.05F;
/** Frames in a row that must read as the title before the fade-out is scheduled. */
constexpr unsigned kTitleFramesNeeded = 3;
/** The phase line moves from the boot rule to the closing rule over this after the state... */
constexpr ULONGLONG kRuleInMs = 200;
/** ...and over this when the dissolve itself was what was noticed. */
constexpr ULONGLONG kRuleInFastMs = 80;
/** Shortest closing before the frame is judged. */
constexpr ULONGLONG kHoldMs = 200;
/** Wait after the title is first seen before the fade; the blob has filled the screen by then. */
constexpr ULONGLONG kFadeDelayMs = 150;
/** The effect crossfades out over this once the title screen is under it. */
constexpr ULONGLONG kFadeOutMs = 700;
/** A closing that never sees the title fades the effect after this regardless. */
constexpr ULONGLONG kClosingLimitMs = 30'000;
/** Diagnostic: readings are traced for this long after the closing starts. */
constexpr ULONGLONG kTraceMs = 3'000;
/** Diagnostic: the first frames are traced for this long. */
constexpr ULONGLONG kStartTraceMs = 500;
/** Without a brightness reading the closing simply holds this long before fading. */
constexpr ULONGLONG kBlindHoldMs = 1'500;
/** The boot screens are long over by this; past it the layer stands down for good (boot and closing only). */
constexpr ULONGLONG kBootLimitMs = 180'000;
/** Armed for the menus at most this long after the title; the world's own bright frames are never touched. */
constexpr ULONGLONG kMenusLimitMs = 30 * 60'000;
/** Buffer 0 is the back buffer the game just finished drawing. */
constexpr UINT kBackBufferIndex = 0;
/** One mip, one slice, one sample: plain 2D copy targets. */
constexpr UINT kSingleLevel = 1;
constexpr UINT kSingleSample = 1;
/** Largest value of an 8-, 10- and 16-bit channel. */
constexpr float kEightBitMax = 255.0F;
constexpr float kTenBitMax = 1023.0F;
constexpr float kSixteenBitMax = 65535.0F;
/** Every channel of the render target takes part in the blend. */
constexpr UINT kAllSamples = 0xFFFFFFFFU;
/** Fields of a 10-10-10-2 texel. */
constexpr std::uint32_t kTenBitMask = 0x3FFU;
constexpr unsigned kTenBitGreenShift = 10;
constexpr unsigned kTenBitBlueShift = 20;
/** Two-bit alpha of that layout. */
constexpr unsigned kTwoBitAlphaShift = 30;
constexpr float kTwoBitMax = 3.0F;
/** Channel count in the mean. */
constexpr float kChannelCount = 3.0F;
/** Full scale of one 8-bit vertex colour channel. */
constexpr float kColourScale = 255.0F;
/** Texture slot the shader reads the frame copy from; slot 0 is Dear ImGui's own. */
constexpr UINT kFrameSlot = 1;
/** Shader-source buffer; the template below fits with room to spare. */
constexpr std::size_t kShaderSourceCapacity = 3072;
/** Longest compiler message that is copied into the log. */
constexpr int kCompilerMessageLimit = 200;

// The shader's shape, in luma of the frame under it (0..1, on the stored values).
/** Boot rule: the wash ends and the inversion begins at this luma... */
constexpr float kBootLineStart = 0.16F;
/** ...and the inversion is full from this one (the tricorn at 0.22 and the letters at 0.27 are in). */
constexpr float kBootLineFull = 0.24F;
/** Closing rule: the same line, moved above the whole title (filigree up to 0.45). */
constexpr float kClosingLineStart = 0.45F;
constexpr float kClosingLineFull = 0.60F;
/** Closing rule: the inverted part (splash remnants) is dimmed to this, so the blob is the bright thing. */
constexpr float kClosingInvertedGain = 0.6F;
/** The inverted boot screens are scaled by this... */
constexpr float kBootGain = 0.45F;
/** ...and tinted by this (per channel), so the inverted grey reads near-black navy and the white marks
 * read a faint blue ("the opening screen that is now inverted should be a lot darker with the white
 * logo/lines being faintly blue", 2026-09-20). Before: the loading screen inverted to luma 0.22 in the
 * middle with its lines and tricorn at 0.71 grey; now about 0.06 and 0.20 blue-grey. The closing dim
 * still applies on top. */
constexpr float kBootTintRed = 0.55F;
constexpr float kBootTintGreen = 0.72F;
constexpr float kBootTintBlue = 1.0F;
/** The Bungie splash is dark by its own textures since 2026-09-21 (the bootflow texture override carries a
 * pre-darkened marble, wordmark and turning square: "recolor the bungie logo before it gets loaded ... so we
 * don't have to do a hacky inversion fix"), so this layer only acts on frames that read bright, which is the
 * loading screen before it (drawn from nothing the texture override can reach). The strength follows the
 * frame's mean between these two values: the loading screen reads ~0.8, the dark splash and the title ~0.1,
 * so a dark frame passes through untouched and the wash never paints it. */
constexpr float kGateDark = 0.25F;
constexpr float kGateBright = 0.45F;
/** The Dawn logo blue (#0859F2) at this gain is what the wash is made of. 0.6 filled the screen with
 * a bright saturated blue the moment the dissolve completed, which read as a flash. */
constexpr float kWashGain = 0.4F;
/** Share of a dark pixel that the wash takes; the rest is the frame's own colour, so the title's
 * shapes stay visible inside the blue instead of the blue being a flat field. */
constexpr float kWashBlend = 0.7F;
constexpr float kDawnRed = 8.0F / kColourScale;
constexpr float kDawnGreen = 89.0F / kColourScale;
constexpr float kDawnBlue = 242.0F / kColourScale;
/** Once the layer has had its way a frame never reads brighter than this on average: the inverted loading screen
 * reads ~0.095, but a crossfade between it (or the flat grey that follows it) and the dark marble is a mid-grey
 * frame that neither the inversion nor the gate can make dark on its own, and it showed ("there is a slight white
 * transition for a bit after launching past the title screen", 2026-09-21). The output is dimmed so that a uniform
 * frame of the measured mean would read this; a frame already darker is never dimmed. The dim rides in the quad's
 * vertex alpha. */
constexpr float kTargetLuma = 0.10F;
/** The dim only touches frames whose mean is above these (a ramp): the title reads 0.10 (its blue channel is
 * high) and character select 0.09, and neither must be touched, or the filigree takes the draw for the end of
 * the title. */
constexpr float kDimFloorLow = 0.12F;
constexpr float kDimFloorHigh = 0.18F;
/** A frame drawn at this strength or more is a bright screen being inverted (covering()). */
constexpr float kCoveringStrength = 0.5F;
/** The loading emblem of the marble waiting screen after the loading screen behind Enter ("can the second part
 * have the same slightly blue loading icon", 2026-09-21): the game draws the boot loading screen's figure (the
 * lattice and the morphing shape) small at the bottom right in neutral grey, up to (56, 56, 56), which all but
 * vanishes on the dark marble. The recoloured marble lies on the boot tint's hue line (red = 0.55 blue), the
 * emblem does not, so inside this window, on frames as dark as that screen, pixels whose red exceeds 0.55 of
 * their blue are shifted onto the tint's hue at their own brightness, which is what the boot screen's marks read. */
constexpr float kEmblemLeft = 0.82F;
constexpr float kEmblemTop = 0.70F;
constexpr float kEmblemRight = 0.99F;
constexpr float kEmblemBottom = 0.98F;
/** The rule is on while the sampled grid holds no saturated texel (character select's class cards and the title's
 * icon are saturated, the marble waiting screen and its grey emblem are not: 0 there, 0.014 and up on the title,
 * 0.04 on character select, at 16:9 and 21:9 alike), fading out between these shares... */
constexpr float kEmblemSaturatedStart = 0.005F;
constexpr float kEmblemSaturatedFull = 0.02F;
/** ...and only on a dark frame: off above this channel mean (the loading screen reads 0.7, the waiting screen
 * 0.07 at 16:9 and 0.09 at 21:9; a mean window tight round the waiting screen, 0.078..0.086, was aspect-bound and
 * left the emblem grey at 21:9)... */
constexpr float kEmblemMeanOn = 0.12F;
constexpr float kEmblemMeanOff = 0.16F;
/** ...a texel counts as saturated when its channels differ by more than this (stored values). */
constexpr float kSaturatedTexel = 0.25F;
/** ...red minus 0.55 blue, stored values, between which a pixel becomes emblem (the marble scatters within +-0.01)... */
constexpr float kEmblemChromaLow = 4.0F / kColourScale;
constexpr float kEmblemChromaHigh = 12.0F / kColourScale;
/** ...and the luma above which a pixel is not emblem (light text, the cursor). */
constexpr float kEmblemLumaCap = 0.30F;
/** The emblem's brightness gain on top of the hue shift. */
constexpr float kEmblemGain = 1.1F;
/** Pixel-shader constant-buffer slot for the emblem rule; Dear ImGui's pixel shader binds none. */
constexpr UINT kParamsSlot = 0;
/** Luma of the boot tint: what a white texel inverts to, before the gain. */
constexpr float kTintLuma = 0.299F * kBootTintRed + 0.587F * kBootTintGreen + 0.114F * kBootTintBlue;

/**
 * The shader. It shares Dear ImGui's vertex layout, reads the frame copy at its own pixel, and
 * takes strength, phase line and wash gain from the quad's vertex colour (red, green, blue) and the
 * dim from its alpha.
 */
constexpr const char* kShaderTemplate = R"(
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
Texture2D frame : register(t%u);
cbuffer Emblem : register(b0)
{
    float4 emblem;      // strength (0..1), tint red / blue ratio, unused, unused
    float4 emblemRamp;  // red-minus-ratio-blue low, high (stored values), luma cap, gain
    float4 emblemBox;   // left, top, right, bottom (pixels)
    float4 emblemTint;  // the boot tint at unit luma, unused
};
float4 main(PS_INPUT input) : SV_Target
{
    float3 d = frame.Load(int3(input.pos.xy, 0)).rgb;
    float luma = dot(d, float3(0.299, 0.587, 0.114));
    float strength = input.col.r;
    float phase = input.col.g;
    float gain = input.col.b;
    float lineStart = lerp(%.3f, %.3f, phase);
    float lineFull = lerp(%.3f, %.3f, phase);
    float invert = smoothstep(lineStart, lineFull, luma);
    float3 wash = float3(%.4f, %.4f, %.4f) * (1.0 - luma);
    float3 darkPart = lerp(d, wash, gain);
    float3 inverted = (1.0 - d) * float3(%.4f, %.4f, %.4f) * lerp(1.0, %.3f, phase);
    float3 shaped = lerp(darkPart, inverted, invert);
    float3 outColour = lerp(d, shaped, strength) * input.col.a;
    if (emblem.x > 0.0 && input.pos.x > emblemBox.x && input.pos.x < emblemBox.z
        && input.pos.y > emblemBox.y && input.pos.y < emblemBox.w)
    {
        // The loading emblem: neutral dark pixels off the marble's hue line take the boot tint's hue.
        float neutral = smoothstep(emblemRamp.x, emblemRamp.y, d.r - emblem.y * d.b);
        float dark = 1.0 - smoothstep(emblemRamp.z, emblemRamp.z + 0.1, luma);
        float3 tinted = luma * emblemTint.rgb * emblemRamp.w;
        outColour = lerp(outColour, tinted, neutral * dark * emblem.x);
    }
    return float4(outColour, 1.0);
}
)";

/** Where the layer is in the boot. */
enum class Phase : int {
    /** Before the dissolve: the boot screens are inverted. */
    boot,
    /** The dissolve or the title-screen state has been seen: the title is looked for, then the effect fades. */
    closing,
    /** The title has been seen: armed for the bright screens behind Enter (the loading screen before character
     * select), dark frames untouched, until the client leaves the menus. */
    menus,
    /** Over for this process. */
    done,
};

/** How the sampled texels are decoded. */
enum class Layout {
    unsupported,
    rgba8,
    bgra8,
    rgb10a2,
    rgba16Unorm,
    rgba16Float,
    rgba32Float,
};

/** What the sampled texels said about one frame. */
struct Reading {
    float mean{};
    float darkest{};
    /** Share of texels darker than kBlobSample. */
    float blobShare{};
    /** Share of texels brighter than kBrightSample. */
    float brightShare{};
    /** Share of texels whose channels differ by more than kSaturatedTexel (coloured UI, the icon). */
    float saturatedShare{};
    /** Diagnostic: the alpha channel of the sampled texels. */
    float alphaMin{1.0F};
    float alphaMax{};
    float alphaMean{};
};

/** The numbers the shader takes for one frame in the quad's vertex colour. */
struct Controls {
    float strength{1.0F};
    float phaseLine{};
    float washGain{};
    /** Multiplies the output: 1 leaves it, less darkens a crossfade frame (kTargetLuma). */
    float dim{1.0F};
    /** The emblem rule's strength (0..1), in the constant buffer. */
    float emblem{};
};

/** The emblem rule's constant buffer. Sixteen-byte rows, as a constant buffer wants. */
struct alignas(16) EmblemParams {
    float strength, ratio, padding0, padding1;
    float chromaLow, chromaHigh, lumaCap, gain;
    float left, top, right, bottom;
    float tintRed, tintGreen, tintBlue, padding2;
};
static_assert(sizeof(EmblemParams) % 16 == 0, "constant buffers are sized in 16-byte rows");

std::atomic<int> g_phase{static_cast<int>(Phase::boot)};
/** Blend state that writes 1 - dest for a white source (the fallback). */
ID3D11BlendState* g_invertBlend{};
/** Blend state that writes the source as it is (the shader pass). */
ID3D11BlendState* g_opaqueBlend{};
/** One row of sample texels in the back buffer's own format. */
ID3D11Texture2D* g_staging{};
DXGI_FORMAT g_stagingFormat{DXGI_FORMAT_UNKNOWN};
/** The shader, compiled once per device. */
ID3D11PixelShader* g_shader{};
bool g_shaderTried{};
/** The emblem rule's parameters, updated every drawn frame. */
ID3D11Buffer* g_params{};
/** A copy of the back buffer the shader reads, remade when the back buffer changes. */
ID3D11Texture2D* g_frameCopy{};
ID3D11ShaderResourceView* g_frameView{};
D3D11_TEXTURE2D_DESC g_frameCopyDesc{};
/** Tick of the first frame the layer saw, which starts the boot limit. */
ULONGLONG g_firstFrameTick{};
/** Ticks of the closing sequence: its start, the title first seen, the fade-out starting. */
ULONGLONG g_closingStartTick{};
ULONGLONG g_titleSeenTick{};
ULONGLONG g_fadeOutTick{};
/** Tick the menus phase began, which starts its limit. */
ULONGLONG g_menusTick{};
/** How long the current closing takes to move the phase line. */
ULONGLONG g_ruleInMs{kRuleInMs};
/** Frames in a row that have read as the title screen. */
unsigned g_titleFrames{};
/** Why the closing began, reported on its first frame. */
const char* g_closingReason{"state"};
bool g_started{};
bool g_reportedStart{};
/** The frame just drawn was a bright screen inverted at strength (see covering()). */
bool g_covering{};
/** The frame just drawn was inverted or dimmed (see acting()); an emblem-only draw does not count. */
bool g_acting{};
bool g_reportedSampling{};
bool g_reportedClosing{};

[[nodiscard]] Phase phase() noexcept {
    return static_cast<Phase>(g_phase.load(std::memory_order_acquire));
}

void set_phase(Phase value) noexcept {
    g_phase.store(static_cast<int>(value), std::memory_order_release);
}

/** @param object COM object we own. Released and cleared when set. */
template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

/** Ends the layer for this process and says why. */
void finish(const char* reason) noexcept {
    if (phase() == Phase::done) {
        return;
    }
    set_phase(Phase::done);
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "ev=splash_invert stage=end reason=%s result=ok",
                      reason);
}

/** The title is under the layer: from here on only bright frames (the loading screen behind Enter) are touched. */
void enter_menus(ULONGLONG now) noexcept {
    int expected = static_cast<int>(Phase::closing);
    if (g_phase.compare_exchange_strong(
            expected, static_cast<int>(Phase::menus), std::memory_order_acq_rel)) {
        g_menusTick = now;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=splash_invert stage=menus result=ok");
    }
}

/** @param format Back-buffer format. @return How its texels are decoded, or unsupported. */
[[nodiscard]] Layout layout_of(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return Layout::rgba8;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return Layout::bgra8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return Layout::rgb10a2;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return Layout::rgba16Unorm;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return Layout::rgba16Float;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return Layout::rgba32Float;
    default:
        return Layout::unsupported;
    }
}

/** @param format Back-buffer format. @return The typed format a view of a typeless one reads with. */
[[nodiscard]] DXGI_FORMAT view_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return format;
    }
}

/** @param layout Decoded layout. @return Bytes one texel occupies in the staging row. */
[[nodiscard]] std::size_t texel_size(Layout layout) noexcept {
    switch (layout) {
    case Layout::rgba16Unorm:
    case Layout::rgba16Float:
        return sizeof(std::uint16_t) * 4;
    case Layout::rgba32Float:
        return sizeof(float) * 4;
    default:
        return sizeof(std::uint32_t);
    }
}

/** @return True for a layout whose values are linear light rather than encoded. */
[[nodiscard]] bool linear_light(Layout layout) noexcept {
    return layout == Layout::rgba16Unorm || layout == Layout::rgba16Float
           || layout == Layout::rgba32Float;
}

template <typename Value> [[nodiscard]] Value load(const std::byte* bytes) noexcept {
    Value value{};
    std::memcpy(&value, bytes, sizeof value);
    return value;
}

/** @param half IEEE binary16 bits. @return The same value as a float. */
[[nodiscard]] float half_to_float(std::uint16_t half) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000U) << 16;
    std::uint32_t exponent = (half >> 10) & 0x1FU;
    std::uint32_t mantissa = half & 0x3FFU;
    std::uint32_t bits = sign;
    if (exponent == 0) {
        if (mantissa != 0) {
            // Subnormal: shift the mantissa up until its leading bit is the implicit one.
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400U) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3FFU;
            bits |= (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1FU) {
        bits |= 0x7F800000U | (mantissa << 13);
    } else {
        bits |= ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

/** @param texel One texel in the given layout. @return Its alpha, 0..1 (diagnostic). */
[[nodiscard]] float texel_alpha(Layout layout, const std::byte* texel) noexcept {
    switch (layout) {
    case Layout::rgba8:
    case Layout::bgra8:
        return static_cast<float>(load<std::uint8_t>(texel + 3)) / kEightBitMax;
    case Layout::rgb10a2:
        return static_cast<float>(load<std::uint32_t>(texel) >> kTwoBitAlphaShift) / kTwoBitMax;
    case Layout::rgba16Unorm:
        return static_cast<float>(load<std::uint16_t>(texel + 6)) / kSixteenBitMax;
    case Layout::rgba16Float:
        return half_to_float(load<std::uint16_t>(texel + 6));
    case Layout::rgba32Float:
        return load<float>(texel + 3 * sizeof(float));
    default:
        return 0.0F;
    }
}

/** @param texel One texel in the given layout. Receives its red, green and blue (unclamped). */
void texel_channels(Layout layout, const std::byte* texel, float& red, float& green, float& blue) noexcept {
    red = 0.0F;
    green = 0.0F;
    blue = 0.0F;
    switch (layout) {
    case Layout::rgba8:
        red = static_cast<float>(load<std::uint8_t>(texel)) / kEightBitMax;
        green = static_cast<float>(load<std::uint8_t>(texel + 1)) / kEightBitMax;
        blue = static_cast<float>(load<std::uint8_t>(texel + 2)) / kEightBitMax;
        break;
    case Layout::bgra8:
        blue = static_cast<float>(load<std::uint8_t>(texel)) / kEightBitMax;
        green = static_cast<float>(load<std::uint8_t>(texel + 1)) / kEightBitMax;
        red = static_cast<float>(load<std::uint8_t>(texel + 2)) / kEightBitMax;
        break;
    case Layout::rgb10a2: {
        const std::uint32_t packed = load<std::uint32_t>(texel);
        red = static_cast<float>(packed & kTenBitMask) / kTenBitMax;
        green = static_cast<float>((packed >> kTenBitGreenShift) & kTenBitMask) / kTenBitMax;
        blue = static_cast<float>((packed >> kTenBitBlueShift) & kTenBitMask) / kTenBitMax;
        break;
    }
    case Layout::rgba16Unorm:
        red = static_cast<float>(load<std::uint16_t>(texel)) / kSixteenBitMax;
        green = static_cast<float>(load<std::uint16_t>(texel + 2)) / kSixteenBitMax;
        blue = static_cast<float>(load<std::uint16_t>(texel + 4)) / kSixteenBitMax;
        break;
    case Layout::rgba16Float:
        red = half_to_float(load<std::uint16_t>(texel));
        green = half_to_float(load<std::uint16_t>(texel + 2));
        blue = half_to_float(load<std::uint16_t>(texel + 4));
        break;
    case Layout::rgba32Float:
        red = load<float>(texel);
        green = load<float>(texel + sizeof(float));
        blue = load<float>(texel + 2 * sizeof(float));
        break;
    default:
        break;
    }
}

/** @param texel One texel in the given layout. @return Its channel mean, clamped to 0..1. */
[[nodiscard]] float texel_brightness(Layout layout, const std::byte* texel) noexcept {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    texel_channels(layout, texel, red, green, blue);
    const float mean = (red + green + blue) / kChannelCount;
    return std::isfinite(mean) ? std::clamp(mean, 0.0F, 1.0F) : 0.0F;
}

/** @param texel One texel in the given layout. @return True when its channels differ by more than kSaturatedTexel. */
[[nodiscard]] bool texel_saturated(Layout layout, const std::byte* texel) noexcept {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    texel_channels(layout, texel, red, green, blue);
    const float spread = (std::max)({red, green, blue}) - (std::min)({red, green, blue});
    return std::isfinite(spread) && spread > kSaturatedTexel;
}

/** Makes the two blend states: the fallback inversion, and a plain overwrite for the shader pass. */
[[nodiscard]] bool create_blends(ID3D11Device* device) noexcept {
    D3D11_BLEND_DESC description{};
    D3D11_RENDER_TARGET_BLEND_DESC& target = description.RenderTarget[0];
    target.BlendEnable = TRUE;
    // out = src * (1 - dest) + dest * 0, and the quad's src is 1, so out = 1 - dest.
    target.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    target.DestBlend = D3D11_BLEND_ZERO;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    // Alpha is left as the game wrote it.
    target.SrcBlendAlpha = D3D11_BLEND_ZERO;
    target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(device->CreateBlendState(&description, &g_invertBlend)) || g_invertBlend == nullptr) {
        return false;
    }
    D3D11_BLEND_DESC opaque{};
    D3D11_RENDER_TARGET_BLEND_DESC& overwrite = opaque.RenderTarget[0];
    overwrite.BlendEnable = FALSE;
    overwrite.RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    return SUCCEEDED(device->CreateBlendState(&opaque, &g_opaqueBlend)) && g_opaqueBlend != nullptr;
}

/** Compiles the shader once; a failure is reported once and leaves the shader null. */
void create_shader(ID3D11Device* device) noexcept {
    if (g_shaderTried) {
        return;
    }
    g_shaderTried = true;
    std::array<char, kShaderSourceCapacity> source{};
    const int written = std::snprintf(source.data(),
                                      source.size(),
                                      kShaderTemplate,
                                      static_cast<unsigned>(kFrameSlot),
                                      static_cast<double>(kBootLineStart),
                                      static_cast<double>(kClosingLineStart),
                                      static_cast<double>(kBootLineFull),
                                      static_cast<double>(kClosingLineFull),
                                      static_cast<double>(kDawnRed * kWashGain),
                                      static_cast<double>(kDawnGreen * kWashGain),
                                      static_cast<double>(kDawnBlue * kWashGain),
                                      static_cast<double>(kBootTintRed * kBootGain),
                                      static_cast<double>(kBootTintGreen * kBootGain),
                                      static_cast<double>(kBootTintBlue * kBootGain),
                                      static_cast<double>(kClosingInvertedGain));
    if (written <= 0 || static_cast<std::size_t>(written) >= source.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=splash_invert stage=shader result=fail reason=source");
        return;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    const HRESULT compiled = D3DCompile(source.data(),
                                        static_cast<std::size_t>(written),
                                        "dawn_splash_invert",
                                        nullptr,
                                        nullptr,
                                        "main",
                                        "ps_4_0",
                                        0,
                                        0,
                                        &code,
                                        &messages);
    if (FAILED(compiled) || code == nullptr) {
        const char* text = messages != nullptr ? static_cast<const char*>(messages->GetBufferPointer()) : "";
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=splash_invert stage=shader result=fail reason=compile message=%.*s",
                          kCompilerMessageLimit,
                          text);
    } else if (FAILED(device->CreatePixelShader(
                   code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_shader))
               || g_shader == nullptr) {
        g_shader = nullptr;
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=splash_invert stage=shader result=fail reason=create");
    } else {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=splash_invert stage=shader result=ok");
    }
    release_com(messages);
    release_com(code);
}

/** Makes the constant buffer the emblem rule's parameters travel in. */
[[nodiscard]] bool ensure_params(ID3D11Device* device) noexcept {
    if (g_params != nullptr) {
        return true;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(EmblemParams);
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&description, nullptr, &g_params)) || g_params == nullptr) {
        g_params = nullptr;
        return false;
    }
    return true;
}

/** @return The emblem rule's constant buffer contents for this frame. */
[[nodiscard]] EmblemParams emblem_params(const D3D11_TEXTURE2D_DESC& shape, float strength) noexcept {
    EmblemParams params{};
    params.strength = strength;
    params.ratio = kBootTintRed / kBootTintBlue;
    params.chromaLow = kEmblemChromaLow;
    params.chromaHigh = kEmblemChromaHigh;
    params.lumaCap = kEmblemLumaCap;
    params.gain = kEmblemGain;
    params.left = kEmblemLeft * static_cast<float>(shape.Width);
    params.top = kEmblemTop * static_cast<float>(shape.Height);
    params.right = kEmblemRight * static_cast<float>(shape.Width);
    params.bottom = kEmblemBottom * static_cast<float>(shape.Height);
    params.tintRed = kBootTintRed / kTintLuma;
    params.tintGreen = kBootTintGreen / kTintLuma;
    params.tintBlue = kBootTintBlue / kTintLuma;
    return params;
}

/** Keeps one staging row in the back buffer's format, remade when the format changes. */
[[nodiscard]] bool ensure_staging(ID3D11Device* device, DXGI_FORMAT format) noexcept {
    if (g_staging != nullptr && g_stagingFormat == format) {
        return true;
    }
    release_com(g_staging);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kSampleCount;
    description.Height = 1;
    description.MipLevels = kSingleLevel;
    description.ArraySize = kSingleLevel;
    description.Format = format;
    description.SampleDesc.Count = kSingleSample;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &g_staging)) || g_staging == nullptr) {
        g_staging = nullptr;
        return false;
    }
    g_stagingFormat = format;
    return true;
}

/** Keeps a shader-readable copy of the back buffer's shape, remade when that shape changes. */
[[nodiscard]] bool ensure_frame_copy(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& shape) noexcept {
    if (g_frameCopy != nullptr && g_frameView != nullptr && g_frameCopyDesc.Width == shape.Width
        && g_frameCopyDesc.Height == shape.Height && g_frameCopyDesc.Format == shape.Format) {
        return true;
    }
    release_com(g_frameView);
    release_com(g_frameCopy);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = shape.Width;
    description.Height = shape.Height;
    description.MipLevels = kSingleLevel;
    description.ArraySize = kSingleLevel;
    description.Format = shape.Format;
    description.SampleDesc.Count = kSingleSample;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &g_frameCopy)) || g_frameCopy == nullptr) {
        g_frameCopy = nullptr;
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = view_format(shape.Format);
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = kSingleLevel;
    if (FAILED(device->CreateShaderResourceView(g_frameCopy, &view, &g_frameView)) || g_frameView == nullptr) {
        g_frameView = nullptr;
        release_com(g_frameCopy);
        return false;
    }
    g_frameCopyDesc = description;
    return true;
}

/**
 * Reads a grid of texels of the finished game frame and sums them up.
 * The map waits for the copies, which is a small stall the boot screens can afford; the layer
 * stops sampling once it is done.
 * @param reading Receives the mean, the darkest texel and the blob and bright shares.
 * @return False when the back buffer cannot be sampled (multisampled or an unknown format).
 */
[[nodiscard]] bool sample_frame(ID3D11Device* device,
                                ID3D11DeviceContext* context,
                                ID3D11Texture2D* backBuffer,
                                const D3D11_TEXTURE2D_DESC& shape,
                                Reading& reading) noexcept {
    const Layout layout = layout_of(shape.Format);
    bool sampled = false;
    if (layout != Layout::unsupported && shape.SampleDesc.Count == kSingleSample
        && shape.Width >= kSampleColumns && shape.Height >= kSampleRows
        && ensure_staging(device, shape.Format)) {
        for (UINT index = 0; index < kSampleCount; ++index) {
            const UINT column = index % kSampleColumns;
            const UINT row = index / kSampleColumns;
            // Cell centres of the grid, so the corners and the exact middle are both avoided.
            const UINT x = (shape.Width * (2 * column + 1)) / (2 * kSampleColumns);
            const UINT y = (shape.Height * (2 * row + 1)) / (2 * kSampleRows);
            const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
            context->CopySubresourceRegion(g_staging, 0, index, 0, 0, backBuffer, 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            if (mapped.pData != nullptr) {
                const auto* texels = static_cast<const std::byte*>(mapped.pData);
                const std::size_t stride = texel_size(layout);
                float total = 0.0F;
                float alphaTotal = 0.0F;
                float darkest = 1.0F;
                unsigned blob = 0;
                unsigned bright = 0;
                unsigned saturated = 0;
                for (UINT index = 0; index < kSampleCount; ++index) {
                    float value = texel_brightness(layout, texels + index * stride);
                    if (linear_light(layout)) {
                        // Linear light reads far darker than it looks; a square root is close
                        // to the encoded scale the thresholds were chosen on.
                        value = std::sqrt(value);
                    }
                    const float alpha = texel_alpha(layout, texels + index * stride);
                    alphaTotal += alpha;
                    reading.alphaMin = (std::min)(reading.alphaMin, alpha);
                    reading.alphaMax = (std::max)(reading.alphaMax, alpha);
                    total += value;
                    darkest = (std::min)(darkest, value);
                    blob += value < kBlobSample ? 1U : 0U;
                    bright += value > kBrightSample ? 1U : 0U;
                    saturated += texel_saturated(layout, texels + index * stride) ? 1U : 0U;
                }
                reading.mean = total / static_cast<float>(kSampleCount);
                reading.darkest = darkest;
                reading.blobShare = static_cast<float>(blob) / static_cast<float>(kSampleCount);
                reading.brightShare = static_cast<float>(bright) / static_cast<float>(kSampleCount);
                reading.saturatedShare = static_cast<float>(saturated) / static_cast<float>(kSampleCount);
                reading.alphaMean = alphaTotal / static_cast<float>(kSampleCount);
                sampled = true;
            }
            context->Unmap(g_staging, 0);
        }
    }
    if (!g_reportedSampling) {
        g_reportedSampling = true;
        core::log::writef(core::log::Channel::client,
                          sampled ? core::log::Level::info : core::log::Level::warn,
                          "ev=splash_invert stage=sample format=%u width=%u height=%u samples=%u "
                          "grid=%ux%u result=%s",
                          static_cast<unsigned>(shape.Format),
                          static_cast<unsigned>(shape.Width),
                          static_cast<unsigned>(shape.Height),
                          static_cast<unsigned>(shape.SampleDesc.Count),
                          static_cast<unsigned>(kSampleColumns),
                          static_cast<unsigned>(kSampleRows),
                          sampled ? "ok" : "unsupported");
    }
    return sampled;
}

/** Diagnostic: one line with the reading the decisions are made on (debug level). */
void trace(const char* stage, ULONGLONG elapsed, bool sampled, const Reading& reading) noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::debug)) {
        return;
    }
    core::log::writef(core::log::Channel::client,
                      core::log::Level::debug,
                      "ev=splash_invert stage=%s t=%llu sampled=%u mean=%.3f darkest=%.3f blob=%.2f "
                      "bright=%.2f alpha=%.3f/%.3f/%.3f title_frames=%u fading=%u",
                      stage,
                      static_cast<unsigned long long>(elapsed),
                      sampled ? 1U : 0U,
                      static_cast<double>(reading.mean),
                      static_cast<double>(reading.darkest),
                      static_cast<double>(reading.blobShare),
                      static_cast<double>(reading.brightShare),
                      static_cast<double>(reading.alphaMin),
                      static_cast<double>(reading.alphaMean),
                      static_cast<double>(reading.alphaMax),
                      g_titleFrames,
                      g_fadeOutTick != 0 ? 1U : 0U);
}

/** Draw callback: binds the fallback inversion blend for the quad that follows it. */
void set_invert_state(const ImDrawList* /*parent*/, const ImDrawCmd* /*command*/) noexcept {
    auto* state =
        static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (state == nullptr || state->DeviceContext == nullptr || g_invertBlend == nullptr) {
        return;
    }
    constexpr std::array<float, 4> factor{};
    state->DeviceContext->OMSetBlendState(g_invertBlend, factor.data(), kAllSamples);
}

/** Draw callback: binds the shader, the frame copy and a plain overwrite. */
void set_shader_state(const ImDrawList* /*parent*/, const ImDrawCmd* /*command*/) noexcept {
    auto* state =
        static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (state == nullptr || state->DeviceContext == nullptr || g_shader == nullptr
        || g_frameView == nullptr || g_opaqueBlend == nullptr) {
        return;
    }
    ID3D11DeviceContext* context = state->DeviceContext;
    context->PSSetShader(g_shader, nullptr, 0);
    ID3D11ShaderResourceView* views[]{g_frameView};
    context->PSSetShaderResources(kFrameSlot, 1, views);
    if (g_params != nullptr) {
        ID3D11Buffer* buffers[]{g_params};
        context->PSSetConstantBuffers(kParamsSlot, 1, buffers);
    }
    context->OMSetBlendState(g_opaqueBlend, nullptr, kAllSamples);
}

/** Draw callback: drops the frame copy binding, which the reset that follows does not touch. */
void clear_shader_state(const ImDrawList* /*parent*/, const ImDrawCmd* /*command*/) noexcept {
    auto* state =
        static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (state == nullptr || state->DeviceContext == nullptr) {
        return;
    }
    ID3D11ShaderResourceView* views[]{nullptr};
    state->DeviceContext->PSSetShaderResources(kFrameSlot, 1, views);
    ID3D11Buffer* buffers[]{nullptr};
    state->DeviceContext->PSSetConstantBuffers(kParamsSlot, 1, buffers);
}

/** Adds the fallback inverting quad over the whole viewport. */
void draw_inversion(const ImGuiViewport& viewport) noexcept {
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddCallback(&set_invert_state, nullptr);
    background->AddRectFilled(viewport.Pos,
                              {viewport.Pos.x + viewport.Size.x, viewport.Pos.y + viewport.Size.y},
                              IM_COL32_WHITE);
    background->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

/** @return One vertex colour channel for a 0..1 control value. */
[[nodiscard]] int channel(float value) noexcept {
    return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * kColourScale));
}

/** Adds the shader quad over the whole viewport with this frame's controls in its colour. */
void draw_shader_pass(const ImGuiViewport& viewport, const Controls& controls) noexcept {
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddCallback(&set_shader_state, nullptr);
    background->AddRectFilled(
        viewport.Pos,
        {viewport.Pos.x + viewport.Size.x, viewport.Pos.y + viewport.Size.y},
        IM_COL32(channel(controls.strength), channel(controls.phaseLine), channel(controls.washGain), channel(controls.dim)));
    background->AddCallback(&clear_shader_state, nullptr);
    background->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

/** Starts the closing sequence, if the layer is still in its boot phase. */
void begin_closing(const char* reason, ULONGLONG ruleInMs) noexcept {
    int expected = static_cast<int>(Phase::boot);
    if (g_phase.compare_exchange_strong(
            expected, static_cast<int>(Phase::closing), std::memory_order_acq_rel)) {
        g_closingReason = reason;
        g_ruleInMs = ruleInMs;
    }
}

/**
 * Runs one frame of the closing sequence: move the phase line, look for the title, fade out.
 * @param controls This frame's shader controls, completed here.
 * @return True when something should still be drawn; false once the layer is over.
 */
[[nodiscard]] bool run_closing(ULONGLONG now, bool sampled, const Reading& reading, Controls& controls) noexcept {
    if (g_closingStartTick == 0) {
        g_closingStartTick = now;
    }
    if (!g_reportedClosing) {
        g_reportedClosing = true;
        core::log::writef(core::log::Channel::client,
                          core::log::Level::info,
                          "ev=splash_invert stage=closing reason=%s shader=%u result=ok",
                          g_closingReason,
                          g_shader != nullptr ? 1U : 0U);
    }
    const ULONGLONG elapsed = now - g_closingStartTick;
    if (elapsed <= kTraceMs) {
        trace("trace", elapsed, sampled, reading);
    }
    controls.phaseLine = g_ruleInMs == 0 ? 1.0F
                                         : (std::min)(1.0F,
                                                      static_cast<float>(elapsed)
                                                          / static_cast<float>(g_ruleInMs));
    controls.washGain = kWashBlend;
    if (g_fadeOutTick != 0) {
        const ULONGLONG fading = now - g_fadeOutTick;
        if (fading >= kFadeOutMs) {
            enter_menus(now);
            return false;
        }
        controls.strength = 1.0F - static_cast<float>(fading) / static_cast<float>(kFadeOutMs);
        return true;
    }
    bool fade = elapsed >= kClosingLimitMs;
    if (elapsed >= kHoldMs) {
        if (!sampled) {
            fade = fade || elapsed >= kBlindHoldMs;
        } else if (reading.mean > kTitleMeanMin && reading.mean <= kTitleMeanMax
                   && reading.brightShare <= kTitleBrightShare) {
            ++g_titleFrames;
            if (g_titleFrames >= kTitleFramesNeeded) {
                if (g_titleSeenTick == 0) {
                    g_titleSeenTick = now;
                }
                fade = fade || now - g_titleSeenTick >= kFadeDelayMs;
            }
        } else {
            g_titleFrames = 0;
            g_titleSeenTick = 0;
        }
    }
    if (fade) {
        g_fadeOutTick = now;
    }
    return true;
}

} // namespace

/** Adds the boot-screen layer to the current frame. */
bool draw(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    g_covering = false;
    g_acting = false;
    if (phase() == Phase::done) {
        return false;
    }
    if (swapChain == nullptr || device == nullptr || context == nullptr) {
        return false;
    }
    if (!core::settings::get().client.invertSplashScreens) {
        set_phase(Phase::done);
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=splash_invert stage=setting enabled=0 result=skip");
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_started) {
        g_started = true;
        g_firstFrameTick = now;
    }
    if (phase() != Phase::menus && now - g_firstFrameTick > kBootLimitMs) {
        finish("timeout");
        return false;
    }
    if (phase() == Phase::menus && now - g_menusTick > kMenusLimitMs) {
        finish("menus_timeout");
        return false;
    }
    if (g_invertBlend == nullptr && !create_blends(device)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=splash_invert stage=blend result=fail");
        finish("blend_state");
        return false;
    }
    create_shader(device);
    (void)ensure_params(device);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return false;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(
            kBackBufferIndex, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))
        || backBuffer == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC shape{};
    backBuffer->GetDesc(&shape);

    Reading reading{};
    const bool sampled = sample_frame(device, context, backBuffer, shape, reading);
    if (!g_reportedStart) {
        g_reportedStart = true;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=splash_invert stage=start result=ok");
    }
    if (now - g_firstFrameTick <= kStartTraceMs && sampled) {
        trace("trace0", now - g_firstFrameTick, sampled, reading);
    }
    if (phase() == Phase::boot && sampled && reading.mean > kBlobFrameMean
        && reading.blobShare >= kBlobShareStart) {
        // The title showing through a bright splash: the dissolve has begun.
        begin_closing("dissolve", kRuleInFastMs);
    }
    const bool shaderReady = g_shader != nullptr && shape.SampleDesc.Count == kSingleSample
                             && ensure_frame_copy(device, shape);
    bool drawn = false;
    if (shaderReady) {
        Controls controls{};
        bool live = true;
        if (phase() == Phase::closing) {
            live = run_closing(now, sampled, reading, controls);
        } else {
            // Boot and menus phases: the wash only on a bright frame, so a fade or a gap stays dark.
            controls.washGain = !sampled || reading.mean > kWashFrameMean ? kWashBlend : 0.0F;
        }
        if (sampled) {
            // Only a bright frame (the loading screen) is inverted; the splash is dark by its textures.
            const float gate = std::clamp((reading.mean - kGateDark) / (kGateBright - kGateDark), 0.0F, 1.0F);
            controls.strength *= gate * gate * (3.0F - 2.0F * gate);
            // A crossfade between a bright screen and a dark one is a mid-grey frame that the gate would let
            // through half raw: the output is dimmed so that a uniform frame of this mean reads kTargetLuma at most.
            const float invertedLuma = (1.0F - reading.mean) * kBootGain * kTintLuma;
            const float predicted = reading.mean + (invertedLuma - reading.mean) * controls.strength;
            const float floor = std::clamp((reading.mean - kDimFloorLow) / (kDimFloorHigh - kDimFloorLow), 0.0F, 1.0F);
            const float wanted = (std::min)(1.0F, kTargetLuma / (std::max)(predicted, 0.001F));
            controls.dim = 1.0F + (wanted - 1.0F) * floor * floor * (3.0F - 2.0F * floor);
            if (phase() == Phase::menus) {
                // The loading emblem of the marble waiting screen: a dark frame with nothing saturated in it
                // (character select's cards and the title's icon are).
                const float off = std::clamp((reading.mean - kEmblemMeanOn) / (kEmblemMeanOff - kEmblemMeanOn), 0.0F, 1.0F);
                const float coloured = std::clamp((reading.saturatedShare - kEmblemSaturatedStart)
                                                      / (kEmblemSaturatedFull - kEmblemSaturatedStart),
                                                  0.0F, 1.0F);
                controls.emblem = (1.0F - off * off * (3.0F - 2.0F * off))
                                  * (1.0F - coloured * coloured * (3.0F - 2.0F * coloured));
            }
        }
        if (live && (controls.strength > 0.0F || controls.dim < 1.0F || controls.emblem > 0.0F)) {
            if (g_params != nullptr) {
                const EmblemParams params = emblem_params(shape, controls.emblem);
                context->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
            }
            context->CopyResource(g_frameCopy, backBuffer);
            draw_shader_pass(*viewport, controls);
            drawn = true;
            g_covering = controls.strength >= kCoveringStrength;
            g_acting = controls.strength > 0.0F || controls.dim < 1.0F;
        }
    } else if (phase() == Phase::closing && sampled && reading.mean <= kBrightThreshold) {
        // Fallback without the shader: plain inversion until the first dark frame of the closing.
        enter_menus(now);
    } else if (!sampled || reading.mean > kBrightThreshold) {
        draw_inversion(*viewport);
        drawn = true;
    }
    backBuffer->Release();
    return drawn;
}

/** Frees the layer's device objects. The phase survives, so a device change mid-boot carries on. */
void release() noexcept {
    release_com(g_frameView);
    release_com(g_frameCopy);
    g_frameCopyDesc = {};
    release_com(g_shader);
    g_shaderTried = false;
    release_com(g_params);
    release_com(g_staging);
    g_stagingFormat = DXGI_FORMAT_UNKNOWN;
    release_com(g_opaqueBlend);
    release_com(g_invertBlend);
}

/** Records that the client entered the title-screen state, which starts the closing sequence. */
void note_title_screen() noexcept {
    begin_closing("state", kRuleInMs);
}

void note_state(std::string_view name) noexcept {
    if (name == "bootflow:start") {
        note_title_screen();
    } else if (!name.starts_with("bootflow:") && !name.starts_with("character:")
               && phase() == Phase::menus) {
        // The world is loading: whatever is bright from now on is the game's own.
        finish("state");
    }
}

bool released() noexcept {
    return phase() == Phase::menus || phase() == Phase::done;
}

bool covering() noexcept {
    return g_covering;
}

bool acting() noexcept {
    return g_acting;
}

bool sample_mean(ID3D11Device* device,
                 ID3D11DeviceContext* context,
                 ID3D11Texture2D* backBuffer,
                 const D3D11_TEXTURE2D_DESC& shape,
                 float& mean) noexcept {
    Reading reading{};
    if (!sample_frame(device, context, backBuffer, shape, reading)) {
        return false;
    }
    mean = reading.mean;
    return true;
}

} // namespace dawn::client::hooks::graphics::renderer::splash_invert
