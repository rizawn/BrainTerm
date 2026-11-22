in vec2 v_uv;
out vec4 frag_color;

uniform vec3 u_bg_col;
uniform vec3 u_vig_col;

void main()
{
  vec2 d = v_uv - vec2(0.5);
  float dist = length(d) * 1.41421356;
  float vig = smoothstep(0.22, 1.12, dist) * 0.80;
  vec3 col = mix(u_bg_col, u_vig_col, vig);
  frag_color = vec4(col, 1.0);
}
