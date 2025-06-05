#include "QuadRenderer.hpp"
#include <filesystem>
#include <iostream>

// Vertex data: position (x, y) and color (r, g, b)
static constexpr float quadVertices[] = {
    // Positions   // Colors
    -1.0f, -1.0f,  1.0f, 0.0f, 0.0f, // bottom-left (red)
     1.0f, -1.0f,  0.0f, 1.0f, 0.0f, // bottom-right (green)
     1.0f,  1.0f,  0.0f, 0.0f, 1.0f, // top-right (blue)
    -1.0f,  1.0f,  1.0f, 1.0f, 0.0f  // top-left (yellow)
};

static constexpr unsigned int quadIndices[] = {
    0, 1, 2, // first triangle
    2, 3, 0  // second triangle
};

QuadRenderer::QuadRenderer() {
    // 1) Load and compile the shader
    shader = new Shader("shaders/quad.vert", "shaders/quad.frag");

    // 2) Generate VAO/VBO/EBO
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    unsigned int EBO;
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);

    // VBO
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    // EBO
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quadIndices), quadIndices, GL_STATIC_DRAW);

    // Position attribute (location = 0, 2 floats)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute (location = 1, 3 floats)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

QuadRenderer::~QuadRenderer() {
    if (shader) {
        delete shader;
    }
    if (VBO) {
        glDeleteBuffers(1, &VBO);
    }
    if (VAO) {
        glDeleteVertexArrays(1, &VAO);
    }
}

void QuadRenderer::Draw() {
    // 1) Hot-reload shader if in Debug, else skip
    shader->HotReloadIfNeeded();

    // 2) Bind and draw
    shader->Use();
    glBindVertexArray(VAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}
