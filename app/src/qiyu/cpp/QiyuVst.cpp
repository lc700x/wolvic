#include "QiyuVst.h"

#include "vrb/Logger.h"

#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <cstdint>
#include <cstring>

namespace crow {
namespace qiyu {
namespace vst {

namespace {

typedef int (*InitFn)();
typedef int (*UninitFn)();
typedef int (*StartFn)(int, int, void*, int);
typedef int (*StopFn)();
typedef void (*SetParamsFn)(float, float, float, float);
// drawSeethrough(int leftTexture, int rightTexture, int* outIndex, int64_t* outTime,
//                int* outExtra)
typedef int (*DrawFn)(int, int, int*, long long*, int*);

struct Api {
  void* camera { nullptr };
  void* sdk { nullptr };
  InitFn init { nullptr };
  UninitFn uninit { nullptr };
  StartFn start { nullptr };
  StopFn stop { nullptr };
  SetParamsFn setParams { nullptr };
  DrawFn draw { nullptr };
};

Api sApi;
bool sLoadAttempted { false };
bool sLoaded { false };

bool sStarted { false };
bool sInitialized { false };
bool sDisabled { false };
uint32_t sWidth { 0 };
uint32_t sHeight { 0 };
GLuint sTextures[2] { 0, 0 };
GLuint sProgram { 0 };
GLuint sVao { 0 };
GLuint sVbo { 0 };
GLuint sReadFbo { 0 };
GLint sUniformTexture { -1 };
float sFlipV { 0.0f };

int sFramesSeen { 0 };
bool sValidated { false };
bool sValid { false };
int sStartAttempts { 0 };

// The vendor SDK takes an optional frame callback. We never need the raw buffers (frames
// land in the GL textures through drawSeethrough), and the callback slot is not the camera
// client argument startCameraRGB() dereferences, so nothing is registered.

const char* kVertexShader =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aPosition;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "  vTexCoord = aTexCoord;\n"
    "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "}\n";

// Opaque: the camera image replaces the clear, so the SXR compositor must not blend
// anything behind it.
const char* kFragmentShader =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTexture;\n"
    "in vec2 vTexCoord;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    "  oColor = vec4(texture(uTexture, vTexCoord).rgb, 1.0);\n"
    "}\n";

GLuint CompileShader(GLenum aType, const char* aSource) {
  GLuint shader = glCreateShader(aType);
  glShaderSource(shader, 1, &aSource, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] { 0 };
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    VRB_ERROR("QIYU VST: shader compile failed: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

bool CreateQuad() {
  GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return false;
  }
  sProgram = glCreateProgram();
  glAttachShader(sProgram, vertex);
  glAttachShader(sProgram, fragment);
  glLinkProgram(sProgram);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = 0;
  glGetProgramiv(sProgram, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512] { 0 };
    glGetProgramInfoLog(sProgram, sizeof(log), nullptr, log);
    VRB_ERROR("QIYU VST: program link failed: %s", log);
    glDeleteProgram(sProgram);
    sProgram = 0;
    return false;
  }
  sUniformTexture = glGetUniformLocation(sProgram, "uTexture");

  glGenVertexArrays(1, &sVao);
  glGenBuffers(1, &sVbo);
  glBindVertexArray(sVao);
  glBindBuffer(GL_ARRAY_BUFFER, sVbo);
  // Interleaved: vec2 position, vec2 texture coordinate. The camera image orientation
  // varies between firmware builds, so it can be flipped live with
  //   adb shell setprop debug.wolvic.qiyu.vst.flip 1
  const float bottom = sFlipV > 0.5f ? 1.0f : 0.0f;
  const float top = sFlipV > 0.5f ? 0.0f : 1.0f;
  const float vertices[16] = {
      -1.0f, -1.0f, 0.0f, bottom,
       1.0f, -1.0f, 1.0f, bottom,
      -1.0f,  1.0f, 0.0f, top,
       1.0f,  1.0f, 1.0f, top,
  };
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        reinterpret_cast<const void*>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        reinterpret_cast<const void*>(2 * sizeof(float)));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return true;
}

bool CreateEyeTextures(uint32_t aWidth, uint32_t aHeight) {
  for (int i = 0; i < 2; ++i) {
    glGenTextures(1, &sTextures[i]);
    glBindTexture(GL_TEXTURE_2D, sTextures[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // drawSeethrough() attaches these with glFramebufferTexture2D, so level 0 has to
    // exist before the first frame.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(aWidth),
                 static_cast<GLsizei>(aHeight), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  return glGetError() == GL_NO_ERROR;
}

// One-off sanity check: read a few pixels back from the camera texture. A quad that is
// drawn from an empty texture would black out the scene, which is exactly the
// regression this bridge must not introduce.
void ValidateFrames() {
  sValidated = true;
  if (!sReadFbo) {
    glGenFramebuffers(1, &sReadFbo);
  }
  GLint previousFbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, sReadFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         sTextures[0], 0);
  unsigned char pixels[8 * 8 * 4] { 0 };
  const GLint x = static_cast<GLint>(sWidth / 2) - 4;
  const GLint y = static_cast<GLint>(sHeight / 2) - 4;
  glReadPixels(x, y, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));

  size_t nonZero = 0;
  for (unsigned char value : pixels) {
    if (value != 0) {
      ++nonZero;
    }
  }
  sValid = nonZero > 0;
  if (sValid) {
    VRB_LOG("QIYU VST: camera frames confirmed (%d frames)", sFramesSeen);
  } else {
    // Roll back so the eye buffer keeps the alpha-0 clear and the system path (if the
    // service ladder started it) still shows through.
    VRB_WARN("QIYU VST: camera texture is empty after %d frames; disabling the bridge",
             sFramesSeen);
    sDisabled = true;
    Stop();
  }
}

} // namespace

bool Load() {
  if (sLoadAttempted) {
    return sLoaded;
  }
  sLoadAttempted = true;

  // The vendor camera stack cannot work in this APK: libcameralibrary2.so resolves
  // getQvrCameraClientInstance() to the 2022 libsxrapi.so that our qiyuapi renderer needs,
  // and that client is ABI-incompatible with the 2023 camera library - driving it crashes in
  // CameraClientImpl::startCameraRGB(). Only the matching 2023 libsxrapi.so carries the
  // SvrCameraManager the SDK expects, and swapping it black-screens the renderer. The bridge
  // therefore stays compiled but inert unless it is explicitly requested for an experiment.
  char prop[PROP_VALUE_MAX] { 0 };
  __system_property_get("debug.wolvic.qiyu.vst", prop);
  if (prop[0] != '1') {
    sDisabled = true;
    return false;
  }
  VRB_LOG("QIYU VST: opt-in requested by debug.wolvic.qiyu.vst=1");

  // libcameralibrary2.so is a DT_NEEDED of libseethroughsdk.so; loading it first keeps
  // the resolver from depending on the linker search order.
  sApi.camera = dlopen("libcameralibrary2.so", RTLD_NOW | RTLD_LOCAL);
  if (!sApi.camera) {
    VRB_WARN("QIYU VST: libcameralibrary2.so unavailable: %s", dlerror());
  }
  sApi.sdk = dlopen("libseethroughsdk.so", RTLD_NOW | RTLD_LOCAL);
  if (!sApi.sdk) {
    VRB_WARN("QIYU VST: libseethroughsdk.so unavailable: %s", dlerror());
    sDisabled = true;
    return false;
  }

  sApi.init = reinterpret_cast<InitFn>(dlsym(sApi.sdk, "initSeeThrough"));
  sApi.uninit = reinterpret_cast<UninitFn>(dlsym(sApi.sdk, "uninitSeeThrough"));
  sApi.start = reinterpret_cast<StartFn>(dlsym(sApi.sdk, "startSeeThrough"));
  sApi.stop = reinterpret_cast<StopFn>(dlsym(sApi.sdk, "stopSeeThrough"));
  sApi.setParams =
      reinterpret_cast<SetParamsFn>(dlsym(sApi.sdk, "setParamsSeeThrough"));
  sApi.draw = reinterpret_cast<DrawFn>(dlsym(sApi.sdk, "drawSeethrough"));

  if (!sApi.start || !sApi.stop || !sApi.draw || !sApi.setParams || !sApi.init) {
    VRB_WARN("QIYU VST: libseethroughsdk.so is missing entry points");
    sDisabled = true;
    return false;
  }

  __system_property_get("debug.wolvic.qiyu.vst.flip", prop);
  sFlipV = prop[0] == '1' ? 1.0f : 0.0f;

  sLoaded = true;
  VRB_LOG("QIYU VST: vendor see-through SDK loaded (flipV=%.0f)", sFlipV);
  return true;
}

bool Available() {
  return Load() && !sDisabled;
}

bool Start(uint32_t aWidth, uint32_t aHeight) {
  if (sStarted) {
    return true;
  }
  if (sDisabled) {
    return false;
  }
  if (!Load() || aWidth == 0 || aHeight == 0) {
    return false;
  }
  if (++sStartAttempts > 3) {
    sDisabled = true;
    VRB_WARN("QIYU VST: giving up after %d start attempts", sStartAttempts - 1);
    return false;
  }

  sWidth = aWidth;
  sHeight = aHeight;
  if (!CreateEyeTextures(aWidth, aHeight) || !CreateQuad()) {
    VRB_ERROR("QIYU VST: cannot create the eye textures");
    Stop();
    return false;
  }

  // initSeeThrough() needs the eye EGL context current: it calls eglGetCurrentContext,
  // glGenTextures and InitializeResources(w, h).
  const int initialized = sApi.init();
  sInitialized = initialized != 0;
  // The vendor viewport/scale lives in setParamsSeeThrough; our textures are eye sized,
  // so the destination and the frustum are both the eye resolution here.
  sApi.setParams(static_cast<float>(aWidth), static_cast<float>(aHeight),
                 static_cast<float>(aWidth), static_cast<float>(aHeight));

  // startSeeThrough(1, 0, cb, 0) is deliberately not called: it spawns a detached camera
  // thread that reaches CameraClientImpl::startCameraRGB() with a client object the 2023
  // camera library cannot use, which is the reproducible SIGSEGV from 1.11.12. Without a
  // camera client there is no way to obtain frames, so the bridge reports failure and the
  // caller keeps the opaque scene.
  VRB_ERROR("QIYU VST: the vendor camera SDK cannot be driven from this renderer"
            " (init=%d); passthrough needs the system/runtime VST path", initialized);
  Stop();
  return false;
}

bool Update() {
  if (!sStarted || sDisabled) {
    return false;
  }
  int outIndex = 0;
  long long outTime = 0;
  int outExtra = 0;
  const int fresh = sApi.draw(static_cast<int>(sTextures[0]),
                              static_cast<int>(sTextures[1]), &outIndex, &outTime,
                              &outExtra);
  if (fresh) {
    ++sFramesSeen;
    // Give the camera pipeline some frames to settle before judging it.
    if (!sValidated && sFramesSeen >= 30) {
      ValidateFrames();
    }
  }
  return fresh != 0 && sValid;
}

bool Draw(int32_t aEye) {
  if (!sStarted || !sValid || aEye < 0 || aEye > 1) {
    return false;
  }

  GLint previousProgram = 0;
  GLint previousVao = 0;
  GLint previousTexture = 0;
  GLint previousActive = 0;
  GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
  GLboolean depthMask = GL_TRUE;
  GLboolean blend = glIsEnabled(GL_BLEND);
  glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);

