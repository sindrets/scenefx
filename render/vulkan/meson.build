dep_vulkan = dependency('vulkan',
	version: '>=1.2.182',
	required: true,
)

# Vulkan headers are installed separately from the loader (which ships the
# pkg-config file)
if not cc.check_header('vulkan/vulkan.h', dependencies: dep_vulkan)
	error('vulkan headers are required for the vulkan renderer')
endif

glslang = find_program('glslang', 'glslangValidator', native: true, required: true)

glslang_version_info = run_command(glslang, '--version', check: true).stdout()
glslang_version = glslang_version_info.split('\n')[0].split(':')[-1]

scenefx_files += files(
	'buffer.c',
	'pass.c',
	'pipeline.c',
	'renderer.c',
	'shaders.c',
	'vulkan.c',
)

scenefx_deps += dep_vulkan
features += {'vulkan-renderer': true}

subdir('shaders')
