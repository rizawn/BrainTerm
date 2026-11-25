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

  /* Soft glowing warm-gold galactic core bulge + faint dark dust lanes between arms */
  if (u_bulge_str > 0.001) {
    vec2 bd = (v_uv - u_bulge_uv) * vec2(1.0, 1.45);
    float r = length(bd);
    float ang = atan(bd.y, bd.x);
    /* Subtle dark logarithmic dust lanes between 4 spiral arms */
    float spiral = sin(4.0 * (ang - log(max(r, 0.03) / 0.12) / 0.28));
    float dust_lane = smoothstep(0.15, 0.95, -spiral) * smoothstep(0.06, 0.22, r) *
                      (1.0 - smoothstep(0.35, 0.65, r));
    col *= (1.0 - 0.38 * dust_lane * u_bulge_str);

    float core = exp(-r * r * 72.0) * 0.44;
    float halo = exp(-r * r * 12.0) * 0.16;
    float outer = exp(-r * r * 3.2) * 0.06;
    vec3 gold = vec3(1.0, 0.784, 0.341);   /* #FFC857 Core Gold */
    vec3 ember = vec3(1.0, 0.541, 0.239);  /* #FF8A3D Ember */
    vec3 blue = vec3(0.310, 0.639, 1.0);   /* #4FA3FF Andromeda Blue */
    col += (gold * core + ember * halo + blue * outer) * u_bulge_str;
  }

  frag_color = vec4(col, 1.0);
}
