in float v_cross;
in float v_t;
in vec3 v_color;
in float v_alpha;
in float v_pulse;
out vec4 frag_color;

void main()
{
  float edge_aa = smoothstep(1.0, 0.15, abs(v_cross));
  float ends = smoothstep(0.0, 0.03, v_t) * smoothstep(1.0, 0.97, v_t);
  float base_a = v_alpha * edge_aa * (0.4 + 0.6 * ends);
  float pulse_a = v_pulse * edge_aa * 0.75;
  float a = clamp(base_a + pulse_a, 0.0, 1.0);
  if (a < 0.003)
    discard;
  vec3 col = mix(v_color, vec3(0.95, 0.94, 1.0), clamp(v_pulse * 0.75, 0.0, 0.75));
  frag_color = vec4(col, a);
}
