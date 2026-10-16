#version 150
// Sky dome: a sphere around the camera that keeps the world's orientation.

uniform mat4 p3d_ModelViewProjectionMatrix;

in vec4 p3d_Vertex;

out vec3 vDir;

void main() {
    gl_Position = p3d_ModelViewProjectionMatrix * p3d_Vertex;
    vDir = p3d_Vertex.xyz;
}
