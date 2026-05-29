// Diagnostic (EXCLUDE_FROM_ALL, GL): feed a dumped composite signal to the
// REAL NtscPostProcessor GLSL shader and write its demodulated output as a
// PPM — so we can see the actual on-GPU OE colours (not the CPU port the
// render tool uses). Reads /tmp/modes6/oe_signal.bin (560×192 R8).
//
//   build/oe_signal_view <signal.bin> <out.ppm>

#include "NtscPostProcessor.h"

#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {
using PFNGENFB   = void (*)(GLsizei, GLuint*);
using PFNBINDFB  = void (*)(GLenum, GLuint);
using PFNFBTEX2D = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
PFNGENFB p_gen=nullptr; PFNBINDFB p_bind=nullptr; PFNFBTEX2D p_tex=nullptr;
#ifndef GL_FRAMEBUFFER
#  define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#  define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
}

int main(int argc, char** argv)
{
    const char* sigPath = argc > 1 ? argv[1] : "/tmp/modes6/oe_signal.bin";
    const char* outPath = argc > 2 ? argv[2] : "/tmp/oe_real.ppm";
    const int sw = 560, sh = 192;

    std::ifstream f(sigPath, std::ios::binary);
    if (!f) { std::fprintf(stderr, "no signal %s\n", sigPath); return 1; }
    std::vector<uint8_t> sig(static_cast<size_t>(sw) * sh);
    f.read(reinterpret_cast<char*>(sig.data()), static_cast<std::streamsize>(sig.size()));

    if (!glfwInit()) { std::fprintf(stderr, "glfwInit\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "oe", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "no GL\n"); return 1; }
    glfwMakeContextCurrent(win);
    p_gen  = reinterpret_cast<PFNGENFB>(glfwGetProcAddress("glGenFramebuffers"));
    p_bind = reinterpret_cast<PFNBINDFB>(glfwGetProcAddress("glBindFramebuffer"));
    p_tex  = reinterpret_cast<PFNFBTEX2D>(glfwGetProcAddress("glFramebufferTexture2D"));

    pom2::NtscPostProcessor ntsc;
    if (!ntsc.initialize()) { std::fprintf(stderr, "init: %s\n", ntsc.lastError().c_str()); return 1; }
    unsigned tex = ntsc.process(sig.data(), sw, sh);
    const int ow = ntsc.outputWidth(), oh = ntsc.outputHeight();

    unsigned fbo=0; p_gen(1,&fbo); p_bind(GL_FRAMEBUFFER,fbo);
    p_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    std::vector<uint8_t> rgba(static_cast<size_t>(ow)*oh*4);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,ow,oh,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());

    std::ofstream o(outPath, std::ios::binary);
    o << "P6\n" << ow << " " << oh << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(ow)*oh*3);
    for (int i=0;i<ow*oh;++i){ rgb[i*3]=rgba[i*4]; rgb[i*3+1]=rgba[i*4+1]; rgb[i*3+2]=rgba[i*4+2]; }
    o.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    std::fprintf(stderr, "wrote %s (%dx%d) — REAL GLSL OE demod\n", outPath, ow, oh);
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
