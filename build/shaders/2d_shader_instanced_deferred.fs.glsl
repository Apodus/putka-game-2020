#version 330

in vec2 uv_pass;
in vec4 color_pass;
in vec4 normal_pass;
in vec4 position_pass;

uniform sampler2D tex;
uniform float light_direction_bias;
uniform float light_global_multiplier;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 frag_normal;
layout(location = 2) out vec4 frag_position;

void main()
{
	vec4 color_value = texture(tex, uv_pass) * color_pass;
	if(color_value.a < 0.01)
		discard;

    frag_color = color_value;
	
	 // rescale normal to 0-1 for rgba-texture storage. NOTE: the interpolated value will not have length 1!
	frag_normal = vec4(normal_pass.rgb * 0.5 + 0.5, light_direction_bias);
	frag_position = vec4(position_pass.rgb, light_global_multiplier);
}
