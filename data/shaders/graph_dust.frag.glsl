in vec2 v_local;
in float v_alpha;
out vec4 frag_color;

uniform vec3 u_dust_col;
uniform float u_intensity;

void main()
{
  float d2 = dot(v_local, v_local);
  if (d2 > 1.0)
    discard;
  float g = exp(-d2 * 4.2);
  frag_color = vec4(u_dust_col, g * v_alpha * u_intensity);
}
