/**
 * Title-screen filigree: two Julia-set medallions, mirror images of each other about the vertical
 * centre line of the screen, drawn by the DLL over the finished title-screen frame and keyed under
 * everything bright (the logo, the text), morphing in place as their Julia constant walks a small
 * loop with time.
 *
 * Why the DLL draws this rather than a bootflow texture: the game draws slot 80A14604 as two copies
 * at (708, 105) and (1308, 157) of a 1080p frame and spins them at 3.4 and 4.3 deg/s in opposite
 * directions with unrelated phases, so those two can never be mirror images, and a spinning texture
 * cannot change shape in place: every feature drifts along its arc, and any variation baked into
 * the texture reads as rotation. A pixel shader has neither problem. It evaluates the LEFT
 * medallion for every pixel (a pixel on the right half is reflected onto the left half, and within
 * a band either side of the centre line the reflected and the continued point are blended, so the
 * seam is a soft fold rather than a crease) and moves the constant with time.
 *
 * Every frame while the title screen is up the frame is copied, and one full-screen quad in Dear
 * ImGui's background draw list runs the shader: it reads the copy at its own pixel, computes the
 * escape time of z -> z^2 + c for the pixel's place in the medallion, draws thin lines where the
 * escape time crosses a band boundary (width from the screen-space derivative, so lines stay about
 * a pixel and a half wide; lines packed tighter than a couple of pixels fade out instead of
 * aliasing), fades them radially, keys them under bright frame pixels and writes the frame with
 * the lines blended in. Points that do not escape within the budget, the fuzz packed against the
 * dendrite, are left undrawn.
 *
 * The blue of the logo is a hitbox. Once the title is up the frame is read back and its brand-blue
 * pixels are masked. The mask is blurred and its gradient becomes a pull (a small texture) that
 * moves every sampling point towards the logo, so the Julia plane is compressed against the logo
 * and the lines lean along it; and the mask's exact distance transform, at full resolution in a
 * second texture, stops the lines a couple of pixels short of the blue and draws one outline line
 * there, so the rays, the disc and the reflection lines are traced to the pixel and nothing crosses
 * them.
 *
 * The layer starts when the retail log has reported the title state and the boot-screen inversion
 * has let go of the frame, but draws nothing until the logo's blue has been found on the frame (the
 * inversion's title test passes on the dark Bungie splash too, and the filigree used to fade in there);
 * then it fades in, hides the moment the frame turns bright and ends once that has lasted a second (the
 * loading screen behind Enter; the title is never bright, but one stray bright frame must not cost it the
 * filigree), or ends on the first state outside the boot flow (character select; the sign-in states run
 * under the title and are ignored), or when the setting is off.
 */

#include "graphics_title_filigree.h"

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
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <string_view>
#include <vector>

#include "../../../../core/logging/log.h"
#include "graphics_splash_invert.h"

