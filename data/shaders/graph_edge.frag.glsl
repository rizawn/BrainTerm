in float v_cross;
in float v_t;
in vec3 v_color;
in float v_alpha;
out vec4 frag_color;

void main()
{
  float edge_aa = smoothstep(1.0, 0.15, abs(v_cross));
  float ends = smoothstep(0.0, 0.03, v_t) * smoothstep(1.0, 0.97, v_t);
  float a = v_alpha * edge_aa * (0.4 + 0.6 * ends);
  if (a < 0.003)
    discard;
  frag_color = vec4(v_color, a);
}
