#version 440

// Full-screen triangle with no vertex buffer. The real compositor replaces
// this with YUV sampling shaders; this one proves the source -> qsb -> RHI path.
void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
