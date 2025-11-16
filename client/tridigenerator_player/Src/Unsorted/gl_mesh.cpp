#include "gl_mesh.h"
#include <vector>
#include <glm/glm.hpp>

struct Vertex {
    float px, py, pz;
    float u, v;
};

GLMesh::GLMesh() {}
GLMesh::~GLMesh() { Destroy(); }

void GLMesh::CreateGrid(size_t width, size_t height) {
    Destroy();
    std::vector<Vertex> verts;
    verts.reserve(width * height);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            float u = float(x) / float(width - 1);
            float v = float(y) / float(height - 1);
            verts.push_back({u, v, 0.0f, u, v}); // z placeholder 0.0, real z supplied by posTex sampled in vertex shader or via VBO updates
        }
    }

    std::vector<uint32_t> inds;
    for (size_t y = 0; y + 1 < height; ++y) {
        for (size_t x = 0; x + 1 < width; ++x) {
            uint32_t i0 = uint32_t(y * width + x);
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + uint32_t(width);
            uint32_t i3 = i2 + 1;
            inds.push_back(i0); inds.push_back(i2); inds.push_back(i1);
            inds.push_back(i1); inds.push_back(i2); inds.push_back(i3);
        }
    }

    indexCount_ = GLsizei(inds.size());

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vertex), verts.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ibo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size() * sizeof(uint32_t), inds.data(), GL_STATIC_DRAW);

    // layout: location 0 = position (vec3), location 1 = uv (vec2)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, px));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, u));

    glBindVertexArray(0);
}

void GLMesh::Destroy() {
    if (ibo_) { glDeleteBuffers(1, &ibo_); ibo_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    indexCount_ = 0;
}

void GLMesh::Bind() { glBindVertexArray(vao_); }
void GLMesh::Unbind() { glBindVertexArray(0); }
void GLMesh::Draw() { glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, 0); }