namespace dawn::client::hooks::graphics::renderer::title_filigree {
namespace {

// The look. preview_dll_filigree.py in the texture package is a port of the shader with the same
// names, for tuning these without a build.
/** Left medallion centre as a fraction of the render width and height (672, 140 at 1080p). Until
 * 2026-09-20 late evening it was (0.369, 0.097), the centre of the game's own 80A14604 copy. */
constexpr float kCentreX = 0.35F;
constexpr float kCentreY = 0.13F;
/** Medallion radius as a fraction of the render height (918 px at 1080p). Enlarged from 0.533
 * (576 px) on 2026-09-20 late evening ("the moving symmetrical patterns at the top should take up
 * more of the screen"): the pair now reaches the sides and about three quarters of the way down,
 * and stays clear of the corner-V medallions and the prompt. */
constexpr float kRadius = 0.85F;
/** Fraction of the radius where the radial fade begins (0.53 before the enlargement). */
constexpr float kFadeStart = 0.50F;
/** Julia-plane half-extent at the radius. */
constexpr float kSpan = 1.2F;
/** Centre of the loop the Julia constant walks: Seahorse Valley, outside the Mandelbrot set. */
constexpr float kConstantReal = -0.765F;
constexpr float kConstantImag = 0.18F;
/** Loop half-axes (real, imaginary). The gap between the cardioid and the period-2 bulb is only
 * ~0.07 wide in the real direction here, so the loop is long in the imaginary one; all 720 sampled
 * loop points were checked to stay outside the set, so no medallion ever grows a solid interior. */
constexpr float kLoopReal = 0.02F;
constexpr float kLoopImag = 0.055F;
/** Seconds per loop: the morph cycle (16 until "can we make the movement slightly slower", 2026-09-21). */
constexpr float kMorphPeriodS = 20.0F;
/** Degrees per second the left medallion turns (the right one counter-turns); 0 is morph only. */
constexpr float kSpinDegPerSec = 0.0F;
/** Contour lines per escape-time period, and that period. */
constexpr float kBands = 5.0F;
constexpr float kBandPeriod = 8.0F;
/** Bands per second the contour lines crawl inwards (the palette-cycle "zoom"; 0.08 until the slow-down). */
constexpr float kCrawlBandsPerSec = 0.06F;
/** Escape-iteration budget. */
constexpr int kBudget = 70;
/** On-screen opacity of a line, and its half-width in pixels. */
constexpr float kLineAlpha = 0.16F;
constexpr float kLinePixels = 1.3F;
/** Lines packed tighter than this many bands per pixel fade out over the next range. */
constexpr float kPackStart = 0.5F;
constexpr float kPackWidth = 0.3F;
/** Frame luma between which the filigree fades out, so it sits under the logo and the text. */
constexpr float kKeepLow = 0.20F;
constexpr float kKeepHigh = 0.30F;
/** Half-width in pixels of the soft mirror seam at the centre line. */
constexpr float kBlendPixels = 80.0F;
/** The logo as a solid wall ("the background lines should bounce off the dawn logo and treat it as a
 * solid wall", then "the white fractal waves should touch the borders of the blue dawn logo, not
 * disappear around a radius of it", 2026-09-20). The wall is the logo's own outline, read off the
 * frame: a mask of its brand-blue texels is blurred and the sampling point is pulled towards the
 * logo by the blurred mask's gradient. An ellipse round the icon with a potential-flow map was tried
 * first and rejected for the empty margin it left. */
constexpr bool kWall = true;
/** Blur of the logo mask, as a fraction of the frame height (43 px at 1080p). */
constexpr float kWallSigma = 0.040F;
/** Pull of the sampling point at a straight logo edge, as a fraction of the frame height (70 px at
 * 1080p); a straight edge's blurred-mask gradient peaks at 1 / (sigma * sqrt(2 pi)). */
constexpr float kWallPush = 0.065F;
/** A frame texel is logo when its blue is above this and its red below the next (stored values):
 * the icon's #0859F2 and the DAWN glow, not the white text. */
constexpr float kWallBlueMin = 0.5F;
constexpr float kWallRedMax = 0.35F;
/** The field is computed at this fraction of the frame's resolution (a 4x4 block average). */
constexpr unsigned kWallDownsample = 4;
/** At least this many logo cells at that resolution (the icon is ~3800 at 1080p) or the frame is not
 * the title yet (the dark Bungie splash, most often) and the field is tried again later; nothing is drawn
 * until it succeeds... */
constexpr unsigned kWallMinCells = 800;
/** ...every this many frames, at most this often (the splash can sit under the layer for ten seconds). */
constexpr unsigned kWallRetryFrames = 16;
constexpr unsigned kWallMaxAttempts = 600;
/** Weight of that pull (0 turns it off, the hitbox zone stays). */
constexpr float kWallPull = 1.0F;
/** The hitbox ("treat the blue of the logo as a hitbox", 2026-09-20): one outline line runs round the
 * blue with this gap in pixels between the blue and the line's inner edge ("accurate to the blue + 2
 * pixel gap"); its centre sits kLinePixels further out, and the fractal's lines stop at its outer
 * edge. In pixels at any resolution, like the line width. Earlier versions blended the band coordinate
 * into the distance to the blue over a zone (36 px with a line every 12 px: "many parallel lines
 * around the borders"; then 22 px with one line): the blend swept the unwrapped Julia coordinate
 * through tens of bands inside the zone, packing lines so tightly that the anti-aliasing rule faded
 * them out, which left an empty halo round the logo ("why is there weird spacing around the logo"). */
constexpr float kWrapGapPixels = 2.0F;
/** The distance texture holds distances up to this many pixels, in 256 steps (a quarter pixel). */
constexpr float kDistanceRangePixels = 64.0F;
/** "The spirals are still rendering through the logo ... make them go around and highlight the path
 * intersection" (2026-09-21). Any warp along the outline's normal shows the same continuation on both
 * sides of a ray, so the eye joins the lines across the blue. So: a tangential sweep along the outline,
 * this many pixels at the outline, falling off with the next e-folding distance, whose direction is
 * opposite on the two sides of a ray (the two sides no longer line up, and lines curve into the
 * outline instead of meeting it square); suppressed where the exact normal and the blurred pull
 * disagree, which is the narrow gaps and the medial axes between rays, where it would kink. */
constexpr float kSwirlPixels = 14.0F;
constexpr float kSwirlRangePixels = 7.0F;
/** The path intersection: a line brightens by up to this much (times its coverage) as it ends at the
 * outline, and the outline brightens and whitens by the same where a line crosses it. */
constexpr float kJunctionGain = 1.5F;
/** The DAWN wordmark breathes ("make the dawn text pulse with a glow slightly", 2026-09-21). The wordmark is a
 * static texture the game draws once, so the pulse is this layer's: inside a window round the wordmark strip
 * the light glyphs are found on the frame (bright and low in saturation; nothing else in the window is, the
 * icon and the strip's own glow being blue), blurred into a soft halo that is added in the colour below, and
 * lifted a little themselves, all scaled by a cosine of this period (seconds per breath)... */
constexpr float kPulsePeriodS = 4.0F;
/** ...the halo's peak strength next to a glyph (additive, times the colour)... */
constexpr float kPulseGain = 0.45F;
/** ...the halo's Gaussian sigma in pixels (sampled every half sigma out to two sigma)... */
constexpr float kPulseSigmaPixels = 8.0F;
/** ...and the glyphs' own lift at the peak (times their colour). */
constexpr float kPulseCoreGain = 0.10F;
/** The window, as fractions of the frame: the wordmark strip (render y 542..602 at 1080p, DAWN at 557..588)
 * with a margin for the halo, below DESTINY 2 (ends at 0.46) and above the prompt (0.73). */
constexpr float kPulseLeft = 0.40F;
constexpr float kPulseTop = 0.49F;
constexpr float kPulseRight = 0.60F;
constexpr float kPulseBottom = 0.565F;
/** A frame pixel is a glyph as its darkest channel rises between these (the core is (205, 228, 255) so its
 * darkest channel is 0.80; the blue glow's and the icon's red is near 0). */
constexpr float kPulseGlyphLow = 0.55F;
constexpr float kPulseGlyphHigh = 0.75F;
/** Halo colour: between the wordmark's inner glow (96, 156, 255) and its core. */
constexpr std::array<float, 3> kPulseColour{110.0F / 255.0F, 170.0F / 255.0F, 1.0F};
/** Texture slots the pull field and the distance are read from, and their sampler's slot (Dear ImGui's
 * sampler is slot 0). */
constexpr UINT kWallSlot = 2;
constexpr UINT kDistanceSlot = 3;
constexpr UINT kWallSamplerSlot = 1;
/** Bytes per texel of the frame layouts the field can be built from. */
constexpr unsigned kFrameTexelBytes = 4;
/** Channels of a pull-field texel: pull x, pull y (pixels). */
constexpr unsigned kFieldChannels = 2;
/** Squared distance of a cell that is not the logo before the transform runs. */
constexpr float kFarSquared = 1.0e12F;
constexpr float kByteMax = 255.0F;
/** Line colours, cycled by band. */
constexpr std::array<float, 3> kColourA{150.0F / 255.0F, 205.0F / 255.0F, 1.0F};
constexpr std::array<float, 3> kColourB{70.0F / 255.0F, 140.0F / 255.0F, 1.0F};
constexpr std::array<float, 3> kColourC{205.0F / 255.0F, 232.0F / 255.0F, 1.0F};

/** The filigree fades in over this once it starts... */
constexpr ULONGLONG kFadeInMs = 1'500;
/** ...and out over this once the client leaves the title state. */
constexpr ULONGLONG kFadeOutMs = 600;
/** The frame is sampled every this many frames for the bright-frame end. */
constexpr unsigned kSampleEveryFrames = 16;
/** A frame this bright on average is not the title (the loading screen behind Enter): the layer hides... */
constexpr float kBrightMean = 0.5F;
/** ...and ends once the brightness has lasted this long. */
constexpr ULONGLONG kBrightEndMs = 1'000;
/** Buffer 0 is the back buffer the game just finished drawing. */
constexpr UINT kBackBufferIndex = 0;
/** One mip, one slice, one sample: plain 2D copy targets. */
constexpr UINT kSingleLevel = 1;
constexpr UINT kSingleSample = 1;
/** Every channel of the render target takes part in the blend. */
constexpr UINT kAllSamples = 0xFFFFFFFFU;
/** Texture slot the shader reads the frame copy from; slot 0 is Dear ImGui's own. */
constexpr UINT kFrameSlot = 1;
/** Pixel-shader constant-buffer slot; Dear ImGui's pixel shader binds none. */
constexpr UINT kParamsSlot = 0;
/** Shader-source buffer. The filled template must fit or the layer logs `stage=shader result=fail
 * reason=source` and never draws (the sweep of 2026-09-21 pushed it past the old 6144 unnoticed until the
 * spirals vanished; compile_check.py now checks the size before a build). */
constexpr std::size_t kShaderSourceCapacity = 16384;
/** Longest compiler message that is copied into the log. */
constexpr int kCompilerMessageLimit = 200;
constexpr float kDegreesToRadians = 3.14159265358979F / 180.0F;
constexpr float kTwoPi = 2.0F * 3.14159265358979F;
constexpr float kMillisecondsPerSecond = 1000.0F;

/** What the shader takes for one frame. Sixteen-byte rows, as a constant buffer wants. */
struct alignas(16) Params {
    float width, height, time, strength;
    float centreX, centreY, radius, fadeStart;
    float span, spinRadians, blendPixels, crawlPhase;
    float constantReal, constantImag, keepLow, keepHigh;
    float bands, bandPeriod, lineAlpha, linePixels;
    float packStart, packWidth, wallPull, wallOn;
    float outlineCentre, outlineHalfWidth, padding1, distanceRange;
    float swirlPixels, swirlRange, junctionGain, padding2;
    float pulseLevel, pulseGain, pulseSigma, pulseCoreGain;
    float pulseLeft, pulseTop, pulseRight, pulseBottom;
    float pulseGlyphLow, pulseGlyphHigh, padding3, padding4;
    float pulseColour[4];
    float colourA[4];
    float colourB[4];
    float colourC[4];
};
static_assert(sizeof(Params) % 16 == 0, "constant buffers are sized in 16-byte rows");

/**
 * The shader. It shares Dear ImGui's vertex layout, reads the frame copy at its own pixel and takes
 * its parameters from the constant buffer.
 */
constexpr const char* kShaderTemplate = R"(
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
Texture2D frame : register(t%u);
Texture2D wallField : register(t%u);
Texture2D wallDistance : register(t%u);
SamplerState wallSampler : register(s%u);
cbuffer Params : register(b%u)
{
    float4 frameTime;   // width, height, time, strength
    float4 medallion;   // centre x, centre y (fractions), radius (fraction of height), fade start
    float4 shape;       // span, spin (radians), blend half-width (px), crawl phase (bands)
    float4 constant;    // c real, c imaginary, keep low, keep high
    float4 bandsLook;   // bands, band period, line alpha, line half-width (px)
    float4 packing;     // pack start, pack width, pull weight, wall on (0 / 1)
    float4 wrap;        // outline line centre (px from the blue), its half-width (px), unused, distance texture range (px)
    float4 swirl;       // sweep at the outline (px), its e-folding range (px), junction gain, unused
    float4 pulse;       // wordmark pulse: level this frame (0..1), halo gain, halo sigma (px), glyph lift
    float4 pulseBox;    // window the wordmark's glyphs are looked for in: left, top, right, bottom (fractions)
    float4 pulseGlyph;  // darkest-channel thresholds a frame pixel becomes a glyph between, unused, unused
    float4 pulseColour;
    float4 colourA;
    float4 colourB;
    float4 colourC;
};
// Smooth escape time of z under z -> z*z + c, or -1 when it stays bounded for the whole budget.
float escape(float2 z, float2 c)
{
    float e = -1.0;
    [loop] for (int i = 0; i < %d; ++i)
    {
        z = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
        float m2 = dot(z, z);
        if (m2 > 16.0) { e = i + 1.0 - log2(log2(m2) * 0.5); break; }
    }
    return e;
}
// Unwrapped band coordinate of the left medallion at pixel p, and its radial fade (0 where undrawn).
float2 band_at(float2 p)
{
    float2 centre = medallion.xy * frameTime.xy;
    float2 rel = (p - centre) / (medallion.z * frameTime.y);
    float fade = 1.0 - smoothstep(medallion.w, 1.0, length(rel));   // radial fade on the unwarped point
    if (packing.w > 0.5)
    {
        // The blue is a hitbox. A tangential sweep along the outline (opposite directions on the two
        // sides of a ray, so the two sides no longer line up and the lines curve into the outline),
        // suppressed where the exact normal and the blurred pull disagree (narrow gaps, medial axes),
        // plus the pull towards the logo. The stop at the outline and the outline line are in main().
        float2 uv = p / frameTime.xy;
        float2 texel = 2.0 / frameTime.xy;
        float dHere = wallDistance.SampleLevel(wallSampler, uv, 0).x * wrap.w;
        float2 n = float2(wallDistance.SampleLevel(wallSampler, uv + float2(texel.x, 0.0), 0).x
                              - wallDistance.SampleLevel(wallSampler, uv - float2(texel.x, 0.0), 0).x,
                          wallDistance.SampleLevel(wallSampler, uv + float2(0.0, texel.y), 0).x
                              - wallDistance.SampleLevel(wallSampler, uv - float2(0.0, texel.y), 0).x);
        float nLen = length(n);
        n = nLen > 1e-5 ? n / nLen : float2(0.0, 0.0);
        float2 pull = wallField.SampleLevel(wallSampler, uv, 0).xy;
        float pullLen = length(pull);
        float2 away = pullLen > 1e-4 ? -pull / pullLen : n;
        float agree = smoothstep(0.3, 0.9, dot(n, away));
        float sweep = swirl.x * exp(-dHere / swirl.y) * agree;
        p += float2(-n.y, n.x) * sweep + pull * packing.z;
        rel = (p - centre) / (medallion.z * frameTime.y);
    }
    float s = sin(shape.y);
    float co = cos(shape.y);
    rel = float2(rel.x * co - rel.y * s, rel.x * s + rel.y * co);
    float e = escape(rel * shape.x, constant.xy);
    float u = e * bandsLook.x / bandsLook.y + shape.w;
    return float2(u, e < 0.0 ? 0.0 : fade);
}
// A thin line where the band coordinate crosses an integer, about linePixels wide either side.
float line_of(float u, float fw)
{
    float v = frac(u);
    float dv = min(v, 1.0 - v);
    float w = max(fw * bandsLook.w, 0.0015);
    float stroke = 1.0 - smoothstep(0.0, w, dv);
    return stroke * saturate((packing.x - fw) / packing.y);
}
float3 palette(float u)
{
    int i = (int)floor(frac(u / 3.0) * 3.0);
    return i == 0 ? colourA.rgb : (i == 1 ? colourB.rgb : colourC.rgb);
}
float4 main(PS_INPUT input) : SV_Target
{
    float2 px = input.pos.xy;
    float3 d = frame.Load(int3(px, 0)).rgb;
    float luma = dot(d, float3(0.299, 0.587, 0.114));
    float mid = frameTime.x * 0.5;
    float dist = abs(px.x - mid);
    float2 b1 = band_at(float2(mid - dist, px.y));    // the reflected point: always the left half
    float2 b2 = band_at(float2(mid + dist, px.y));    // the continuation across the centre line
    float fw1 = fwidth(b1.x);
    float fw2 = fwidth(b2.x);
    float l1 = line_of(b1.x, fw1) * b1.y;
    float l2 = line_of(b2.x, fw2) * b2.y;
    float wgt = 0.5 + 0.5 * smoothstep(0.0, shape.z, dist);
    float a = wgt * l1 + (1.0 - wgt) * l2;
    float3 col = (wgt * l1 * palette(b1.x) + (1.0 - wgt) * l2 * palette(b2.x)) / max(a, 1e-5);
    if (packing.w > 0.5)
    {
        // The hitbox: the fractal lines stop at the outer edge of the outline line, which sits wrap.x
        // px off the blue and is drawn here in pixels, fading with the medallion like everything else.
        // The path intersection is highlighted: a line brightens as it ends, and the outline brightens
        // and whitens where a line (before the stop) crosses it.
        float wallDist = wallDistance.SampleLevel(wallSampler, px / frameTime.xy, 0).x * wrap.w;
        float outer = wrap.x + wrap.y;
        float stop = smoothstep(outer - 1.0, outer + 1.0, wallDist);
        float raw = saturate(a);
        a = raw * stop * (1.0 + swirl.z * (1.0 - stop));
        float2 relHere = (float2(mid - dist, px.y) - medallion.xy * frameTime.xy) / (medallion.z * frameTime.y);
        float radial = 1.0 - smoothstep(medallion.w, 1.0, length(relHere));
        float outline = (1.0 - smoothstep(0.0, wrap.y, abs(wallDist - wrap.x))) * radial * (1.0 + swirl.z * raw);
        float3 outlineColour = lerp(colourA.rgb, colourC.rgb, raw);
        col = (a * col + outline * outlineColour) / max(a + outline, 1e-5);
        a = max(a, outline);
    }
    float keep = 1.0 - smoothstep(constant.z, constant.w, luma);
    a *= bandsLook.z * keep * frameTime.w;
    float3 outColour = lerp(d, col, saturate(a));
    // The DAWN wordmark breathes: inside the window round the wordmark strip the light glyphs (bright and low in
    // saturation; nothing else there is) are blurred into a halo that is added in the pulse colour outside them,
    // and lifted a little themselves, both scaled by this frame's pulse level and the layer's strength.
    float2 uv = px / frameTime.xy;
    if (pulse.y > 0.0 && uv.x > pulseBox.x && uv.x < pulseBox.z && uv.y > pulseBox.y && uv.y < pulseBox.w)
    {
        float here = smoothstep(pulseGlyph.x, pulseGlyph.y, min(d.r, min(d.g, d.b)));
        float halo = 0.0;
        float weight = 0.0;
        float spacing = pulse.z * 0.5;
        [loop] for (int j = -4; j <= 4; ++j)
        {
            [loop] for (int i = -4; i <= 4; ++i)
            {
                float2 o = float2(i, j) * spacing;
                float w = exp(-0.5 * dot(o, o) / (pulse.z * pulse.z));
                float3 s = frame.Load(int3(int2(px + o), 0)).rgb;
                halo += w * smoothstep(pulseGlyph.x, pulseGlyph.y, min(s.r, min(s.g, s.b)));
                weight += w;
            }
        }
        halo = halo / max(weight, 1e-5) * (1.0 - here);
        float level = pulse.x * frameTime.w;
        outColour += pulseColour.rgb * (halo * pulse.y * level) + d * (here * pulse.w * level);
    }
    return float4(saturate(outColour), 1.0);
}
)";

/** Where the layer is. */
enum class Phase : int {
    /** Before the title state, or before the boot-screen inversion has let go of the frame. */
    waiting,
    /** Drawing. */
    running,
    /** The client has left the title state: fading out. */
    ending,
    /** Over for this process. */
    done,
};

std::atomic<int> g_phase{static_cast<int>(Phase::waiting)};
std::atomic<bool> g_titleNoted{false};
std::atomic<bool> g_endRequested{false};
ID3D11BlendState* g_opaqueBlend{};
ID3D11PixelShader* g_shader{};
bool g_shaderTried{};
ID3D11Buffer* g_params{};
ID3D11Texture2D* g_frameCopy{};
ID3D11ShaderResourceView* g_frameView{};
D3D11_TEXTURE2D_DESC g_frameCopyDesc{};
/** The logo wall: the displacement field (towards the logo, in pixels) built once from the frame. */
ID3D11Texture2D* g_wallTexture{};
ID3D11ShaderResourceView* g_wallView{};
/** Distance from every pixel to the blue, full resolution, R8 in quarter-pixel steps. */
ID3D11Texture2D* g_distanceTexture{};
ID3D11ShaderResourceView* g_distanceView{};
ID3D11SamplerState* g_wallSampler{};
bool g_wallReady{};
unsigned g_wallAttempts{};
/** Frame size the field was built for; a different size rebuilds it. */
UINT g_wallWidth{};
UINT g_wallHeight{};
ULONGLONG g_startTick{};
/** Tick the filigree last became visible; the fade-in counts from here. */
ULONGLONG g_visibleTick{};
ULONGLONG g_endTick{};
unsigned g_frameCounter{};
/** The logo has been found and the filigree has started to show; the fade-in and the clock run from then. */
bool g_shown{};
/** Tick of the first bright sample of the current bright run (0: the frame is dark). */
ULONGLONG g_brightTick{};
bool g_reportedStart{};

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
                      "ev=title_filigree stage=end reason=%s result=ok",
                      reason);
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

