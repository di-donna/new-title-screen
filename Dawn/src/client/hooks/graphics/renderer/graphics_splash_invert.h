#pragma once

#include <string_view>

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct D3D11_TEXTURE2D_DESC;

namespace dawn::client::hooks::graphics::renderer::splash_invert {

/**
 * Adds the boot-screen inversion to the current Dear ImGui frame when a bright boot screen is
 * on the back buffer. Presentation thread, inside the active frame, under the renderer lock.
 * @param swapChain Chosen swap chain, whose back buffer is sampled.
 * @param device Device that owns the back buffer; creates the layer's objects on first use.
 * @param context Immediate context of that device.
 * @return True when draw data was added.
 */
[[nodiscard]] bool
draw(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context) noexcept;

/** Frees the layer's device objects. Presentation thread, before the device is released. */
void release() noexcept;

/** Records that the client entered the title-screen state. Safe from any thread. */
void note_title_screen() noexcept;

/**
 * Records a world-controller state the client entered: "bootflow:start" is the title screen, the
 * sign-in states under it ("bootflow:...") and character select ("character:...") keep the layer
 * armed for the bright loading screen between them, anything else (the world loading) ends it.
 * Safe from any thread.
 * @param name The state name, e.g. "bootflow:start".
 */
void note_state(std::string_view name) noexcept;

/** @return True once the layer has let go of the title screen (armed for the menus, or over). */
[[nodiscard]] bool released() noexcept;

/** @return True when the frame just drawn was a bright screen the layer inverted (not a dim-only touch). */
[[nodiscard]] bool covering() noexcept;

/** @return True when the layer changed the brightness of the frame just drawn (inverted or dimmed it). */
[[nodiscard]] bool acting() noexcept;

/**
 * Reads the layer's texel grid off a back buffer and returns its mean brightness. Presentation
 * thread, under the renderer lock; used by the title filigree's bright-frame guard.
 * @return False when the back buffer cannot be sampled.
 */
[[nodiscard]] bool sample_mean(ID3D11Device* device,
                               ID3D11DeviceContext* context,
                               ID3D11Texture2D* backBuffer,
                               const D3D11_TEXTURE2D_DESC& shape,
                               float& mean) noexcept;

} // namespace dawn::client::hooks::graphics::renderer::splash_invert
