// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Minimal GLSL shader compile/link helper. Used by NtscPostProcessor to
// build the OpenEmulator-style composite-video shader pass without
// dragging in a full shader framework. Header-only API; the cpp owns the
// platform GL include and the diagnostic strings.

#ifndef POM2_OPENGL_SHADER_H
#define POM2_OPENGL_SHADER_H

#include <string>

namespace pom2 {

// Compile + link a single (vertex, fragment) program. Returns the GL
// program object on success, 0 on failure. The chosen GLSL version line
// (e.g. "#version 150" on desktop, "#version 300 es" on Emscripten) is
// prepended automatically — pass only the body of each shader. Any
// compile / link errors are written to `errorOut` and also logged via
// pom2::log() at warn level.
unsigned int compileShaderProgram(const char* vertexBody,
                                  const char* fragmentBody,
                                  std::string* errorOut = nullptr);

// True when the running GL context is GLES (Emscripten / WebGL2). The
// shader sources need a `precision highp float;` line and the GLES
// version string; this lets the postprocessor swap them in.
bool shaderRunningOnGLES();

} // namespace pom2

#endif // POM2_OPENGL_SHADER_H
