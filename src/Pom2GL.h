// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Pom2GL.h — pull in the platform-correct OpenGL header, once.
//
// Seven translation units used to repeat this #if/#elif/#else block, and they
// had drifted: some included <GL/glext.h> and some did not, some used
// <OpenGL/gl.h> and some <OpenGL/gl3.h>. Two bugs fell out of that on Windows,
// where the block was never exercised until the release build ran:
//
//   * The Windows SDK's <GL/gl.h> is NOT self-contained — it uses WINGDIAPI and
//     APIENTRY, which come from <windows.h>. Including it first yields a wall of
//     "syntax error: 'void' should be preceded by ';'" inside gl.h itself.
//   * That header is also frozen at OpenGL **1.1** (1995). Anything newer —
//     GL_CLAMP_TO_EDGE is GL 1.2 — is simply absent, which is why MainWindow.cpp
//     failed with "'GL_CLAMP_TO_EDGE': undeclared identifier". <GL/glext.h>
//     supplies the modern enums.
//
// Include this instead of a bare GL header. The tier (desktop GL vs GLES) comes
// from Pom2Build.h, so the WASM and Raspberry Pi builds are covered by the same
// decision.

#ifndef POM2_GL_H
#define POM2_GL_H

#include "Pom2Build.h"

#if POM2_GL_ES
// GLES 3.0: WebGL2 in the browser, Mesa V3D on a Raspberry Pi. One header,
// self-contained, and it declares every entry point we call.
#  include <GLES3/gl3.h>
#elif defined(__APPLE__)
// Apple's gl3.h is the core-profile header matching the GL 3.2 context main.cpp
// requests. GL_SILENCE_DEPRECATION is already set by CMake for this target.
#  include <OpenGL/gl3.h>
#else
#  ifdef _WIN32
// MUST precede <GL/gl.h> — see the header comment above. LEAN_AND_MEAN keeps
// the include cone small, NOMINMAX stops windows.h defining min/max as macros
// and breaking std::min / std::max at every call site.
#    ifndef WIN32_LEAN_AND_MEAN
#      define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#      define NOMINMAX
#    endif
#    include <windows.h>
#  endif
#  include <GL/gl.h>
// GL 1.2+ enums and the GL 2.0+ function-pointer typedefs (PFNGL*PROC) that the
// dynamic loaders in OpenGLShader.cpp / CrtEffectStack.cpp / NtscPostProcessor.cpp
// declare their entry points with.
#  include <GL/glext.h>
#endif

#endif  // POM2_GL_H
