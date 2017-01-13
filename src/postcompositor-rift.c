#include "postcompositor-rift.h"
#include "compositor.h"
#include "gl-renderer.h"
#include <wayland-server.h>
#include <GLES2/gl2.h>
#include <linux/input.h>

// Rift shaders
// Distortion shader

static const char* distortion_vertex_shader =
  "uniform vec2 EyeToSourceUVScale;\n"
  "uniform vec2 EyeToSourceUVOffset;\n"
  "uniform bool RightEye;\n"
  "uniform float angle;\n"
  "attribute vec2 Position;\n"
  "attribute vec2 TexCoord0;\n"
  "varying mediump vec2 oTexCoord0;\n"
  "attribute vec2 TexCoordR;\n"
  //"attribute vec2 TexCoordG;\n"
  "attribute vec2 TexCoordB;\n"
  "varying mediump vec2 oTexCoordR;\n"
  //"varying mediump vec2 oTexCoordG;\n"
  "varying mediump vec2 oTexCoordB;\n"
  "vec2 tanEyeAngleToTexture(vec2 v) {\n"
  "  vec2 result = v * EyeToSourceUVScale + EyeToSourceUVOffset;\n"
  "  result.y = 1.0 - result.y;\n"
  "  return result;\n"
  "}\n"
  "void main() {\n"
  "  oTexCoord0 = tanEyeAngleToTexture(TexCoord0);\n"
  "  oTexCoordR = tanEyeAngleToTexture(TexCoordR);\n"
  //"  oTexCoordG = tanEyeAngleToTexture(TexCoordG);\n"
  "  oTexCoordB = tanEyeAngleToTexture(TexCoordB);\n"
  "  vec2 b = Position;\n"
  "  b.x = Position.x*cos(angle) - Position.y*sin(angle);\n"
  "  b.y = Position.y*cos(angle) + Position.x*sin(angle);\n"
  "  gl_Position.xy = b;\n"
  "  gl_Position.z = 0.5;\n"
  "  gl_Position.w = 1.0;\n"
  "}\n";

static const char* distortion_fragment_shader =
  "varying mediump vec2 oTexCoord0;\n"
  "varying mediump vec2 oTexCoordR;\n"
  //"varying mediump vec2 oTexCoordG;\n"
  "varying mediump vec2 oTexCoordB;\n"
  "uniform sampler2D Texture0;\n"
  "void main() {\n"
/*  "  gl_FragColor.r = texture2D(Texture0, oTexCoordR).r;\n"
  "  gl_FragColor = texture2D(Texture0, oTexCoord0);\n"
  "  gl_FragColor.a = 1.0;\n"
  "  gl_FragColor.g = texture2D(Texture0, oTexCoordG).g;\n"
  "  gl_FragColor.b = texture2D(Texture0, oTexCoordB).b;\n"*/
  "  mediump float r = texture2D(Texture0, oTexCoordR).r;\n"
  "  mediump float g = texture2D(Texture0, oTexCoord0).g;\n"
  "  mediump float b = texture2D(Texture0, oTexCoordB).b;\n"
  "  gl_FragColor = vec4(r, g, b, 1.0);\n"
  "}\n";

// Rendered scene (per eye) shader

static const char* eye_vertex_shader = 
  "attribute vec3 Position;\n"
  "attribute vec2 TexCoord0;\n"
  "uniform mat4 Projection;\n"
  "uniform mat4 ModelView;\n"
  "varying mediump vec2 oTexCoord0;\n"
  "void main() {\n"
  "  oTexCoord0 = TexCoord0;\n"
  "  gl_Position = vec4(Position, 1.0) * ModelView * Projection;\n"
  //"  gl_Position = Projection * ModelView * vec4(Position, 1.0);\n"// * Projection;\n"
  "}\n";

static const char* eye_fragment_shader =
  "varying mediump vec2 oTexCoord0;\n"
  "uniform sampler2D Texture0;\n"
  "void main() {\n"
  "  gl_FragColor = texture2D(Texture0, oTexCoord0);\n"
  "}\n";

// End of shaders

// Matrix, Quaternion, and Vector math functions needed not in weston/matrix.h

static inline struct weston_matrix initIdentity(void)
{
  struct weston_matrix r;
  //according to weston source matricies are initialized to identity
  weston_matrix_init(&r);

  return r;
}


static inline struct weston_matrix transposeMat(const struct weston_matrix s)
{
  struct weston_matrix r;
  r.type = s.type;
  for (int i = 0; i < 4; i++)
  {
    for (int j = 0; j < 4; j++)
    {
      r.d[4*i+j] = s.d[4*j+i];
    }
  }

  return r;
}


