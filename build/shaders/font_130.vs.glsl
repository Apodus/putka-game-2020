#version 130

in vec3 vertexPosition;
in vec4 vertexColor;
in vec2 textureCoordinate;

uniform mat4 MVPMatrix;

out vec4 vertexColorInterpolated;
out vec2 texCoord;

void main() {
    gl_Position = MVPMatrix * vec4(vertexPosition, 1.0);
    texCoord = textureCoordinate;
    vertexColorInterpolated = vertexColor;
}