  glUseProgram(sProgram);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, sTextures[aEye]);
  if (sUniformTexture >= 0) {
    glUniform1i(sUniformTexture, 0);
  }
  glBindVertexArray(sVao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);

  glActiveTexture(static_cast<GLenum>(previousActive));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
  glUseProgram(static_cast<GLuint>(previousProgram));
  glBindVertexArray(static_cast<GLuint>(previousVao));
  if (blend) {
    glEnable(GL_BLEND);
  }
  if (depthTest) {
    glEnable(GL_DEPTH_TEST);
  }
  glDepthMask(depthMask);
  return true;
}

void Stop() {
  if (sStarted && sApi.stop) {
    sApi.stop();
  }
  sStarted = false;
  sValid = false;
  sValidated = false;
  if (sInitialized && sApi.uninit) {
    sApi.uninit();
  }
  sInitialized = false;
  for (int i = 0; i < 2; ++i) {
    if (sTextures[i]) {
      glDeleteTextures(1, &sTextures[i]);
      sTextures[i] = 0;
    }
  }
  if (sVbo) {
    glDeleteBuffers(1, &sVbo);
    sVbo = 0;
  }
  if (sVao) {
    glDeleteVertexArrays(1, &sVao);
    sVao = 0;
  }
  if (sProgram) {
    glDeleteProgram(sProgram);
    sProgram = 0;
  }
  if (sReadFbo) {
    glDeleteFramebuffers(1, &sReadFbo);
    sReadFbo = 0;
  }
}

} // namespace vst
} // namespace qiyu
} // namespace crow
