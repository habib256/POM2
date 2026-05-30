// Offscreen harness: compile the CrtEffectStack shader and render a barrel +
// scanline + shadow-mask test, to confirm the GLSL compiles and the moiré
// fix works. Writes PPMs for both barrel-on and barrel-off.
#include "CrtEffectStack.h"
#include "NtscPostProcessor.h"

#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

static void writePPM(const char* path, const std::vector<uint8_t>& rgba, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    // GL texture origin is bottom-left; flip vertically for PPM (top-left).
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            f.put(char(p[0])); f.put(char(p[1])); f.put(char(p[2]));
        }
}

static bool loadPPM(const char* path, std::vector<uint8_t>& rgba, int& w, int& h) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string magic; f >> magic; if (magic != "P6") return false;
    int maxv; f >> w >> h >> maxv; f.get();   // single whitespace after maxv
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    f.read(reinterpret_cast<char*>(rgb.data()), rgb.size());
    rgba.assign(size_t(w) * h * 4, 255);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        rgba[i*4+0] = rgb[i*3+0]; rgba[i*4+1] = rgb[i*3+1]; rgba[i*4+2] = rgb[i*3+2];
    }
    return true;
}

int main(int argc, char** argv) {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "crt", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "createWindow failed\n"); return 1; }
    glfwMakeContextCurrent(win);

    // ── Source pattern (560×192): a bright field with colour blocks and a
    //    fine 1px checker region — exactly the kind of content whose
    //    scanlines/mask moiré under barrel curvature.
    int sw = 560, sh = 192;
    std::vector<uint8_t> src;
    if (argc > 1 && loadPPM(argv[1], src, sw, sh)) {
        std::printf("loaded source %s (%dx%d)\n", argv[1], sw, sh);
    } else {
    src.assign(size_t(sw) * sh * 4, 0);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            uint8_t r = 220, g = 220, b = 220;          // bright grey field
            if (y < 24)               { r = 230; g = 40;  b = 200; } // magenta bar (top — barrel curve)
            else if (y >= sh - 24)    { r = 40;  g = 220; b = 60;  } // green bar (bottom)
            else if (x < 40)          { r = 40;  g = 80;  b = 230; } // blue edge (left curve)
            else if (x >= sw - 40)    { r = 230; g = 180; b = 30;  } // amber edge (right)
            else if (((x ^ y) & 1))   { r = g = b = 255; }           // 1px checker → worst-case
            else                      { r = g = b = 30;  }
            uint8_t* p = &src[(size_t(y) * sw + x) * 4];
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
    }
    GLuint srcTex = 0;
    glGenTextures(1, &srcTex);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, src.data());

    pom2::CrtEffectStack stack;
    if (!stack.initialize()) {
        std::fprintf(stderr, "SHADER COMPILE FAILED: %s\n", stack.lastError().c_str());
        return 2;
    }
    std::printf("shader compiled OK\n");

    const int dstW = 840, dstH = 768;   // typical on-screen size
    std::vector<uint8_t> out(size_t(dstW) * dstH * 4);

    auto run = [&](float barrel, const char* path) {
        pom2::NtscParams p;
        p.brightness = 0.0f; p.contrast = 1.0f; p.saturation = 1.0f;
        p.persistence = 0.0f;            // isolate geometry/scanlines/mask
        p.scanlines = 0.35f;
        p.barrel = barrel;
        p.shadowMask = pom2::NtscParams::ShadowMask::ApertureGrille;
        p.shadowMaskStrength = 0.5f;
        stack.setParams(p);
        // Two frames so persistence ping-pong settles (persistence=0 anyway).
        GLuint t = stack.process(srcTex, sw, sh, dstW, dstH);
        t        = stack.process(srcTex, sw, sh, dstW, dstH);
        if (!t) { std::fprintf(stderr, "process returned 0\n"); return; }
        glBindTexture(GL_TEXTURE_2D, t);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        writePPM(path, out, dstW, dstH);
        std::printf("wrote %s (%dx%d) barrel=%.2f\n", path, dstW, dstH, barrel);
    };

    run(0.25f, "/tmp/crt_barrel_on.ppm");
    run(0.0f,  "/tmp/crt_barrel_off.ppm");

    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