static inline struct weston_matrix initTranslationF(float x, float y, float z)
{
  struct weston_matrix r;

  weston_matrix_init(&r);
  /*r.d[0] = 1; r.d[4] = 0; r.d[8] = 0;*/ r.d[12] = x;
  /*r.d[1] = 0; r.d[5] = 1; r.d[9] = 0;*/ r.d[13] = y;
  /*r.d[2] = 0; r.d[6] = 0; r.d[10] = 1;*/ r.d[14] = z;
  /*r.d[3] = 0; r.d[7] = 0; r.d[11] = 0; r.d[15] = 1;*/
  r.type = WESTON_MATRIX_TRANSFORM_TRANSLATE;

  return r;
}


static inline struct weston_matrix quatfToMatrix4f(const struct weston_vector q)
{
  struct weston_matrix m1, m2;

  weston_matrix_init(&m1);
  weston_matrix_init(&m2);

  m1.d[0] = q.f[3];   m1.d[4] = q.f[2];   m1.d[8] = -q.f[1];  m1.d[12] = q.f[0];
  m1.d[1] = -q.f[2];  m1.d[5] = q.f[3];   m1.d[9] = q.f[0];   m1.d[13] = q.f[1];
  m1.d[2] = q.f[1];   m1.d[6] = -q.f[0];  m1.d[10] = q.f[3];  m1.d[14] = q.f[2];
  m1.d[3] = -q.f[0];  m1.d[7] = -q.f[1];  m1.d[11] = -q.f[2]; m1.d[15] = q.f[3];
  m1.type = WESTON_MATRIX_TRANSFORM_ROTATE;

  m2.d[0] = q.f[3];   m2.d[4] = q.f[2];   m2.d[8] = -q.f[1];  m2.d[12] = -q.f[0];
  m2.d[1] = -q.f[2];  m2.d[5] = q.f[3];   m2.d[9] = q.f[0];   m2.d[13] = -q.f[1];
  m2.d[2] = q.f[1];   m2.d[6] = -q.f[0];  m2.d[10] = q.f[3];  m2.d[14] = -q.f[2];
  m2.d[3] = q.f[0];   m2.d[7] = q.f[1];   m2.d[11] = q.f[2];  m2.d[15] = q.f[3];
  m2.type = WESTON_MATRIX_TRANSFORM_ROTATE;

//#warning "Double check the order here"
  weston_matrix_multiply(&m1, &m2);
  return m1;
}

static inline struct weston_matrix initTranslation(const struct weston_vector position)
{
  return initTranslationF(position.f[0], position.f[1], position.f[2]);
}

static inline struct weston_matrix posefToMatrix4f(const struct weston_vector attitude, const struct weston_vector position)
{
  struct weston_matrix orientation = quatfToMatrix4f(attitude);
  struct weston_matrix translation = initTranslation(position);
  translation.d[12] = -translation.d[12];
  translation.d[13] = -translation.d[13];
  translation.d[14] = -translation.d[14];

  weston_matrix_multiply(&translation, &orientation);
  return translation;
}

// End of Matrix, Quaternion, and Vector math

int
config_rift(struct weston_compositor *compositor)
//config_rift(struct weston_compositor *compositor, EGLConfig egl_config, EGLDisplay egl_display, EGLSurface orig_surface, EGLContext egl_context)
{
  compositor->rift = calloc(1, sizeof *(compositor->rift));
  /*compositor->rift->egl_config = egl_config;
  compositor->rift->egl_display = egl_display;
  compositor->rift->orig_surface = orig_surface;
  compositor->rift->egl_context = egl_context;*/

  return 0;
}

void show_error_(const char *file, int line)
{
  GLenum error = GL_NO_ERROR;
  error = glGetError();
  if(error != GL_NO_ERROR)
  {
    switch(error)
    {
      case GL_INVALID_OPERATION: weston_log("\tGL Error: GL_INVALID_OPERATION - %s : %i\n", file, line); break;
      case GL_INVALID_ENUM: weston_log("\tGL Error: GL_INVALID_ENUM - %s : %i\n", file, line); break;
      case GL_INVALID_VALUE: weston_log("\tGL Error: GL_INVALID_VALUE - %s : %i\n", file, line); break;
      case GL_OUT_OF_MEMORY: weston_log("\tGL Error: GL_OUT_OF_MEMORY - %s : %i\n", file, line); break;
      case GL_INVALID_FRAMEBUFFER_OPERATION: weston_log("\tGL Error: GL_INVALID_FRAMEBUFFER_OPERATION - %s : %i\n", file, line); break;
    }
  }
}

#define show_error() show_error_(__FILE__,__LINE__)

