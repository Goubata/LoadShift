#version 450
layout(push_constant) uniform P { int iters; } pc;
layout(location = 0) out vec4 o;
void main() { float a = gl_FragCoord.x * 0.001; for (int i = 0; i < pc.iters; i++) a = sin(a * 1.1 + gl_FragCoord.y * 0.001); o = vec4(a); }
