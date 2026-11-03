project(
	'scenefx',
	'c',
	version: '0.5.0',
	license: 'MIT',
	meson_version: '>=1.3',
	default_options: [
		'c_std=' + (meson.version().version_compare('>=1.4.0') ? 'c23,c11' : 'c11'),
		'warning_level=2',
		'werror=true',
		'wrap_mode=nodownload',
	],
)

version = meson.project_version().split('-')[0]
version_major = version.split('.')[0]
version_minor = version.split('.')[1]
versioned_name = '@0@-@1@.@2@'.format(meson.project_name(), version_major, version_minor)

little_endian = target_machine.endian() == 'little'
big_endian = target_machine.endian() == 'big'

add_project_arguments([
	'-D_POSIX_C_SOURCE=200809L',
	'-DWLR_USE_UNSTABLE',
	'-DWLR_PRIVATE=',
	'-DWLR_LITTLE_ENDIAN=@0@'.format(little_endian.to_int()),
	'-DWLR_BIG_ENDIAN=@0@'.format(big_endian.to_int()),
], language: 'c')

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
	'-Wundef',
	'-Wlogical-op',
	'-Wmissing-include-dirs',
	'-Wold-style-definition',
	'-Wpointer-arith',
	'-Winit-self',
	'-Wstrict-prototypes',
	'-Wimplicit-fallthrough=2',
	'-Wendif-labels',
	'-Wstrict-aliasing=2',
	'-Woverflow',
	'-Wmissing-prototypes',
	'-Walloca',

	'-Wno-missing-braces',
	'-Wno-missing-field-initializers',
	'-Wno-unused-parameter',
]), language: 'c')

fs = import('fs')

# Strip relative path prefixes from the code if possible, otherwise hide them.
relative_dir = fs.relative_to(meson.current_source_dir(), meson.global_build_root()) + '/'
if cc.has_argument('-fmacro-prefix-map=/prefix/to/hide=')
	add_project_arguments(
		'-fmacro-prefix-map=@0@='.format(relative_dir),
		language: 'c',
	)
else
	add_project_arguments(
		'-D_WLR_REL_SRC_DIR="@0@"'.format(relative_dir),
		language: 'c',
	)
endif

features = {
	'gles2-renderer': false,
	'vulkan-renderer': false,
	'color-management': false,
}
internal_features = {
	'egl': false,
	'tracy': false,
}

wayland_kwargs = {
	'version': '>=1.24.0',
	'fallback': 'wayland',
	'default_options': [
		'tests=false',
		'documentation=false',
	],
}
wayland_server = dependency('wayland-server',
	kwargs: wayland_kwargs,
)

wlroots_options = [ 'examples=false' ]
wlroots_version = ['>=0.20.0', '<0.21.0']
# The Vulkan FX renderer needs wlroots entry points that upstream does not
# export - wlr_vk_render_pass_suspend/resume() and the blend-image getters, which
# let a consumer render into the frame in progress.
#
# Upstream wlroots is still accepted (the GLES2 renderer works with it), but
# whether it has the extra API decides if the Vulkan renderer can be built.
# Check dependency compatibility by linking against the entry points we need.
wlroots_has_fx_api_code = '''
#define WLR_USE_UNSTABLE
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/pass.h>
#include <wlr/render/vulkan.h>
int main(void) {
	void *p = (void*)wlr_vk_render_pass_get_blend_image;
	return p == 0;
}
'''

wlroots = dependency('wlroots-vkfx-0.20',
	version: wlroots_version,
	default_options: wlroots_options,
	required: false,
)
upstream_wlroots = dependency('wlroots-0.20',
	version: wlroots_version,
	default_options: wlroots_options,
	required: false,
)

if wlroots.found()
	wlroots_has_fx_api = true
elif upstream_wlroots.found()
	wlroots = upstream_wlroots
	wlroots_has_fx_api = cc.links(wlroots_has_fx_api_code,
		dependencies: wlroots,
		name: 'wlroots Vulkan FX entry points',
	)
