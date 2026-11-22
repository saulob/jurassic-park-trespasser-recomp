#version 450

void main() {
    vec2 t = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(t.x * 2.0 - 1.0, t.y * 2.0 - 1.0, 0.0, 1.0);
}
