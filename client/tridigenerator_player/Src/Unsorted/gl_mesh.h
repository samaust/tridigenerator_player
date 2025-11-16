#pragma once
#include <GLES3/gl3.h>
#include <cstddef>

class GLMesh {
public:
    GLMesh();
    ~GLMesh();
    void CreateGrid(size_t width, size_t height); // creates VAO/VBO/IBO with UV coords
    void Destroy();
    void Bind();
    void Unbind();
    void Draw();

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ibo_ = 0;
    GLsizei indexCount_ = 0;
};