/** Makes the plain-overwrite blend state the shader pass draws with. */
[[nodiscard]] bool create_blend(ID3D11Device* device) noexcept {
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
                                      static_cast<unsigned>(kWallSlot),
                                      static_cast<unsigned>(kDistanceSlot),
                                      static_cast<unsigned>(kWallSamplerSlot),
                                      static_cast<unsigned>(kParamsSlot),
                                      kBudget);
    if (written <= 0 || static_cast<std::size_t>(written) >= source.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=title_filigree stage=shader result=fail reason=source");
        return;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    const HRESULT compiled = D3DCompile(source.data(),
                                        static_cast<std::size_t>(written),
                                        "dawn_title_filigree",
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
                          "ev=title_filigree stage=shader result=fail reason=compile message=%.*s",
                          kCompilerMessageLimit,
                          text);
    } else if (FAILED(device->CreatePixelShader(
                   code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_shader))
               || g_shader == nullptr) {
        g_shader = nullptr;
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=title_filigree stage=shader result=fail reason=create");
    } else {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=title_filigree stage=shader result=ok");
    }
    release_com(messages);
    release_com(code);
}

/** Makes the constant buffer the parameters travel in. */
[[nodiscard]] bool ensure_params(ID3D11Device* device) noexcept {
    if (g_params != nullptr) {
        return true;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(Params);
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&description, nullptr, &g_params)) || g_params == nullptr) {
        g_params = nullptr;
        return false;
    }
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

