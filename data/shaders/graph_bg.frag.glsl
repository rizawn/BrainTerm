in vec2 v_uv;
out vec4 frag_color;

uniform vec3 u_bg_col;
uniform vec3 u_vig_col;
uniform vec2 u_bulge_uv;
uniform float u_bulge_str;

void main()
{
  vec2 d = v_uv - vec2(0.5);
  float dist = length(d) * 1.41421356;
  float vig = smoothstep(0.22, 1.12, dist) * 0.80;
  vec3 col = mix(u_bg_col, u_vig_col, vig);

  /* Soft glowing warm-gold galactic core bulge + subtle outer nebula haze in Galaxy mode */
  if (u_bulge_str > 0.001) {
    vec2 bd = (v_uv - u_bulge_uv) * vec2(1.0, 1.35);
    float r = length(bd);
    float core = exp(-r * r * 65.0) * 0.36;
    float halo = exp(-r * r * 10.0) * 0.14;
    float outer = exp(-r * r * 3.0) * 0.05;
    vec3 gold = vec3(1.0, 0.784, 0.341);   /* #FFC857 Core Gold */
    vec3 ember = vec3(1.0, 0.541, 0.239);  /* #FF8A3D Ember */
    vec3 blue = vec3(0.310, 0.639, 1.0);   /* #4FA3FF Andromeda Blue */
    col += (gold * core + ember * halo + blue * outer) * u_bulge_str;
  }

  frag_color = vec4(col, 1.0);
}