else
	wlroots_proj = subproject(
		'wlroots',
		default_options: wlroots_options,
		version: wlroots_version,
	)
	wlroots = wlroots_proj.get_variable('wlroots')
	wlroots_has_fx_api = cc.links(wlroots_has_fx_api_code,
		dependencies: wlroots,
		name: 'wlroots Vulkan FX entry points',
	)
endif

# wlroots publishes these feature flags only in the .pc file it installs; the
# -uninstalled.pc generated for a build tree carries no variables at all. A
# missing flag therefore means "cannot tell", and defaults to yes: pointing
# PKG_CONFIG_PATH at a wlroots build tree used to disable both renderers and then
# fail somewhere unrelated in render/meson.build. An installed wlroots always has
# the flags, so a genuine "not built" is still honoured.
#
# The Vulkan renderer additionally needs the fork's FX entry points, whatever the
# flags say: a wlroots without them compiles the GLES2 renderer happily but not
# render/vulkan, so gate it here rather than at link time.
renderers = {
	'gles2': wlroots.get_variable('have_gles2_renderer',
		default_value: 'true') == 'true',
	'vulkan': wlroots_has_fx_api and
		wlroots.get_variable('have_vulkan_renderer',
			default_value: 'true') == 'true',
}

if not wlroots_has_fx_api and not get_option('auto_features').disabled()
	warning('wlroots does not export the Vulkan FX entry points, so the Vulkan ' +
		'renderer will not be built. Install the fork from ' +
		'https://github.com/sindrets/wlroots-vkfx (pkg-config name ' +
		'wlroots-vkfx-0.20) and point PKG_CONFIG_PATH at it to enable it.')
endif

drm = dependency('libdrm',
	version: '>=2.4.129',
	fallback: 'libdrm',
	default_options: [
		'auto_features=disabled',
		'tests=false',
	],
)
xkbcommon = dependency('xkbcommon',
	version: '>=1.8.0',
	fallback: 'libxkbcommon',
	default_options: [
		'enable-tools=false',
		'enable-x11=false',
		'enable-docs=false',
		'enable-xkbregistry=false',
	],
)
pixman = dependency('pixman-1',
	version: '>=0.43.0',
	fallback: 'pixman',
	default_options: ['werror=false'],
)
math = cc.find_library('m')
rt = cc.find_library('rt')

scenefx_files = []
scenefx_deps = [
	wlroots,
	wayland_server,
	drm,
	xkbcommon,
	pixman,
	math,
	rt,
]

# Tracy
if get_option('tracy_enable')
	internal_features += { 'tracy': true }

	if get_option('buildtype') != 'debugoptimized'
		warning('Profiling builds should set --buildtype=debugoptimized')
	endif

	tracy_options = [
		'default_library=static',
		'werror=false',
		'warning_level=0',
	]
	tracy_proj = subproject(
		'tracy',
		required: true,
		default_options: tracy_options
	)
	scenefx_deps += tracy_proj.get_variable('tracy_dep')
	add_project_arguments(
		[
			'-DTRACY_ENABLE',
			'-DTRACY_ON_DEMAND',
		],
		language: 'c',
	)
endif

subdir('protocol')
subdir('render')

subdir('types')
subdir('util')

subdir('include')

# '.' picks up the build directory as well as the source directory, so the
# generated scenefx-config.h is reachable as "scenefx-config.h".
scenefx_inc = include_directories('.', 'include')

lib_scenefx = library(
	versioned_name, scenefx_files,
	version: version,
	soversion: version_major,
	dependencies: scenefx_deps,
	include_directories:  [ scenefx_inc ],
	install: true,
)


scenefx = declare_dependency(
	link_with: lib_scenefx,
	dependencies: scenefx_deps,
	include_directories: scenefx_inc,
)

meson.override_dependency(versioned_name, scenefx)

summary(features + internal_features, bool_yn: true)

if get_option('examples')
	subdir('examples')
	subdir('tinywl')
endif

pkgconfig = import('pkgconfig')
pkgconfig.generate(
	lib_scenefx,
	name: versioned_name,
	version: meson.project_version(),
	description: 'Wlroots effects library',
	subdirs: versioned_name,
)