/** Frame layouts the wall field can be read from. */
enum class WallLayout { unsupported, rgba8, bgra8 };

/** @param format Back-buffer format. @return Where red and blue sit in a texel, or unsupported. */
[[nodiscard]] WallLayout wall_layout_of(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return WallLayout::rgba8;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return WallLayout::bgra8;
    default:
        return WallLayout::unsupported;
    }
}

/** Makes the field's sampler (linear, clamped) once. */
[[nodiscard]] bool ensure_wall_sampler(ID3D11Device* device) noexcept {
    if (g_wallSampler != nullptr) {
        return true;
    }
    D3D11_SAMPLER_DESC description{};
    description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device->CreateSamplerState(&description, &g_wallSampler)) && g_wallSampler != nullptr;
}

/**
 * One-dimensional squared Euclidean distance transform (Felzenszwalb and Huttenlocher's lower envelope
 * of parabolas). @param f Squared distances in, n of them, `stride` apart. @param d Squared distances out,
 * same layout. @param v Scratch of n ints. @param z Scratch of n + 1 floats.
 */
void distance_transform_1d(const float* f, std::size_t stride, int n, float* d, int* v, float* z) noexcept {
    int k = 0;
    v[0] = 0;
    z[0] = -kFarSquared;
    z[1] = kFarSquared;
    for (int q = 1; q < n; ++q) {
        const float fq = f[static_cast<std::size_t>(q) * stride] + static_cast<float>(q) * static_cast<float>(q);
        float s = 0.0F;
        for (;;) {
            const float fv = f[static_cast<std::size_t>(v[k]) * stride] + static_cast<float>(v[k]) * static_cast<float>(v[k]);
            s = (fq - fv) / (2.0F * static_cast<float>(q - v[k]));
            if (s > z[k] || k == 0) {
                break;
            }
            --k;
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kFarSquared;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<float>(q)) {
            ++k;
        }
        const float dq = static_cast<float>(q - v[k]);
        d[static_cast<std::size_t>(q) * stride] = dq * dq + f[static_cast<std::size_t>(v[k]) * stride];
    }
}

