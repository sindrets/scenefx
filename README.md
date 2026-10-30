# scenefx

> [!NOTE]
> **What is this fork?**: This fork aims to provide a Vulkan renderer with full
> parity with the GLES2 renderer (with some differences. See details under
> [_Renderer Support_](#renderer-support)).
>
> The Vulkan renderer needs the wlroots fork at
> <https://github.com/sindrets/wlroots-vkfx>; the GLES2 one builds against
> upstream wlroots. See [_Usage_](#usage).
>
> This builds on top of the work done in
> <https://github.com/wlrfx/scenefx/tree/vulkan>.
>
> And adapts work done in
> <https://github.com/wlrfx/scenefx/pull/204> and its companion wlroots branch
> <https://github.com/spoloxs/wlroots-vkfx/tree/vulkan-effects>, itself based on
> <https://gitlab.freedesktop.org/ammen99/wlroots/-/tree/vulkan-effects>.

wlroots is the de-facto library for building wayland compositors, and its scene api has vastly simplified compositor development. However, for developers wanting modern aesthetic flair, the Scene API forces the use of the standard wlr_renderer, which is intentionally kept minimal and simple. SceneFX bridges this gap by serving as a drop-in replacement for the Scene API. It swaps out the default renderer for our custom FX renderer, allowing compositors to easily render eye-candy effects like blur, shadows, and rounded corners while maintaining the simplicity of the upstream Scene API.

## Compositors Using SceneFX
Plenty of popular wayland compositors are using SceneFX to render eyecandy, including:
- [SwayFX](https://github.com/WillPower3309/swayfx)
- [Mango](https://github.com/mangowm/mango)
- [Umbriel](https://github.com/noctalia-dev/umbriel)
- [nauka](https://github.com/shadowash8/nauka)
- [mwc](https://github.com/nikoloc/mwc)
- dwl [with a patch](https://codeberg.org/dwl/dwl-patches/src/branch/main/stale-patches/scenefx)
- labwc [with a patch](https://github.com/grigio/labwc/tree/ext-background-effect) ([upstream PR labwc#3751](https://github.com/labwc/labwc/pull/3751#issuecomment-6046975930), distributed as [labwc-blur](https://github.com/grigio/labwc/releases))

## Installation
<a href="https://repology.org/project/scenefx/versions"><img src="https://repology.org/badge/vertical-allrepos/scenefx.svg"/></a>

Scenefx is also available in <a href="https://copr.fedorainfracloud.org/coprs/damiand/scenefx/">copr</a> repositories. Enable via `dnf copr enable damiand/scenefx` and install via `dnf install scenefx scenefx-devel`

## Compiling From Source
Install dependencies:
* meson \*
* wlroots
* wayland
* wayland-protocols \*
* EGL and GLESv2
* libdrm
* pixman

_\* Compile-time dep_

Run these commands:
```sh
meson setup build/
ninja -C build/
```

Install like so:
```sh
sudo ninja -C build/ install
```

## Usage

Just replace your calls to `wlr_renderer_autocreate()` with `scenefx_init()`
providing your wlr_scene and wlr_backend structs and replace all
`wlr/types/wlr_scene.h` includes with `scenefx/types/wlr_scene.h`.

SceneFX picks an FX renderer matching whatever `wlr_renderer` it is given, so
the usual `WLR_RENDERER` selection applies: `gles2` (the default) or `vulkan`.

The Vulkan renderer additionally requires
<https://github.com/sindrets/wlroots-vkfx>: upstream's Vulkan renderer has no
way to let a consumer sample the frame drawn so far, which blur needs. That
fork installs under the `wlroots-vkfx-<version>` pkg-config name, which is the
name this project looks for, so build and install it first (or point
`PKG_CONFIG_PATH` at its `build/meson-uninstalled` directory) and it is used
automatically. The GLES2 renderer is unaffected and builds against upstream
wlroots.

## Renderer Support

Both FX renderers are feature-complete for the scene API. The Vulkan one is
newer, so the differences below are deliberate or still open:

| Feature                         | GLES2   | Vulkan               |
| ------------------------------- | ------- | -------------------- |
| Rects, textures, clipping       | yes     | yes                  |
| Rounded corners                 | yes     | yes                  |
| Linear and radial gradients     | yes     | yes                  |
| Box shadows                     | yes     | yes                  |
| Blur (dual-Kawase)              | yes     | yes                  |
| Blur transparency mask          | stencil | sampled mask texture |
| Blur padding save/restore       | yes     | yes                  |
| Optimized blur nodes            | yes     | yes                  |
| Explicit sync (`wait_timeline`) | yes     | yes                  |
| Device-lost propagation         | yes     | yes                  |
| Tracy GPU zones                 | yes     | yes                  |
| `wlr_render_timer`              | yes     | **not yet**          |

Two things are worth knowing before switching a compositor over:

* **Blur is computed in linear light on Vulkan.** GLES2 averages already-encoded
  sRGB bytes, which is the classic gamma-space blur error and makes blurred
  content too dark; Vulkan blurs the 16-bit float blending image instead. The
  output is therefore *correct* rather than identical - high-contrast content
  differs by roughly 8-11% while solid areas match exactly. Sharp (unblurred)
  content is pixel-identical between the two renderers.
* **Optimized blur falls back to a live blur at reduced strength.**
  `wlr_scene_optimized_blur_create()` nodes fill a per-output cache without
  drawing anything, and nodes marked with
  `wlr_scene_blur_set_should_only_blur_bottom_layer()` sample that cache instead
  of blurring again, matching GLES2. The one difference is a blur node with a
  strength below 1.0: GLES2 re-blurs an unblurred copy kept alongside the cache,
  which Vulkan does not maintain, so it blurs the live frame instead. The result
  is visually equivalent, just not identical.

`wlr_render_timer` requires wlroots work first: its Vulkan renderer has no
`render_timer_create` at all, so the call returns `NULL` regardless of SceneFX.

## Troubleshooting

### Using scenefx features breaks the compositor

This issue might be caused by compiling scenefx and wlroots with
Clang compiler and thin LTO(`-flto=thin`) option enabled. Try to
compile the libraries without LTO optimizations or with GCC compiler instead.

## Debugging

SceneFX includes the same debugging tools and environment variables as upstream wlroots does, but with some extra goodies.

### Environment variables:

- `WLR_RENDERER_ALLOW_SOFTWARE=1`: Use software rendering
- `WLR_SCENE_DEBUG_DAMAGE=rerender`: Re-render the whole display on each commit (don't use damage)
- `WLR_SCENE_DEBUG_DAMAGE=highlight`: Highlights where damage has occurred (where SwayFX get's re-rendered)
- `WLR_SCENE_DISABLE_DIRECT_SCANOUT=1`: Disable direct scanout (always composites, even fullscreen windows)
- `WLR_SCENE_DISABLE_VISIBILITY=1`: Disables culling of non-visible regions of a window/buffer (an example would be a small window fully covered by an opaque window)
- `WLR_SCENE_HIGHLIGHT_TRANSPARENT_REGION=1`: Highlights the transparent areas of a window/buffer
- `WLR_EGL_NO_MODIFIERS=1`: Disables modifiers for EGL

### Tracy profiling

Optional [Tracy](https://github.com/wolfpld/tracy) profiling can be enabled for an extra good view of when and what is happening.

#### Enabling:

Note: These instructions will enable basic profiling

1. Add SceneFX as a subproject to your compositor
2. Import it as a subproject dependency
3. Run `meson subprojects download` to download the tracy project into the subproject directory
4. Compile SceneFX with `-Dtracy_enable=true` (and `--buildtype=debugoptimized` if using meson).
5. Start your compositor
6. Start the `tracy-profiler` and connect to the running compositor (The version of the profiler should not matter, but if any issues are encountered, try using a version that matches the subproject).

To enable more advanced profiling, the compositor in question needs to be run by the root user which comes with its own drawbacks, like DBus not working out of the box. A recommended way of doing this is by running your compositor with the `-E` sudo flag, such as `sudo -E sway`.

To enable DBus, you need to give the root user access to the DBus session bus by adding the following lines to the `/etc/dbus-1/session-local.conf` file (you might have to create said file), and reboot your system.

```xml
<!-- /etc/dbus-1/session-local.conf -->
<!-- Allow root to access session bus -->
<busconfig>
  <policy context="mandatory">
    <allow user="root"/>
  </policy>
</busconfig>
```

#### Additional tracy documentation

The links below are helpful when learning how to use tracy:

- https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
- https://www.youtube.com/watch?v=ghXk3Bk5F2U

---
[Join our Discord](https://discord.gg/qsSx397rkh)

[Check out the wiki](https://wlrfx.github.io/wiki/data/scenefx/index.html)
