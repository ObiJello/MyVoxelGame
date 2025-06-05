#pragma once

#include "Shader.hpp"
#include <array>
#include <glad/glad.h>

class QuadRenderer {
public:
    QuadRenderer();
    ~QuadRenderer();

    // Call once per frame to draw the quad
    void Draw();

private:
    unsigned int VAO = 0, VBO = 0;
    Shader* shader = nullptr;

    // Initialize the mesh (a full‐screen quad) and load the shader
    void SetupMesh();
};