/**
 * Builds the logo wall from the frame: reads the back buffer back once, averages its brand-blue mask
 * into cells of kWallDownsample texels, blurs the cells with a Gaussian of kWallSigma and turns the
 * gradient into the pull (pixels, towards the logo), and takes the cells' exact distance transform
 * (pixels to the nearest logo cell); both go into one RGBA32F texture.
 * @return True when the field is ready. False when the frame carries too little logo yet (the caller
 * tries again later) or the format cannot be read (no further attempts).
 */
[[nodiscard]] bool build_wall_field(ID3D11Device* device,
                                    ID3D11DeviceContext* context,
                                    ID3D11Texture2D* backBuffer,
                                    const D3D11_TEXTURE2D_DESC& shape) noexcept {
    const WallLayout layout = wall_layout_of(shape.Format);
    if (layout == WallLayout::unsupported || shape.SampleDesc.Count != kSingleSample) {
        g_wallAttempts = kWallMaxAttempts;
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=title_filigree stage=wall format=%u result=unsupported",
                          static_cast<unsigned>(shape.Format));
        return false;
    }
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = shape.Width;
    description.Height = shape.Height;
    description.MipLevels = kSingleLevel;
    description.ArraySize = kSingleLevel;
    description.Format = shape.Format;
    description.SampleDesc.Count = kSingleSample;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging)) || staging == nullptr) {
        return false;
    }
    context->CopyResource(staging, backBuffer);
    const unsigned width = (shape.Width + kWallDownsample - 1) / kWallDownsample;
    const unsigned height = (shape.Height + kWallDownsample - 1) / kWallDownsample;
    bool ready = false;
    try {
        std::vector<float> mask(static_cast<std::size_t>(width) * height);
        std::vector<std::uint8_t> full(static_cast<std::size_t>(shape.Width) * shape.Height);
        unsigned logoCells = 0;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData != nullptr) {
            const auto* rows = static_cast<const std::byte*>(mapped.pData);
            const unsigned redOffset = layout == WallLayout::rgba8 ? 0 : 2;
            const unsigned blueOffset = layout == WallLayout::rgba8 ? 2 : 0;
            const auto blueMin = static_cast<unsigned>(kWallBlueMin * kByteMax);
            const auto redMax = static_cast<unsigned>(kWallRedMax * kByteMax);
            for (unsigned y = 0; y < height; ++y) {
                for (unsigned x = 0; x < width; ++x) {
                    unsigned hits = 0;
                    unsigned count = 0;
                    for (unsigned dy = 0; dy < kWallDownsample; ++dy) {
                        const unsigned sy = y * kWallDownsample + dy;
                        if (sy >= shape.Height) {
                            break;
                        }
                        const std::byte* row = rows + static_cast<std::size_t>(sy) * mapped.RowPitch;
                        for (unsigned dx = 0; dx < kWallDownsample; ++dx) {
                            const unsigned sx = x * kWallDownsample + dx;
                            if (sx >= shape.Width) {
                                break;
                            }
                            const std::byte* texel = row + static_cast<std::size_t>(sx) * kFrameTexelBytes;
                            const auto red = static_cast<unsigned>(texel[redOffset]);
                            const auto blue = static_cast<unsigned>(texel[blueOffset]);
                            ++count;
                            const bool logo = blue > blueMin && red < redMax;
                            hits += logo ? 1U : 0U;
                            full[static_cast<std::size_t>(sy) * shape.Width + sx] = logo ? 1U : 0U;
                        }
                    }
                    const float value = count != 0 ? static_cast<float>(hits) / static_cast<float>(count) : 0.0F;
                    mask[static_cast<std::size_t>(y) * width + x] = value;
                    logoCells += value > 0.5F ? 1U : 0U;
                }
            }
            context->Unmap(staging, 0);
        } else {
            logoCells = 0;
        }
        if (logoCells >= kWallMinCells) {
            // Separable Gaussian blur of the mask, then its gradient scaled so that a straight edge pulls by
            // kWallPush of the height at the edge.
            const float sigmaFull = kWallSigma * static_cast<float>(shape.Height);
            const float sigma = sigmaFull / static_cast<float>(kWallDownsample);
            const int radius = static_cast<int>(std::ceil(3.0F * sigma));
            std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
            float total = 0.0F;
            for (int i = -radius; i <= radius; ++i) {
                const float weight = std::exp(-0.5F * static_cast<float>(i) * static_cast<float>(i) / (sigma * sigma));
                kernel[static_cast<std::size_t>(i + radius)] = weight;
                total += weight;
            }
            for (float& weight : kernel) {
                weight /= total;
            }
            std::vector<float> rowsBlurred(mask.size());
            std::vector<float> phi(mask.size());
            const int lastX = static_cast<int>(width) - 1;
            const int lastY = static_cast<int>(height) - 1;
            for (int y = 0; y <= lastY; ++y) {
                for (int x = 0; x <= lastX; ++x) {
                    float sum = 0.0F;
                    for (int i = -radius; i <= radius; ++i) {
                        const int sx = std::clamp(x + i, 0, lastX);
                        sum += kernel[static_cast<std::size_t>(i + radius)] * mask[static_cast<std::size_t>(y) * width + sx];
                    }
                    rowsBlurred[static_cast<std::size_t>(y) * width + x] = sum;
                }
            }
            for (int y = 0; y <= lastY; ++y) {
                for (int x = 0; x <= lastX; ++x) {
                    float sum = 0.0F;
                    for (int i = -radius; i <= radius; ++i) {
                        const int sy = std::clamp(y + i, 0, lastY);
                        sum += kernel[static_cast<std::size_t>(i + radius)] * rowsBlurred[static_cast<std::size_t>(sy) * width + x];
                    }
                    phi[static_cast<std::size_t>(y) * width + x] = sum;
                }
            }
            const float kappa = kWallPush * static_cast<float>(shape.Height) * sigmaFull * std::sqrt(kTwoPi);
            // Exact squared distance from every PIXEL to the nearest logo pixel: rows, then columns.
            const int fullWidth = static_cast<int>(shape.Width);
            const int fullHeight = static_cast<int>(shape.Height);
            std::vector<float> squared(full.size());
            for (std::size_t i = 0; i < full.size(); ++i) {
                squared[i] = full[i] != 0 ? 0.0F : kFarSquared;
            }
            std::vector<float> squaredRows(full.size());
            const int longest = std::max(fullWidth, fullHeight);
            std::vector<int> parabola(static_cast<std::size_t>(longest));
            std::vector<float> boundary(static_cast<std::size_t>(longest) + 1);
            for (int y = 0; y < fullHeight; ++y) {
                distance_transform_1d(squared.data() + static_cast<std::size_t>(y) * shape.Width, 1, fullWidth,
                                      squaredRows.data() + static_cast<std::size_t>(y) * shape.Width, parabola.data(), boundary.data());
            }
            for (int x = 0; x < fullWidth; ++x) {
                distance_transform_1d(squaredRows.data() + x, shape.Width, fullHeight,
                                      squared.data() + x, parabola.data(), boundary.data());
            }
            std::vector<std::uint8_t> distance(full.size());
            for (std::size_t i = 0; i < full.size(); ++i) {
                const float pixels = (std::min)(std::sqrt(squared[i]), kDistanceRangePixels);
                distance[i] = static_cast<std::uint8_t>(std::lround(pixels / kDistanceRangePixels * kByteMax));
            }
            std::vector<float> field(mask.size() * kFieldChannels);
            for (int y = 0; y <= lastY; ++y) {
                for (int x = 0; x <= lastX; ++x) {
                    const int x0 = std::max(x - 1, 0);
                    const int x1 = std::min(x + 1, lastX);
                    const int y0 = std::max(y - 1, 0);
                    const int y1 = std::min(y + 1, lastY);
                    const float spanX = static_cast<float>((x1 - x0) * static_cast<int>(kWallDownsample));
                    const float spanY = static_cast<float>((y1 - y0) * static_cast<int>(kWallDownsample));
                    const float gradientX = spanX > 0.0F ? (phi[static_cast<std::size_t>(y) * width + x1] - phi[static_cast<std::size_t>(y) * width + x0]) / spanX : 0.0F;
                    const float gradientY = spanY > 0.0F ? (phi[static_cast<std::size_t>(y1) * width + x] - phi[static_cast<std::size_t>(y0) * width + x]) / spanY : 0.0F;
                    const std::size_t cell = static_cast<std::size_t>(y) * width + x;
                    field[cell * kFieldChannels] = gradientX * kappa;
                    field[cell * kFieldChannels + 1] = gradientY * kappa;
                }
            }
            release_com(g_wallView);
            release_com(g_wallTexture);
            release_com(g_distanceView);
            release_com(g_distanceTexture);
            D3D11_TEXTURE2D_DESC fieldDescription{};
            fieldDescription.Width = width;
            fieldDescription.Height = height;
            fieldDescription.MipLevels = kSingleLevel;
            fieldDescription.ArraySize = kSingleLevel;
            fieldDescription.Format = DXGI_FORMAT_R32G32_FLOAT;
            fieldDescription.SampleDesc.Count = kSingleSample;
            fieldDescription.Usage = D3D11_USAGE_IMMUTABLE;
            fieldDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = field.data();
            data.SysMemPitch = width * static_cast<UINT>(sizeof(float) * kFieldChannels);
            D3D11_TEXTURE2D_DESC distanceDescription{};
            distanceDescription.Width = shape.Width;
            distanceDescription.Height = shape.Height;
            distanceDescription.MipLevels = kSingleLevel;
            distanceDescription.ArraySize = kSingleLevel;
            distanceDescription.Format = DXGI_FORMAT_R8_UNORM;
            distanceDescription.SampleDesc.Count = kSingleSample;
            distanceDescription.Usage = D3D11_USAGE_IMMUTABLE;
            distanceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA distanceData{};
            distanceData.pSysMem = distance.data();
            distanceData.SysMemPitch = shape.Width;
            if (SUCCEEDED(device->CreateTexture2D(&fieldDescription, &data, &g_wallTexture)) && g_wallTexture != nullptr
                && SUCCEEDED(device->CreateShaderResourceView(g_wallTexture, nullptr, &g_wallView)) && g_wallView != nullptr
                && SUCCEEDED(device->CreateTexture2D(&distanceDescription, &distanceData, &g_distanceTexture))
                && g_distanceTexture != nullptr
                && SUCCEEDED(device->CreateShaderResourceView(g_distanceTexture, nullptr, &g_distanceView))
                && g_distanceView != nullptr) {
                ready = true;
                g_wallWidth = shape.Width;
                g_wallHeight = shape.Height;
            } else {
                release_com(g_distanceView);
                release_com(g_distanceTexture);
                release_com(g_wallView);
                release_com(g_wallTexture);
            }
        }
        core::log::writef(core::log::Channel::client,
                          core::log::Level::info,
                          "ev=title_filigree stage=wall cells=%u grid=%ux%u distance=full sigma=%.1f push=%.1f attempt=%u result=%s",
                          logoCells,
                          width,
                          height,
                          static_cast<double>(kWallSigma * static_cast<float>(shape.Height)),
                          static_cast<double>(kWallPush * static_cast<float>(shape.Height)),
                          g_wallAttempts,
                          ready ? "ok" : (logoCells >= kWallMinCells ? "fail" : "retry"));
    } catch (...) {
        ready = false;
    }
    release_com(staging);
    return ready;
}

