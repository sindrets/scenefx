vulkan_shaders_src = [
	'common.vert',
	'quad.frag',
	'quad_grad.frag',
	'blur1.frag',
	'blur2.frag',
	'blur_effects.frag',
	'box_shadow.frag',
	'tex.frag',
]

# Shader snippets pulled in with #include by the fragment shaders above. They are
# Vulkan-only: the GLES2 renderer embeds its own copies through embed.sh.
vulkan_shader_includes = files(
	'corner_alpha.glsl',
	'gradient.glsl',
)

vulkan_shaders = []
foreach shader : vulkan_shaders_src
	name = shader.underscorify() + '_data'
	args = [
		glslang,
		'-V', '@INPUT@',
		'-o', '@OUTPUT@',
		'--vn', name,
	]
	if glslang_version.version_compare('>=11.0.0')
		args += '--quiet'
	endif

	header = custom_target(
		shader + '_spv',
		output: shader + '.h',
		input: shader,
		command: args,
		depend_files: vulkan_shader_includes,
	)

	vulkan_shaders += [header]
endforeach

scenefx_files += vulkan_shaders
