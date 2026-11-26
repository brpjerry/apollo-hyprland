#version 300 es

#ifdef GL_ES
precision lowp float;
#endif

uniform sampler2D image;

layout(shared) uniform ColorMatrix {
  vec4 color_vec_y;
  vec4 color_vec_u;
  vec4 color_vec_v;
  vec2 range_y;
  vec2 range_uv;
};

// HDR streams from SDR-encoded frames (Wayland: Hyprland hands capture clients sRGB even for an HDR output).
// sdr_to_pq = 1: the input is sRGB with BT.709 primaries whose white is sdr_white (cd/m² / 10000); it is turned into
// SMPTE ST 2084 (PQ) with BT.2020 primaries before the YUV matrix. 0 (default): the input is used as is.
uniform int sdr_to_pq;
uniform highp float sdr_white;

highp vec3 srgb_to_linear(highp vec3 c) {
  highp vec3 lo = c / 12.92;
  highp vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
  return mix(lo, hi, step(vec3(0.04045), c));
}

highp vec3 pq_encode(highp vec3 l) {
  const highp float m1 = 0.1593017578125;
  const highp float m2 = 78.84375;
  const highp float c1 = 0.8359375;
  const highp float c2 = 18.8515625;
  const highp float c3 = 18.6875;
  highp vec3 p = pow(clamp(l, 0.0, 1.0), vec3(m1));
  return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

highp vec3 to_stream_rgb(highp vec3 rgb) {
  if (sdr_to_pq == 0) {
    return rgb;
  }
  // BT.709 -> BT.2020 (ITU-R BT.2087), column-major.
  const highp mat3 bt709_to_bt2020 = mat3(
    0.6274040, 0.0690970, 0.0163916,
    0.3292820, 0.9195400, 0.0880132,
    0.0433136, 0.0113612, 0.8955950
  );
  return pq_encode(bt709_to_bt2020 * srgb_to_linear(clamp(rgb, 0.0, 1.0)) * sdr_white);
}

in vec2 tex;
layout(location = 0) out float color;

void main()
{
	vec3 rgb = to_stream_rgb(texture(image, tex).rgb);
	float u = dot(color_vec_u.xyz, rgb) + color_vec_u.w;

	color = u * range_uv.x + range_uv.y;
}