/** Draw callback: binds the shader, the frame copy, the wall field, the parameters and a plain overwrite. */
void set_shader_state(const ImDrawList* /*parent*/, const ImDrawCmd* /*command*/) noexcept {
    auto* state =
        static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (state == nullptr || state->DeviceContext == nullptr || g_shader == nullptr
        || g_frameView == nullptr || g_params == nullptr || g_opaqueBlend == nullptr) {
        return;
    }
    ID3D11DeviceContext* context = state->DeviceContext;
    context->PSSetShader(g_shader, nullptr, 0);
    ID3D11ShaderResourceView* views[]{g_frameView, g_wallReady ? g_wallView : nullptr, g_wallReady ? g_distanceView : nullptr};
    static_assert(kWallSlot == kFrameSlot + 1 && kDistanceSlot == kFrameSlot + 2,
                  "the frame copy, the pull field and the distance are bound as one range");
    context->PSSetShaderResources(kFrameSlot, 3, views);
    ID3D11SamplerState* samplers[]{g_wallSampler};
    context->PSSetSamplers(kWallSamplerSlot, 1, samplers);
    ID3D11Buffer* buffers[]{g_params};
    context->PSSetConstantBuffers(kParamsSlot, 1, buffers);
    context->OMSetBlendState(g_opaqueBlend, nullptr, kAllSamples);
}

