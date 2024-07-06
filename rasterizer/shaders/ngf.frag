#version 450

layout (push_constant) uniform ShadingPushConstants {
	layout(offset = 208)
	vec3 viewing;
	vec3 color;
	uint mode;
};

layout (location = 0) in vec3 position;
layout (location = 2) in flat uint pindex;

layout (location = 0) out vec4 fragment;

// TODO: load the texture here...

const uint COLORS = 16;

// Color wheel for patches
// TODO: separate files
const vec3 WHEEL[16] = vec3[](
	vec3(0.880, 0.320, 0.320),
	vec3(0.880, 0.530, 0.320),
	vec3(0.880, 0.740, 0.320),
	vec3(0.810, 0.880, 0.320),
	vec3(0.600, 0.880, 0.320),
	vec3(0.390, 0.880, 0.320),
	vec3(0.320, 0.880, 0.460),
	vec3(0.320, 0.880, 0.670),
	vec3(0.320, 0.880, 0.880),
	vec3(0.320, 0.670, 0.880),
	vec3(0.320, 0.460, 0.880),
	vec3(0.390, 0.320, 0.880),
	vec3(0.600, 0.320, 0.880),
	vec3(0.810, 0.320, 0.880),
	vec3(0.880, 0.320, 0.740),
	vec3(0.880, 0.320, 0.530)
);

// Tone mapping
vec3 aces(vec3 x)
{
	const float a = 2.51;
	const float b = 0.03;
	const float c = 2.43;
	const float d = 0.59;
	const float e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Spherical harmonics lighting
mat4 R = mat4(
	-0.034424, -0.021329, 0.086425, -0.146638,
	-0.021329, 0.034424, 0.004396, -0.050819,
	0.086425, 0.004396, 0.088365, -0.377601,
	-0.146638, -0.050819, -0.377601, 1.618500
);

mat4 G = mat4(
	-0.032890, -0.013668, 0.066403, -0.107776,
	-0.013668, 0.032890, -0.012273, 0.013852,
	0.066403, -0.012273, -0.021086, -0.223067,
	-0.107776, 0.013852, -0.223067, 1.598757
);

mat4 B = mat4(
	-0.035777, -0.008999, 0.051376, -0.087520,
	-0.008999, 0.035777, -0.034691, 0.030949,
	0.051376, -0.034691, -0.010211, -0.081895,
	-0.087520, 0.030949, -0.081895, 1.402876
);

void main()
{
	vec3 light_direction = normalize(vec3(1, 1, 1));

	vec3 dU = dFdx(position);
	vec3 dV = dFdyFine(position);
	vec3 N = normalize(cross(dU, dV));

	if (mode == 2) {
		vec3 diffuse = color * vec3(max(0, dot(N, light_direction)));
		vec3 ambient = color * 0.1f;

		vec3 H = normalize(-viewing + light_direction);
		vec3 specular = vec3(pow(max(0, dot(N, H)), 16));

		// fragment = vec4(diffuse + specular + ambient, 1);

		vec4 n = vec4(N, 1);
		float r = dot(n, R * n);
		float g = dot(n, G * n);
		float b = dot(n, B * n);

		vec3 c = vec3(r, g, b) * 0.5;
		fragment = vec4(aces(c), 1);
		// fragment = pow(fragment, vec4(1/2.2));
	} else if (mode == 1) {
		// Normals
		fragment = vec4(0.5 + 0.5 * N, 1.0f);
	} else {
		// Patches
		fragment = vec4(WHEEL[pindex % COLORS], 1.0f);
	}
}
