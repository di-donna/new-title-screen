#pragma once

#include <string_view>

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace dawn::client::hooks::graphics::renderer::title_filigree {

/**
 * Adds the title-screen filigree (two mirrored, morphing Julia medallions) to the current Dear
 * ImGui frame while the title screen is up. Presentation thread, inside the active frame, under
 * the renderer lock.
 * @param swapChain Chosen swap chain, whose back buffer is copied and keyed.
 * @param device Device that owns the back buffer; creates the layer's objects on first use.
 * @param context Immediate context of that device.
 * @param inversionActing The boot-screen inversion inverted or dimmed this frame (a bright screen
 * after the title, or a crossfade round it, whose quad this layer's would overwrite): nothing is drawn.
 * @param inversionCovering It inverted a bright screen outright: the filigree is over.
 * @return True when draw data was added.
 */
[[nodiscard]] bool draw(IDXGISwapChain* swapChain,
                        ID3D11Device* device,
                        ID3D11DeviceContext* context,
                        bool inversionActing,
                        bool inversionCovering) noexcept;

/** Frees the layer's device objects. Presentation thread, before the device is released. */
void release() noexcept;

/** Records that the client entered the title-screen state. Safe from any thread. */
void note_title_screen() noexcept;

/**
 * Records a world-controller state the client entered: "bootflow:start" is the title screen, the
 * sign-in states under it ("bootflow:...") are ignored, anything else is the title screen over.
 * Safe from any thread.
 * @param name The state name, e.g. "bootflow:start".
 */
void note_state(std::string_view name) noexcept;

} // namespace dawn::client::hooks::graphics::renderer::title_filigree