/** Draw callback: drops the bindings the reset that follows does not touch. */
void clear_shader_state(const ImDrawList* /*parent*/, const ImDrawCmd* /*command*/) noexcept {
    auto* state =
        static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (state == nullptr || state->DeviceContext == nullptr) {
        return;
    }
    ID3D11ShaderResourceView* views[]{nullptr, nullptr, nullptr};
    state->DeviceContext->PSSetShaderResources(kFrameSlot, 3, views);
    ID3D11SamplerState* samplers[]{nullptr};
    state->DeviceContext->PSSetSamplers(kWallSamplerSlot, 1, samplers);
    ID3D11Buffer* buffers[]{nullptr};
    state->DeviceContext->PSSetConstantBuffers(kParamsSlot, 1, buffers);
}

/** Adds the shader quad over the whole viewport. */
void draw_shader_pass(const ImGuiViewport& viewport) noexcept {
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddCallback(&set_shader_state, nullptr);
    background->AddRectFilled(viewport.Pos,
                              {viewport.Pos.x + viewport.Size.x, viewport.Pos.y + viewport.Size.y},
                              IM_COL32_WHITE);
    background->AddCallback(&clear_shader_state, nullptr);
    background->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

/** @return This frame's strength (fade in, fade out), or a negative value once the fade-out is over. */
[[nodiscard]] float strength_at(ULONGLONG now) noexcept {
    float strength = (std::min)(1.0F,
                                static_cast<float>(now - g_visibleTick) / static_cast<float>(kFadeInMs));
    if (phase() == Phase::ending) {
        const ULONGLONG fading = now - g_endTick;
        if (fading >= kFadeOutMs) {
            return -1.0F;
        }
        strength *= 1.0F - static_cast<float>(fading) / static_cast<float>(kFadeOutMs);
    }
    return strength;
}

/** @return The constant buffer contents for this frame. */
[[nodiscard]] Params params_at(const D3D11_TEXTURE2D_DESC& shape, float seconds, float strength, bool wall) noexcept {
    const float loop = kTwoPi * seconds / kMorphPeriodS;
    Params params{};
    params.width = static_cast<float>(shape.Width);
    params.height = static_cast<float>(shape.Height);
    params.time = seconds;
    params.strength = strength;
    params.centreX = kCentreX;
    params.centreY = kCentreY;
    params.radius = kRadius;
    params.fadeStart = kFadeStart;
    params.span = kSpan;
    params.spinRadians = kSpinDegPerSec * kDegreesToRadians * seconds;
    params.blendPixels = kBlendPixels;
    params.crawlPhase = kCrawlBandsPerSec * seconds;
    params.constantReal = kConstantReal + kLoopReal * std::cos(loop);
    params.constantImag = kConstantImag + kLoopImag * std::sin(loop);
    params.keepLow = kKeepLow;
    params.keepHigh = kKeepHigh;
    params.bands = kBands;
    params.bandPeriod = kBandPeriod;
    params.lineAlpha = kLineAlpha;
    params.linePixels = kLinePixels;
    params.packStart = kPackStart;
    params.packWidth = kPackWidth;
    params.wallPull = kWallPull;
    params.wallOn = wall ? 1.0F : 0.0F;
    params.outlineCentre = kWrapGapPixels + kLinePixels;
    params.outlineHalfWidth = kLinePixels;
    params.distanceRange = kDistanceRangePixels;
    params.swirlPixels = kSwirlPixels;
    params.swirlRange = kSwirlRangePixels;
    params.junctionGain = kJunctionGain;
    // The wordmark's breath: a raised cosine, so it is smooth at both ends.
    params.pulseLevel = 0.5F - 0.5F * std::cos(kTwoPi * seconds / kPulsePeriodS);
    params.pulseGain = kPulseGain;
    params.pulseSigma = kPulseSigmaPixels;
    params.pulseCoreGain = kPulseCoreGain;
    params.pulseLeft = kPulseLeft;
    params.pulseTop = kPulseTop;
    params.pulseRight = kPulseRight;
    params.pulseBottom = kPulseBottom;
    params.pulseGlyphLow = kPulseGlyphLow;
    params.pulseGlyphHigh = kPulseGlyphHigh;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        params.pulseColour[channel] = kPulseColour[channel];
        params.colourA[channel] = kColourA[channel];
        params.colourB[channel] = kColourB[channel];
        params.colourC[channel] = kColourC[channel];
    }
    params.pulseColour[3] = params.colourA[3] = params.colourB[3] = params.colourC[3] = 1.0F;
    return params;
}

} // namespace

