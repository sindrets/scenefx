exclude_files = ['meson.build']

# Internal config header. The renderers are gated on SCENEFX_HAS_* rather than on
# wlroots' WLR_HAS_*: those describe what wlroots was built with, not what scenefx
# actually compiled. They coincide by default, but diverge whenever scenefx skips
# a renderer that wlroots has (for example a wlroots without the Vulkan FX entry
# points), and then fx_renderer.c would reference a renderer that was never built.
conf_data = configuration_data()
conf_data.set10('SCENEFX_HAS_GLES2_RENDERER', features.get('gles2-renderer'))
conf_data.set10('SCENEFX_HAS_VULKAN_RENDERER', features.get('vulkan-renderer'))
configure_file(
	output: 'scenefx-config.h',
	configuration: conf_data,
)

install_subdir('scenefx',
	install_dir: get_option('includedir') / versioned_name,
	exclude_files: exclude_files,
)