static GLuint CreateShader(GLenum type, const char *shader_src)
{
  GLuint shader = glCreateShader(type);
  if(!shader)
    return 0;

  glShaderSource(shader, 1, &shader_src, NULL);
  glCompileShader(shader);

  GLint compiled = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if(!compiled)
  {
    GLint info_len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
    if(info_len > 1)
    {
      char *info_log = (char *)malloc(sizeof(char) * info_len);
      glGetShaderInfoLog(shader, info_len, NULL, info_log);
      weston_log("\tError compiling shader:\n\t%s\n", info_log);
      free(info_log);
    }
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint CreateProgram(const char *vertex_shader_src, const char *fragment_shader_src)
{
  GLuint vertex_shader = CreateShader(GL_VERTEX_SHADER, vertex_shader_src);
  if(!vertex_shader)
    return 0;
  GLuint fragment_shader = CreateShader(GL_FRAGMENT_SHADER, fragment_shader_src);
  if(!fragment_shader)
  {
    glDeleteShader(vertex_shader);
    return 0;
  }

  GLuint program_object = glCreateProgram();
  if(!program_object)
    return 0;
  glAttachShader(program_object, vertex_shader);
  glAttachShader(program_object, fragment_shader);

  glLinkProgram(program_object);

  GLint linked = 0;
  glGetProgramiv(program_object, GL_LINK_STATUS, &linked);
  if(!linked)
  {
    GLint info_len = 0;
    glGetProgramiv(program_object, GL_INFO_LOG_LENGTH, &info_len);
    if(info_len > 1)
    {
      char *info_log = (char *)malloc(info_len);
      glGetProgramInfoLog(program_object, info_len, NULL, info_log);
      weston_log("\tError linking program:\n\t%s\n", info_log);
      free(info_log);
    }
    glDeleteProgram(program_object);
    return 0;
  }

  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
  return program_object;
}

static void
toggle_sbs(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  if(compositor->rift->sbs == 1)
    compositor->rift->sbs = 0;
  else
    compositor->rift->sbs = 1;
}

static void
toggle_rotate(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  if(compositor->rift->rotate == 1)
    compositor->rift->rotate = 0;
  else
    compositor->rift->rotate = 1;
}

static void
move_in(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  compositor->rift->screen_z += 0.1;
}

static void
move_out(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  compositor->rift->screen_z -= 0.1;
}

static void
scale_up(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  compositor->rift->screen_scale += 0.1;
}

static void
scale_down(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  compositor->rift->screen_scale -= 0.1;
}

int
setup_rift(struct weston_compositor *compositor)
{
  struct oculus_rift *rift = compositor->rift;

  rift->enabled = 1;

  rift->screen_z = -5.0;
  rift->screen_scale = 1.0;

  weston_compositor_add_key_binding(compositor, KEY_5, MODIFIER_SUPER, 
      toggle_sbs, compositor);
  weston_compositor_add_key_binding(compositor, KEY_6, MODIFIER_SUPER, 
      toggle_rotate, compositor);
  weston_compositor_add_key_binding(compositor, KEY_7, MODIFIER_SUPER, 
      move_in, compositor);
  weston_compositor_add_key_binding(compositor, KEY_8, MODIFIER_SUPER, 
      move_out, compositor);
  weston_compositor_add_key_binding(compositor, KEY_9, MODIFIER_SUPER, 
      scale_up, compositor);
  weston_compositor_add_key_binding(compositor, KEY_0, MODIFIER_SUPER, 
      scale_down, compositor);

  /*// use this at some point in the future to detect and grab the rift display
  struct weston_output *output;
  wl_list_for_each(output, &compositor->output_list, link)
  {
    weston_log("Output (%i): %s\n\t%ix%i\n", output->id, output->name,
        output->width, output->height);
  }*/

  rift->distortion_shader = calloc(1, sizeof *(rift->distortion_shader));
  struct distortion_shader_ *d = rift->distortion_shader;
  d->program = CreateProgram(distortion_vertex_shader, distortion_fragment_shader);
  d->EyeToSourceUVScale = glGetUniformLocation(d->program, "EyeToSourceUVScale");
  d->EyeToSourceUVOffset = glGetUniformLocation(d->program, "EyeToSourceUVOffset");
  d->RightEye = glGetUniformLocation(d->program, "RightEye");
  d->angle = glGetUniformLocation(d->program, "angle");
  d->Position = glGetAttribLocation(d->program, "Position");
  d->TexCoord0 = glGetAttribLocation(d->program, "TexCoord0");
  d->TexCoordR = glGetAttribLocation(d->program, "TexCoordR");
  d->TexCoordG = glGetAttribLocation(d->program, "TexCoordG");
  d->TexCoordB = glGetAttribLocation(d->program, "TexCoordB");
  d->eyeTexture = glGetAttribLocation(d->program, "Texture0");

  rift->eye_shader = calloc(1, sizeof *(rift->eye_shader));
  struct eye_shader_ *e = rift->eye_shader;
  e->program = CreateProgram(eye_vertex_shader, eye_fragment_shader);
  e->Position = glGetAttribLocation(d->program, "Position");
  e->TexCoord0 = glGetAttribLocation(d->program, "TexCoord0");
  e->Projection = glGetUniformLocation(e->program, "Projection");
  e->ModelView = glGetUniformLocation(e->program, "ModelView");
  e->virtualScreenTexture = glGetAttribLocation(d->program, "Texture0");

  rift->scene = calloc(1, sizeof *(rift->scene));
  glGenBuffers(1, &rift->scene->vertexBuffer);
  glBindBuffer(GL_ARRAY_BUFFER, rift->scene->vertexBuffer);
  static const GLfloat rectangle[] = 
    {-1.0f, -1.0f, -0.5f, 
      1.0f, -1.0f, -0.5f, 
     -1.0f, 1.0f, -0.5f,
      1.0f, -1.0f, -0.5f, 
      1.0f, 1.0f, -0.5f,
     -1.0f, 1.0f, -0.5f};
  glBufferData(GL_ARRAY_BUFFER, sizeof(rectangle), rectangle, GL_STATIC_DRAW);

  glGenBuffers(2, &rift->scene->SBSuvsBuffer[0]);
  glGenBuffers(1, &rift->scene->uvsBuffer);
  static const GLfloat uvs[3][12] = 
   {{ 0.0, 0.0,
      0.5, 0.0,
      0.0, 1.0,
      0.5, 0.0,
      0.5, 1.0,
      0.0, 1.0},
   {  0.5, 0.0,
      1.0, 0.0,
      0.5, 1.0,
      1.0, 0.0,
      1.0, 1.0,
      0.5, 1.0},
   {  0.0, 0.0,
      1.0, 0.0,
      0.0, 1.0,
      1.0, 0.0,
      1.0, 1.0,
      0.0, 1.0}};
  glBindBuffer(GL_ARRAY_BUFFER, rift->scene->SBSuvsBuffer[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(uvs[0]), uvs[0], GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, rift->scene->SBSuvsBuffer[1]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(uvs[1]), uvs[1], GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, rift->scene->uvsBuffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(uvs[2]), uvs[2], GL_STATIC_DRAW);

  rift->width = 1920;
  rift->height = 1080;

  glGenTextures(1, &rift->fbTexture);
  glBindTexture(GL_TEXTURE_2D, rift->fbTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rift->width, rift->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &rift->redirectedFramebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, rift->redirectedFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rift->fbTexture, 0); show_error();
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
  {
    switch(glCheckFramebufferStatus(GL_FRAMEBUFFER))
    {
      case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: weston_log("incomplete attachment\n"); break;
      case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: weston_log("incomplete dimensions\n"); break;
      case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: weston_log("incomplete missing attachment\n"); break;
      case GL_FRAMEBUFFER_UNSUPPORTED: weston_log("unsupported\n"); break;
    }

    weston_log("framebuffer not working\n");
    show_error();
    exit(1);
  }
  glClear(GL_COLOR_BUFFER_BIT);

  /*EGLint pbufferAttributes[] = {
     EGL_WIDTH,           rift->width,
     EGL_HEIGHT,          rift->height,
     EGL_TEXTURE_FORMAT,  EGL_TEXTURE_RGB,
     EGL_TEXTURE_TARGET,  EGL_TEXTURE_2D,
     EGL_NONE
  };

  rift->pbuffer = eglCreatePbufferSurface(
      rift->egl_display, rift->egl_config, 
      pbufferAttributes);

  glGenTextures(1, &(rift->texture));
  glBindTexture(GL_TEXTURE_2D, rift->texture);
  //glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rift->width, rift->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  eglMakeCurrent(rift->egl_display, rift->pbuffer, rift->pbuffer, rift->egl_context);
  eglBindTexImage(rift->egl_display, rift->pbuffer, EGL_BACK_BUFFER);
  eglMakeCurrent(rift->egl_display, rift->orig_surface, rift->orig_surface, rift->egl_context);*/

#if defined(LIBOVR)
  ovr_Initialize(0);
  rift->hmd = ovrHmd_Create(0);
  if(rift->hmd == NULL)
  {
    rift->hmd = ovrHmd_CreateDebug(ovrHmd_DK2);
  }
  ovrHmd_ConfigureTracking(rift->hmd, ovrTrackingCap_Orientation | 
      ovrTrackingCap_Position | ovrTrackingCap_MagYawCorrection, 0);
  ovrHmd_ResetFrameTiming(rift->hmd, 0);
#elif defined(OPENHMD)
  uint32_t screen_w = 0;
  uint32_t screen_h = 0;
  if(rift->hmd_ctx == NULL) {
      rift->hmd_ctx = ohmd_ctx_create();
  }
  if(rift->hmd == NULL)
  {
    ohmd_device *hmd;
    ohmd_context *ctx = rift->hmd_ctx;
    int num_devices = ohmd_ctx_probe(rift->hmd_ctx);
    if(num_devices < 0)
    {
      printf("failed to probe devices: %s\n", ohmd_ctx_get_error(ctx));
      return -1;
    }

    printf("num devices: %d\n", num_devices);

    for(int i = 0; i < num_devices; i++)
    {
      printf("vendor: %s\n", ohmd_list_gets(ctx, i, OHMD_VENDOR));
      printf("product: %s\n", ohmd_list_gets(ctx, i, OHMD_PRODUCT));
      printf("path: %s\n", ohmd_list_gets(ctx, i, OHMD_PATH));
    }

    hmd = ohmd_list_open_device(ctx, 0);
    if(!hmd)
    {
      printf("failed to open device: %s\n", ohmd_ctx_get_error(ctx));
      return -1;
    }

    float fval = 0.0f;
    int ival = 0;
    ohmd_device_geti(hmd, OHMD_SCREEN_HORIZONTAL_RESOLUTION, &ival);
    printf("hres: %i\n", ival);
    screen_w = ival;
    //for now assume left eye is left half of the screen
    ohmd_device_geti(hmd, OHMD_SCREEN_VERTICAL_RESOLUTION, &ival);
    printf("vres: %i\n", ival);
    screen_h = ival;

    ohmd_device_getf(hmd, OHMD_SCREEN_HORIZONTAL_SIZE, &fval);
    printf("hsize: %f\n", fval);
    ohmd_device_getf(hmd, OHMD_SCREEN_VERTICAL_SIZE, &fval);
    printf("vsize: %f\n", fval);

    ohmd_device_getf(hmd, OHMD_LENS_HORIZONTAL_SEPARATION, &fval);
    printf("lens seperation: %f\n", fval);
    ohmd_device_getf(hmd, OHMD_LENS_VERTICAL_POSITION, &fval);
    printf("lens vcenter: %f\n", fval);
    ohmd_device_getf(hmd, OHMD_LEFT_EYE_FOV, &fval);
    printf("fov: %f\n", fval);
    ohmd_device_getf(hmd, OHMD_LEFT_EYE_ASPECT_RATIO, &fval);
    printf("aspect: %f\n", fval);

    rift->hmd = hmd;
  }
#endif

  int eye;
  for(eye = 0; eye < 2; eye++)
  {
#if defined(LIBOVR)
    ovrFovPort fov = rift->hmd->DefaultEyeFov[eye];
    ovrEyeRenderDesc renderDesc = ovrHmd_GetRenderDesc(rift->hmd, eye, fov);
#endif
    struct EyeArg *eyeArg = &rift->eyeArgs[eye];

#if defined(LIBOVR)
    ovrMatrix4f proj = ovrMatrix4f_Projection(fov, 0.1, 100000, true);
    eyeArg->projection = initIdentity();
    int k, j;
    for(k=0; k<4; k++)
    {
      for(j=0; j<4; j++)
      {
        eyeArg->projection.d[4*k+j] = proj.M[k][j];
      }
    }
    eyeArg->projection.type = WESTON_MATRIX_TRANSFORM_OTHER;
    /*int j, k;
    for(k=0; k<4; k++)
    {
      for(j=0; j<4; j++)
      {
        printf("%f\t", eyeArg->projection.M[k][j]);
      }
      printf("\n");
    }*/
    rift->hmdToEyeOffsets[eye].f[0] = renderDesc.HmdToEyeViewOffset.x;
    rift->hmdToEyeOffsets[eye].f[1] = renderDesc.HmdToEyeViewOffset.y;
    rift->hmdToEyeOffsets[eye].f[2] = renderDesc.HmdToEyeViewOffset.z;
    rift->hmdToEyeOffsets[eye].f[3] = 0;
    ovrRecti texRect;
    texRect.Size = ovrHmd_GetFovTextureSize(rift->hmd, eye, rift->hmd->DefaultEyeFov[eye],
        1.0f);
    texRect.Pos.x = texRect.Pos.y = 0;
    eyeArg->textureWidth = texRect.Size.w;
    eyeArg->textureHeight = texRect.Size.h;
#elif defined(OPENHMD)
    eyeArg->projection = initIdentity();
    eyeArg->projection.type = WESTON_MATRIX_TRANSFORM_OTHER;
    if (eye == 0)
      ohmd_device_getf(rift->hmd, OHMD_LEFT_EYE_GL_PROJECTION_MATRIX, eyeArg->projection.d);
    else
      ohmd_device_getf(rift->hmd, OHMD_RIGHT_EYE_GL_PROJECTION_MATRIX, eyeArg->projection.d);
      eyeArg->projection = transposeMat(eyeArg->projection);

    eyeArg->textureWidth = screen_w;
    eyeArg->textureHeight = screen_h;
#endif

    glGenTextures(1, &eyeArg->texture);
    glBindTexture(GL_TEXTURE_2D, eyeArg->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, eyeArg->textureWidth, eyeArg->textureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &eyeArg->framebuffer); show_error();
    glBindFramebuffer(GL_FRAMEBUFFER, eyeArg->framebuffer); show_error();
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eyeArg->texture, 0); show_error();
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
      switch(glCheckFramebufferStatus(GL_FRAMEBUFFER))
      {
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: weston_log("incomplete attachment\n"); break;
        case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: weston_log("incomplete dimensions\n"); break;
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: weston_log("incomplete missing attachment\n"); break;
        case GL_FRAMEBUFFER_UNSUPPORTED: weston_log("unsupported\n"); break;
      }

      weston_log("framebuffer not working\n");
      show_error();
      exit(1);
    }
    if(eye)
    {
      glClearColor(1.0, 0.0, 0.0, 1.0); show_error();
    }
    else
    {
      glClearColor(0.0, 1.0, 0.0, 1.0); show_error();
    }
    glClear(GL_COLOR_BUFFER_BIT); show_error();

    /*EGLint eyePbufferAttributes[] = {
       EGL_WIDTH,           texRect.Size.w,
       EGL_HEIGHT,          texRect.Size.h,
       EGL_TEXTURE_FORMAT,  EGL_TEXTURE_RGB,
       EGL_TEXTURE_TARGET,  EGL_TEXTURE_2D,
       EGL_NONE
    };

    eyeArg.surface = eglCreatePbufferSurface(
        rift->egl_display, rift->egl_config, 
        eyePbufferAttributes);*/

#if defined(LIBOVR)
    ovrVector2f scaleAndOffset[2];
    ovrHmd_GetRenderScaleAndOffset(fov, texRect.Size, texRect, scaleAndOffset);
    eyeArg->scale.f[0] = scaleAndOffset[0].x;
    eyeArg->scale.f[1] = scaleAndOffset[0].y;
    eyeArg->offset.f[0] = scaleAndOffset[1].x;
    eyeArg->offset.f[1] = scaleAndOffset[1].y;

    ovrHmd_CreateDistortionMesh(rift->hmd, eye, fov, 0, &eyeArg->mesh);
#elif defined(OPENHMD)
    if (eye == 0)
    {
      eyeArg->scale.f[0] = 1.0f;
      eyeArg->scale.f[1] = 1.0f;
      eyeArg->offset.f[0] = 0.0f;
      eyeArg->offset.f[1] = 0.0f;
    }
    else
    {
      eyeArg->scale.f[0] = 1.0f;
      eyeArg->scale.f[1] = 1.0f;
      eyeArg->offset.f[0] = 0.0f;
      eyeArg->offset.f[1] = 0.0f;
    }
#endif

    glGenBuffers(1, &eyeArg->indexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eyeArg->indexBuffer);
#if defined(LIBOVR)
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, eyeArg->mesh.IndexCount * sizeof(unsigned short), eyeArg->mesh.pIndexData, GL_STATIC_DRAW);
    eyeArg->indexBufferCount = eyeArg->mesh.IndexCount;
#elif defined(OPENHMD)
    //for now dummy rectangular mesh
    unsigned short mesh_idxs[6] = {0,1,2,3,4,5};
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(mesh_idxs), mesh_idxs, GL_STATIC_DRAW);
    eyeArg->indexBufferCount = 6;
#endif
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    uint i;
#if defined(LIBOVR)
    float vertices_buffer[eyeArg->mesh.VertexCount*2];
    float uvs_buffer[3][eyeArg->mesh.VertexCount*2];
    for(i=0; i<eyeArg->mesh.VertexCount; i++)
    {
      ovrDistortionVertex vertex = eyeArg->mesh.pVertexData[i];
      vertices_buffer[i*2] = vertex.ScreenPosNDC.x;
      vertices_buffer[(i*2)+1] = vertex.ScreenPosNDC.y;
      uvs_buffer[0][i*2] = vertex.TanEyeAnglesR.x;
      uvs_buffer[0][(i*2)+1] = vertex.TanEyeAnglesR.y;
      uvs_buffer[1][i*2] = vertex.TanEyeAnglesG.x;
      uvs_buffer[1][(i*2)+1] = vertex.TanEyeAnglesG.y;
      uvs_buffer[2][i*2] = vertex.TanEyeAnglesB.x;
      uvs_buffer[2][(i*2)+1] = vertex.TanEyeAnglesB.y;
    }
#elif defined(OPENHMD)
#endif

    glGenBuffers(1, &eyeArg->vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, eyeArg->vertexBuffer);
#if defined(LIBOVR)
    glBufferData(GL_ARRAY_BUFFER, eyeArg->mesh.VertexCount * sizeof(GL_FLOAT) * 2, vertices_buffer, GL_STATIC_DRAW);
#elif defined(OPENHMD)
    //for now dummy rectangular mesh
    GLfloat rect_mesh[12];
    if (eye == 0) {
      float *r = rect_mesh;
      r[0] = -1.0f; r[1] = 1.0f;
      r[2] = 0.0f;  r[3] = 1.0f;
      r[4] = -1.0f; r[5] = -1.0f;
      r[6] = 0.0f;  r[7] = 1.0f;
      r[8] = 0.0f;  r[9] = -1.0f;
      r[10] = -1.0f; r[11] = -1.0f;
    }
    else {
      float *r = rect_mesh;
      r[0] = 0.0f;  r[1] = 1.0f;
      r[2] = 1.0f;  r[3] = 1.0f;
      r[4] = 0.0f;  r[5] = -1.0f;
      r[6] = 1.0f;  r[7] = 1.0f;
      r[8] = 1.0f;  r[9] = -1.0f;
      r[10] = 0.0f;  r[11] = -1.0f;
    }
    glBufferData(GL_ARRAY_BUFFER, sizeof(rect_mesh), rect_mesh, GL_STATIC_DRAW);
#endif
    glGenBuffers(3, &eyeArg->uvsBuffer[0]);
    for(i=0; i<3; i++)
    {
      glBindBuffer(GL_ARRAY_BUFFER, eyeArg->uvsBuffer[i]);
#if defined(LIBOVR)
      glBufferData(GL_ARRAY_BUFFER, eyeArg->mesh.VertexCount * sizeof(GL_FLOAT) * 2, uvs_buffer[i], GL_STATIC_DRAW);
#elif defined(OPENHMD)
      //glBufferData(GL_ARRAY_BUFFER, sizeof(uvs[i]), uvs[i], GL_STATIC_DRAW);
/* since we don't handle distortion and chromatic aberration yet, just use the green channel uvs for every one */
      glBufferData(GL_ARRAY_BUFFER, sizeof(uvs[1]), uvs[1], GL_STATIC_DRAW);
#endif
      glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
  }

  return 0;
}
int
render_rift(struct weston_compositor *compositor, GLuint original_program)
{
  int i;
  struct oculus_rift *rift = compositor->rift;

  // copy rift->pbuffer into rift->texture
  /*eglMakeCurrent(rift->egl_display, rift->pbuffer, rift->pbuffer, rift->egl_context);
  //glClearColor(0.5, 0.0, 0.5, 1.0);
  //glClear(GL_COLOR_BUFFER_BIT);
  glBindTexture(GL_TEXTURE_2D, rift->texture);
  eglReleaseTexImage(rift->egl_display, rift->pbuffer, EGL_BACK_BUFFER);
  eglBindTexImage(rift->egl_display, rift->pbuffer, EGL_BACK_BUFFER);
  eglMakeCurrent(rift->egl_display, rift->orig_surface, rift->orig_surface, rift->egl_context);*/
  // render eyes

  static int frameIndex = 0;
  ++frameIndex;
#if defined(LIBOVR)
  ovrPosef eyePoses[2];
  ovrHmd_BeginFrameTiming(rift->hmd, frameIndex);
  ovrVector3f eye_offsets[2];
  for (i=0; i<2; i++)
  {
    eye_offsets[i].x = rift->hmdToEyeOffsets[i].f[0];
    eye_offsets[i].y = rift->hmdToEyeOffsets[i].f[1];
    eye_offsets[i].z = rift->hmdToEyeOffsets[i].f[2];
  }
  ovrHmd_GetEyePoses(rift->hmd, frameIndex, eye_offsets, eyePoses, NULL);
#elif defined(OPENHMD)
#endif

  glEnable(GL_DEPTH_TEST);
  glUseProgram(rift->eye_shader->program);
  for(i=0; i<2; i++)
  {
#if defined(LIBOVR)
    const ovrEyeType eye = rift->hmd->EyeRenderOrder[i];
#elif defined(OPENHMD)
    //assume left,right render order
    const int eye = i;
#endif
    struct EyeArg eyeArg = rift->eyeArgs[eye];
    
    struct weston_matrix Model = initTranslationF(0.0, 0.0, rift->screen_z);
    struct weston_matrix Scale = initIdentity();
    weston_matrix_scale(&Scale, 3.2 * rift->screen_scale, 1.8 * rift->screen_scale, 1.0);
    weston_matrix_multiply(&Scale, &Model);
    struct weston_matrix MV;
#if defined(LIBOVR)
    struct weston_vector eye_quat;
    eye_quat.f[0] = eyePoses[eye].Orientation.x;
    eye_quat.f[1] = eyePoses[eye].Orientation.y;
    eye_quat.f[2] = eyePoses[eye].Orientation.z;
    eye_quat.f[3] = eyePoses[eye].Orientation.w;
    struct weston_vector eye_pos;
    eye_pos.f[0] = eyePoses[eye].Position.x;
    eye_pos.f[1] = eyePoses[eye].Position.y;
    eye_pos.f[2] = eyePoses[eye].Position.z;
    eye_pos.f[3] = 0;
    MV = posefToMatrix4f(eye_quat, eye_pos);
    weston_matrix_multiply(&MV, &Scale);
#elif defined(OPENHMD)
    MV.type = WESTON_MATRIX_TRANSFORM_SCALE | WESTON_MATRIX_TRANSFORM_ROTATE | WESTON_MATRIX_TRANSFORM_TRANSLATE;
    if (eye == 0)
      ohmd_device_getf(rift->hmd, OHMD_LEFT_EYE_GL_MODELVIEW_MATRIX, MV.d);
    else
      ohmd_device_getf(rift->hmd, OHMD_RIGHT_EYE_GL_MODELVIEW_MATRIX, MV.d);
    MV = transposeMat(MV);
#endif
    //MV = initIdentity();
    //MV.M[2][3] = 5;

    glBindFramebuffer(GL_FRAMEBUFFER, eyeArg.framebuffer);
    glViewport(0, 0, eyeArg.textureWidth, eyeArg.textureHeight);
    glClearColor(0.0, 0.0, 0.2, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    glUniform1i(rift->eye_shader->virtualScreenTexture, 0);
    glUniformMatrix4fv(rift->eye_shader->Projection, 1, GL_FALSE, &eyeArg.projection.d[0]);
    glUniformMatrix4fv(rift->eye_shader->ModelView, 1, GL_FALSE, &MV.d[0]);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glBindTexture(GL_TEXTURE_2D, rift->fbTexture);
    glBindBuffer(GL_ARRAY_BUFFER, rift->scene->vertexBuffer);
    glVertexAttribPointer(rift->eye_shader->Position, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), NULL);
    if(rift->sbs == 1)
      glBindBuffer(GL_ARRAY_BUFFER, rift->scene->SBSuvsBuffer[eye]);
    else
      glBindBuffer(GL_ARRAY_BUFFER, rift->scene->uvsBuffer);
    glVertexAttribPointer(rift->eye_shader->TexCoord0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    //render_eye(rift, eyeArg);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // render distortion
  glUseProgram(rift->distortion_shader->program);
  glViewport(0, 0, 1920, 1080);

  glClearColor(0.0, 0.1, 0.0, 1.0);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);

  float angle = 0.0;
  if(rift->rotate == 1)
  {
    angle = 1.57079633; // 90 degrees, in radians
    glViewport(0, 0, 1080, 1920);
  }

  int eye;
  for(eye=0; eye<2; eye++)
  {
    struct EyeArg eyeArg = rift->eyeArgs[eye];
    glUniform2fv(rift->distortion_shader->EyeToSourceUVScale, 1, (float *)&eyeArg.scale);
    glUniform2fv(rift->distortion_shader->EyeToSourceUVOffset, 1, (float *)&eyeArg.offset);
    glUniform1i(rift->distortion_shader->RightEye, eye);
    glUniform1f(rift->distortion_shader->angle, angle);
    glUniform1i(rift->distortion_shader->eyeTexture, 0);

    //glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, eyeArg.texture);

    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.vertexBuffer);
    glVertexAttribPointer(rift->distortion_shader->Position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->Position);

    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.uvsBuffer[1]);
    glVertexAttribPointer(rift->distortion_shader->TexCoord0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->TexCoord0);
    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.uvsBuffer[0]);
    glVertexAttribPointer(rift->distortion_shader->TexCoordR, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->TexCoordR);
    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.uvsBuffer[1]);
    glVertexAttribPointer(rift->distortion_shader->TexCoordG, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->TexCoordG);
    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.uvsBuffer[2]);
    glVertexAttribPointer(rift->distortion_shader->TexCoordB, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->TexCoordB);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eyeArg.indexBuffer);

    glDrawElements(GL_TRIANGLES, eyeArg.indexBufferCount, GL_UNSIGNED_SHORT, 0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  }

  //glEnable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);

#if defined(LIBOVR)
  ovrHmd_EndFrameTiming(rift->hmd);
#elif defined(OPENHMD)
  if (rift->hmd_ctx)
    ohmd_ctx_update(rift->hmd_ctx);
#endif

  // set program back to original shader program
  glUseProgram(original_program);
  return 0;
}