/** Adds the title-screen filigree to the current frame. */
bool draw(IDXGISwapChain* swapChain,
          ID3D11Device* device,
          ID3D11DeviceContext* context,
          bool inversionActing,
          bool inversionCovering) noexcept {
    if (phase() == Phase::done) {
        return false;
    }
    if (swapChain == nullptr || device == nullptr || context == nullptr) {
        return false;
    }
    if (!core::settings::get().client.titleFiligree) {
        set_phase(Phase::done);
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=title_filigree stage=setting enabled=0 result=skip");
        return false;
    }
    if (!g_titleNoted.load(std::memory_order_acquire) || !splash_invert::released()) {
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (phase() == Phase::waiting) {
        set_phase(Phase::running);
        g_startTick = now;
        g_visibleTick = now;
    }
    if (g_endRequested.load(std::memory_order_acquire) && phase() == Phase::running) {
        set_phase(Phase::ending);
        g_endTick = now;
    }
    const float strength = strength_at(now);
    if (strength < 0.0F) {
        finish("state");
        return false;
    }
    if (inversionCovering) {
        // The inversion layer only inverts a bright screen after the title (the loading screen behind Enter;
        // the title is never bright), and this layer's opaque quad would overwrite its output with the raw
        // frame: the filigree is over at once.
        finish("inversion");
        return false;
    }
    if (inversionActing) {
        // A crossfade into or out of that screen, inverted a little or dimmed: the same overwrite, so this
        // frame is left to the inversion (the title itself is never touched by it).
        return false;
    }
    if (g_opaqueBlend == nullptr && !create_blend(device)) {
        finish("blend_state");
        return false;
    }
    create_shader(device);
    if (g_shader == nullptr || !ensure_params(device)) {
        finish("shader");
        return false;
    }
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
    bool drawn = false;
    if (g_shown && g_frameCounter++ % kSampleEveryFrames == 0) {
        // Now and then the frame under the layer is read: the title is never bright, so a bright frame is
        // the loading screen behind Enter. The filigree hides at once (its pulse halo would otherwise find
        // "glyphs" all over a bright frame) and ends once the brightness has lasted kBrightEndMs, so one
        // stray bright frame does not cost the title its filigree; the state line is the backstop.
        float mean = 0.0F;
        if (splash_invert::sample_mean(device, context, backBuffer, shape, mean)) {
            if (mean > kBrightMean) {
                if (g_brightTick == 0) {
                    g_brightTick = now;
                    core::log::writef(core::log::Channel::client,
                                      core::log::Level::info,
                                      "ev=title_filigree stage=hide mean=%.3f result=ok",
                                      static_cast<double>(mean));
                } else if (now - g_brightTick >= kBrightEndMs) {
                    finish("bright");
                }
            } else if (g_brightTick != 0) {
                g_brightTick = 0;
                core::log::writef(core::log::Channel::client,
                                  core::log::Level::info,
                                  "ev=title_filigree stage=show mean=%.3f result=ok",
                                  static_cast<double>(mean));
            }
        }
    }
    if (!g_shown) {
        ++g_frameCounter;
    }
    if (g_brightTick != 0 || phase() == Phase::done) {
        backBuffer->Release();
        return false;
    }
    if (shape.SampleDesc.Count == kSingleSample && ensure_frame_copy(device, shape)) {
        if (!g_reportedStart) {
            g_reportedStart = true;
            core::log::writef(core::log::Channel::client,
                              core::log::Level::info,
                              "ev=title_filigree stage=start width=%u height=%u format=%u result=ok",
                              static_cast<unsigned>(shape.Width),
                              static_cast<unsigned>(shape.Height),
                              static_cast<unsigned>(shape.Format));
        }
        if (g_wallReady && (g_wallWidth != shape.Width || g_wallHeight != shape.Height)) {
            // A new frame size: the field is in pixels of the old one, so it is built again.
            g_wallReady = false;
            g_wallAttempts = 0;
        }
        if (kWall && !g_wallReady && g_wallAttempts < kWallMaxAttempts
            && (g_wallAttempts == 0 || g_frameCounter % kWallRetryFrames == 0)) {
            ++g_wallAttempts;
            g_wallReady = ensure_wall_sampler(device) && build_wall_field(device, context, backBuffer, shape);
        }
        if (kWall && !g_wallReady) {
            // The logo is not on the frame yet (the dark Bungie splash): nothing is drawn.
            backBuffer->Release();
            return false;
        }
        if (!g_shown) {
            // The title screen is here: the fade-in and the clock start now.
            g_shown = true;
            g_startTick = now;
            g_visibleTick = now;
        }
        const float seconds = static_cast<float>(now - g_startTick) / kMillisecondsPerSecond;
        const Params params = params_at(shape, seconds, strength, kWall && g_wallReady);
        context->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
        context->CopyResource(g_frameCopy, backBuffer);
        draw_shader_pass(*viewport);
        drawn = true;
    }
    backBuffer->Release();
    return drawn;
}

/** Frees the layer's device objects. The phase survives, so a device change carries on. */
void release() noexcept {
    release_com(g_distanceView);
    release_com(g_distanceTexture);
    release_com(g_wallView);
    release_com(g_wallTexture);
    release_com(g_wallSampler);
    g_wallReady = false;
    g_wallAttempts = 0;
    release_com(g_frameView);
    release_com(g_frameCopy);
    g_frameCopyDesc = {};
    release_com(g_params);
    release_com(g_shader);
    g_shaderTried = false;
    release_com(g_opaqueBlend);
}

/** Records that the client entered the title-screen state. */
void note_title_screen() noexcept {
    g_titleNoted.store(true, std::memory_order_release);
}

/** Records a world-controller state: the title, a sign-in state under it (ignored), or the title over. */
void note_state(std::string_view name) noexcept {
    if (name == "bootflow:start") {
        note_title_screen();
    } else if (!name.starts_with("bootflow:") && g_titleNoted.load(std::memory_order_acquire)) {
        // The sign-in states run under the title screen; the first state outside the boot flow
        // (character select) is the title screen over for good.
        g_endRequested.store(true, std::memory_order_release);
    }
}

} // namespace dawn::client::hooks::graphics::renderer::title_filigree
