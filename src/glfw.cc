#include "common.h"
#include <cstdio>
#include <cstdlib>

using namespace v8;
using namespace node;

#include <vector>
#include <string>
#include <iostream>
using namespace std;

namespace glfw {

/* @Module: initialization and version information */

#define SET_RETURN_VALUE(x) info.GetReturnValue().Set(x);
struct state { double yaw, pitch, lastX, lastY; bool ml; float offset_x, offset_y;};

struct float3 {
  float x, y, z;
};

struct float2 {
  float x, y;
};

JS_METHOD(Init) {
  SET_RETURN_VALUE(JS_BOOL(glfwInit()==GL_TRUE));
}

JS_METHOD(Terminate) {
  glfwTerminate();
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(GetVersion) {
  int major, minor, rev;
  glfwGetVersion(&major,&minor,&rev);
  Local<Array> arr=Array::New(v8::Isolate::GetCurrent(), 3);
  arr->Set(JS_STR("major"),JS_INT(major));
  arr->Set(JS_STR("minor"),JS_INT(minor));
  arr->Set(JS_STR("rev"),JS_INT(rev));
  SET_RETURN_VALUE(arr);
}

JS_METHOD(GetVersionString) {
  const char* ver=glfwGetVersionString();
  SET_RETURN_VALUE(JS_STR(ver));
}

/* @Module: Time input */

JS_METHOD(GetTime) {
  SET_RETURN_VALUE(JS_NUM(glfwGetTime()));
}

JS_METHOD(SetTime) {
  double time = Nan::To<double> (info[0]).FromJust();
  glfwSetTime(time);
  SET_RETURN_VALUE(Nan::Undefined());
}

/* @Module: monitor handling */

/* TODO: Monitor configuration change callback */

JS_METHOD(GetMonitors) {
  int monitor_count, mode_count, xpos, ypos, width, height;
  int i, j;
  GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);
  GLFWmonitor *primary = glfwGetPrimaryMonitor();
  const GLFWvidmode *mode, *modes;
  
  Local<Array> js_monitors = Array::New(v8::Isolate::GetCurrent(), monitor_count);
  Local<Object> js_monitor, js_mode;
  Local<Array> js_modes;
  for(i=0; i<monitor_count; i++){
    js_monitor = Object::New(v8::Isolate::GetCurrent());
    js_monitor->Set(JS_STR("is_primary"), JS_BOOL(monitors[i] == primary));
    js_monitor->Set(JS_STR("index"), JS_INT(i));
    
    js_monitor->Set(JS_STR("name"), JS_STR(glfwGetMonitorName(monitors[i])));
    
    glfwGetMonitorPos(monitors[i], &xpos, &ypos);
    js_monitor->Set(JS_STR("pos_x"), JS_INT(xpos));
    js_monitor->Set(JS_STR("pos_y"), JS_INT(ypos));
    
    glfwGetMonitorPhysicalSize(monitors[i], &width, &height);
    js_monitor->Set(JS_STR("width_mm"), JS_INT(width));
    js_monitor->Set(JS_STR("height_mm"), JS_INT(height));
    
    mode = glfwGetVideoMode(monitors[i]);
    js_monitor->Set(JS_STR("width"), JS_INT(mode->width));
    js_monitor->Set(JS_STR("height"), JS_INT(mode->height));
    js_monitor->Set(JS_STR("rate"), JS_INT(mode->refreshRate));
    
    modes = glfwGetVideoModes(monitors[i], &mode_count);
    js_modes = Array::New(v8::Isolate::GetCurrent(), mode_count);
    for(j=0; j<mode_count; j++){
      js_mode = Object::New(v8::Isolate::GetCurrent());
      js_mode->Set(JS_STR("width"), JS_INT(modes[j].width));
      js_mode->Set(JS_STR("height"), JS_INT(modes[j].height));
      js_mode->Set(JS_STR("rate"), JS_INT(modes[j].refreshRate));
      // NOTE: Are color bits necessary?
      js_modes->Set(JS_INT(j), js_mode);
    }
    js_monitor->Set(JS_STR("modes"), js_modes);
    
    js_monitors->Set(JS_INT(i), js_monitor);
  }
  
  SET_RETURN_VALUE(js_monitors);
}


/* @Module: Window handling */
Nan::Persistent<v8::Object> glfw_events;
int lastX=0,lastY=0;
bool windowCreated=false;

inline void make_depth_histogram(uint8_t rgb_image[],
    const uint16_t depth_image[], int width, int height)
{
  static uint32_t histogram[0x10000];
  memset(histogram, 0, sizeof(histogram));

  for (auto i = 0; i < width*height; ++i) ++histogram[depth_image[i]];
  // Build a cumulative histogram for the indices in [1,0xFFFF]
  for (auto i = 2; i < 0x10000; ++i) histogram[i] += histogram[i - 1];
  for (auto i = 0; i < width*height; ++i) {
    if (auto d = depth_image[i]) {
      // 0-255 based on histogram location
      int f = histogram[d] * 255 / histogram[0xFFFF];
      rgb_image[i * 3 + 0] = 255 - f;
      rgb_image[i * 3 + 1] = 0;
      rgb_image[i * 3 + 2] = f;
    } else {
      rgb_image[i * 3 + 0] = 20;
      rgb_image[i * 3 + 1] = 5;
      rgb_image[i * 3 + 2] = 0;
    }
  }
}

struct Rect {
  float x;
  float y;
  float w;
  float h;
  Rect(float x_val = 0, float y_val = 0, float w_val = 0, float h_val = 0) :
      x(x_val), y(y_val), w(w_val), h(h_val) {}
  // Create new rect within original boundaries with give aspect ration
  Rect adjust_ratio(float2 size) const
  {
      auto H = static_cast<float>(h), W = static_cast<float>(h) * size.x / size.y;
      if (W > w)
      {
          auto scale = w / W;
          W *= scale;
          H *= scale;
      }

      return{ x + (w - W) / 2, y + (h - H) / 2, W, H };
  }
};

static void _DrawImage2D(const Rect& r, const std::string& type,
                         const void* data, int width, int height,
                         float alpha = 1.0) {
  static GLuint texture = 0;

  // Upload
  if (!texture) {
    glGenTextures(1, &texture);
  }
  glBindTexture(GL_TEXTURE_2D, texture);

  if (type == "z16") {
    std::vector<uint8_t> rgb;
    rgb.resize(width * height * 4);
    make_depth_histogram(rgb.data(),
        reinterpret_cast<const uint16_t *>(data), width, height);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height,
        0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
  } else if (type == "rgb8") {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height,
        0, GL_RGB, GL_UNSIGNED_BYTE, data);
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  // Show
  glEnable(GL_BLEND);

  glBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
  glBegin(GL_QUADS);
  glColor4f(1.0f, 1.0f, 1.0f, 1 - alpha);
  glVertex2f(r.x, r.y);
  glVertex2f(r.x + r.w, r.y);
  glVertex2f(r.x + r.w, r.y + r.h);
  glVertex2f(r.x, r.y + r.h);
  glEnd();

  glBindTexture(GL_TEXTURE_2D, texture);
  glEnable(GL_TEXTURE_2D);
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0); glVertex2f(r.x, r.y);
  glTexCoord2f(1, 0); glVertex2f(r.x + r.w, r.y);
  glTexCoord2f(1, 1); glVertex2f(r.x + r.w, r.y + r.h);
  glTexCoord2f(0, 1); glVertex2f(r.x, r.y + r.h);
  glEnd();
  glDisable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);

  glDisable(GL_BLEND);
}

JS_METHOD(drawImage2D) {
  const int x = Nan::To<int32_t>(info[0]).FromJust();        // Viewport x
  const int y =  Nan::To<int32_t>(info[1]).FromJust();        // Viewport y
  const int width = Nan::To<uint32_t>(info[2]).FromJust();   // Viewport width
  const int height = Nan::To<uint32_t>(info[3]).FromJust();  // Viewport height

  Nan::Utf8String str(info[4]); // Buffer type
  std::string type = *str;
  Nan::TypedArrayContents<uint16_t> buffer(info[5].As<Uint16Array>());
  const void* data = *buffer; // Buffer pointer
  const int data_width = Nan::To<uint32_t>(info[6]).FromJust();  // Buffer width
  const int data_height = Nan::To<uint32_t>(info[7]).FromJust(); // Buffer height

  Rect r;
  r.x = x;
  r.y = y;
  r.w = width;
  r.h = height;

  glViewport(x, y, width, height);
  glClear(GL_COLOR_BUFFER_BIT);
  glPushMatrix();
  glOrtho(0, width, height, 0, -1, +1);

  _DrawImage2D(r, type, data, data_width, data_height);

  glPopMatrix();
}

//static GLenum Str2Format(const std::string& str) {
//  if (str == "z16") {
//    return GL_RGB;
//  } else if (str == "rgb8") {
//    return GL_RGB;
//  } else if (str == "y8") {
//    return GL_LUMINANCE;
//  }
//  return GL_LUMINANCE;
//}

void show(GLuint tex,
    const Rect& r) {
    if (!tex)
        return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUAD_STRIP);
    glTexCoord2f(0.f, 1.f); glVertex2f(r.x, r.y + r.h);
    glTexCoord2f(0.f, 0.f); glVertex2f(r.x, r.y);
    glTexCoord2f(1.f, 1.f); glVertex2f(r.x + r.w, r.y + r.h);
    glTexCoord2f(1.f, 0.f); glVertex2f(r.x + r.w, r.y);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    // draw_text(r.x + 15, r.y + 20, rs2_stream_to_string(stream));
}

void upload_texture(
    GLuint texture,
    uint8_t* data,
    uint32_t width,
    uint32_t height,
    const std::string& format) {
    static std::vector<uint8_t> rgb;
    // If the frame timestamp has changed
    //  since the last time show (...) was called, re-upload the texture

    glBindTexture(GL_TEXTURE_2D, texture);

    // glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);

    if (format == "z16") {
      rgb.resize(width * height * 4);
      make_depth_histogram(rgb.data(),
          reinterpret_cast<const uint16_t *>(data),
          width, height);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height,
          0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    } else if (format == "rgb8") {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height,
          0, GL_RGB, GL_UNSIGNED_BYTE, data);
    } else if (format == "y8") {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height,
          0, GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
    } else if (format == "raw8") {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height,
          0, GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
    } else {
      printf("Error: not supported color format in glfw: %s\n", format.c_str());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static state app_state = {0, 0, 0, 0, false, 0, 0};

void register_callbacks(GLFWwindow* window) {
  glfwSetWindowUserPointer(window, &app_state);
  glfwSetMouseButtonCallback(window,
      [](GLFWwindow * win, int button, int action, int /*mods*/) {
    auto s = (state *)glfwGetWindowUserPointer(win);
    if(button == GLFW_MOUSE_BUTTON_LEFT) s->ml = action == GLFW_PRESS;
  });
  glfwSetScrollCallback(window, [](GLFWwindow * win, double xoffset, double yoffset)
  {
      auto s = (state *)glfwGetWindowUserPointer(win);
      s->offset_x += static_cast<float>(xoffset);
      s->offset_y += static_cast<float>(yoffset);
  });
  glfwSetCursorPosCallback(window, [](GLFWwindow * win, double x, double y) {
    auto s = (state *)glfwGetWindowUserPointer(win);
    if(s->ml) {
      s->yaw -= (x - s->lastX);
      s->yaw = max(s->yaw, -120.0);
      s->yaw = min(s->yaw, +120.0);
      s->pitch += (y - s->lastY);
      s->pitch = max(s->pitch, -80.0);
      s->pitch = min(s->pitch, +80.0);
    }
    s->lastX = x;
    s->lastY = y;
  });
  glfwSetKeyCallback(window, [](GLFWwindow * win, int key, int scancode, int action, int mods)
  {
      auto s = (state *)glfwGetWindowUserPointer(win);

      // bool bext = false, bint = false, bloc = false;
      if (0 == action) //on key release
      {
          if (key == GLFW_KEY_SPACE)
          {
              s->yaw = s->pitch = 0; s->offset_x = s->offset_y = 0.0;
          }
      }
  });
}

JS_METHOD(drawDepthAndColorAsPointCloud) {
  size_t argIndex = 0;
  GLFWwindow* win =
      reinterpret_cast<GLFWwindow*>(Nan::To<int64_t>(info[argIndex++]).FromJust());

  Nan::TypedArrayContents<float> buffer0(info[argIndex++].As<Float32Array>());
  const float3* vertices = reinterpret_cast<float3*>(*buffer0);

  uint32_t point_count = Nan::To<int32_t>(info[argIndex++]).FromJust();

  Nan::TypedArrayContents<float> buffer1(info[argIndex++].As<Float32Array>());
  const float2* tex_coords = reinterpret_cast<float2*>(*buffer1);

  Nan::TypedArrayContents<uint8_t> buffer2(info[argIndex++].As<Uint8Array>());
  uint8_t* color = *buffer2;

  uint32_t color_width = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t color_height = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  Nan::Utf8String str0(info[argIndex++]);
  std::string color_format_str = *str0;

  static bool first = true;
  if (first) {
    first = false;
    register_callbacks(win);
  }
  static GLuint tex = 0;
  if (!tex)
    glGenTextures(1, &tex);

  if (color)
    upload_texture(tex, color, color_width, color_height, color_format_str);
  glPopMatrix();
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  int32_t winW, winH;
  float width, height;
  glfwGetWindowSize(win, &winW, &winH);
  width = float(winW);
  height = float(winH);
  glClearColor(52.0f / 255, 72.f / 255, 94.0f / 255, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  gluPerspective(60, width / height, 0.01f, 10.0f);

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  gluLookAt(0, 0, 0, 0, 0, 1, 0, -1, 0);

  glTranslatef(0, 0, +0.5f + app_state.offset_y*0.05f);
  glRotated(app_state.pitch, 1, 0, 0);
  glRotated(app_state.yaw, 0, 1, 0);
  glTranslatef(0, 0, -0.5f);

  glPointSize(width / 640);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, tex);
  float tex_border_color[] = { 0.8f, 0.8f, 0.8f, 0.8f };
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, tex_border_color);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); // GL_CLAMP_TO_EDGE
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F); // GL_CLAMP_TO_EDGE
  glBegin(GL_POINTS);


  /* this segment actually prints the pointcloud */
  for (uint32_t i = 0; i < point_count; i++)
  {
      if (vertices[i].z)
      {
          // upload the point and texture coordinates only for points we have depth data for
          glVertex3f(vertices[i].x, vertices[i].y, vertices[i].z);
          glTexCoord2f(tex_coords[i].x, tex_coords[i].y);
      }
  }

  // OpenGL cleanup
  glEnd();
  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glPopAttrib();
  glPushMatrix();
}


Nan::Callback* global_js_key_callback = nullptr;

static void global_key_func(GLFWwindow *, int key,
    int scancode, int action, int mods) {
  if (global_js_key_callback) {
    v8::Local<v8::Value> argv[4] = {
        Nan::New(key), Nan::New(scancode), Nan::New(action), Nan::New(mods)};
    Nan::Call(*global_js_key_callback, 4, argv);
  }
}

JS_METHOD(setKeyCallback) {
  GLFWwindow* win = reinterpret_cast<GLFWwindow*>(Nan::To<int32_t>(info[0]).FromJust());
  global_js_key_callback = new Nan::Callback(info[1].As<v8::Function>());
  glfwSetKeyCallback(win, global_key_func);
}

JS_METHOD(draw2x2Streams) {
  size_t argIndex = 0;
  GLFWwindow* win =
    reinterpret_cast<GLFWwindow*>(Nan::To<int32_t>(info[argIndex++]).FromJust());
  int32_t winW = 0;
  int32_t winH = 0;
  glfwGetWindowSize(win, &winW, &winH);

  uint32_t channel_count = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  float width_divid_factor = 1.0f;
  float height_divid_factor = 1.0f;
  if (channel_count == 2) {
    width_divid_factor = 2.0f;
    height_divid_factor = 1.0f;
  } else if (channel_count > 2) {
    width_divid_factor = 2.0f;
    height_divid_factor = 2.0f;
  }

  Nan::TypedArrayContents<uint8_t> buffer0(info[argIndex++].As<Uint8Array>());
  const void* data0 = *buffer0;
  Nan::Utf8String str0(info[argIndex++]);
  std::string type0 = *str0;
  uint32_t width0 = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t height0 = Nan::To<uint32_t>(info[argIndex++]).FromJust();

  Nan::TypedArrayContents<uint8_t> buffer1(info[argIndex++].As<Uint8Array>());
  const void* data1 = *buffer1;
  Nan::Utf8String str1(info[argIndex++]);
  std::string type1 = *str1;
  uint32_t width1 = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t height1 = Nan::To<uint32_t>(info[argIndex++]).FromJust();

  Nan::TypedArrayContents<uint8_t> buffer2(info[argIndex++].As<Uint8Array>());
  const void* data2 = *buffer2;
  Nan::Utf8String str2(info[argIndex++]);
  std::string type2 = *str2;
  uint32_t width2 = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t height2 = Nan::To<uint32_t>(info[argIndex++]).FromJust();

  Nan::TypedArrayContents<uint8_t> buffer3(info[argIndex++].As<Uint8Array>());
  const void* data3 = *buffer3;
  Nan::Utf8String str3(info[argIndex++]);
  std::string type3 = *str3;
  uint32_t width3 = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t height3 = Nan::To<uint32_t>(info[argIndex++]).FromJust();

  glPixelZoom(1, -1);

  // X _
  // _ _
  //
  // if (data0) {
  //   glRasterPos2f(-1, 1);
  //   auto format = Str2Format(type0);
  //   glDrawPixels(width0, height0, format, GL_UNSIGNED_BYTE, data0);
  // }
  static GLuint tex = 0;
  if (!tex)
    glGenTextures(1, &tex);

  if (data0) {
    upload_texture(tex, (uint8_t*)data0, width0, height0, type0);
    Rect rect = { 0, 0, winW/width_divid_factor, winH/height_divid_factor };
    show(tex, rect.adjust_ratio({float(width0), float(height0)}));
  }

  // _ X
  // _ _
  //
  // Display color image as RGB triples
  if (data1) {
    upload_texture(tex, (uint8_t*)data1, width1, height1, type1);
    Rect rect = { winW/width_divid_factor, 0, winW/width_divid_factor, winH/height_divid_factor };
    show(tex, rect.adjust_ratio({float(width1), float(height1)}));
  }

  // _ _
  // X _
  //
  // Display infrared image by mapping IR intensity to visible luminance
  if (data2) {
    upload_texture(tex, (uint8_t*)data2, width2, height2, type2);
    Rect rect = { 0, winH/height_divid_factor, winW/width_divid_factor, winH/height_divid_factor};
    show(tex, rect.adjust_ratio({float(width2), float(height2)}));
  }

  // _ _
  // _ X
  //
  // Display second infrared image by mapping IR intensity to visible luminance
  if (data3) {
    upload_texture(tex, (uint8_t*)data3, width3, height3, type3);
    Rect rect = { winW/width_divid_factor, winH/height_divid_factor, winW/width_divid_factor, winH/height_divid_factor};
    show(tex, rect.adjust_ratio({float(width3), float(height3)}));
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(testScene) {
  int width = Nan::To<uint32_t>(info[0]).FromJust();
  int height = Nan::To<uint32_t>(info[1]).FromJust();
  float ratio = width / (float) height;

  glViewport(0, 0, width, height);
  glClear(GL_COLOR_BUFFER_BIT);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-ratio, ratio, -1.f, 1.f, 1.f, -1.f);
  glMatrixMode(GL_MODELVIEW);

  glLoadIdentity();
  glRotatef((float) glfwGetTime() * 50.f, 0.f, 0.f, 1.f);

  glBegin(GL_TRIANGLES);
  glColor3f(1.f, 0.f, 0.f);
  glVertex3f(-0.6f, -0.4f, 0.f);
  glColor3f(0.f, 1.f, 0.f);
  glVertex3f(0.6f, -0.4f, 0.f);
  glColor3f(0.f, 0.f, 1.f);
  glVertex3f(0.f, 0.6f, 0.f);
  glEnd();

  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(WindowHint) {
  int target       = Nan::To<uint32_t>(info[0]).FromJust();
  int hint         = Nan::To<uint32_t>(info[1]).FromJust();
  glfwWindowHint(target, hint);
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(DefaultWindowHints) {
  glfwDefaultWindowHints();
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(CreateGLFWWindow) {
  int width       = Nan::To<uint32_t>(info[0]).FromJust();
  int height      = Nan::To<uint32_t>(info[1]).FromJust();
  Nan::Utf8String str(info[2]);
  int monitor_idx = Nan::To<uint32_t>(info[3]).FromJust();
  
  GLFWwindow* window = NULL;
  GLFWmonitor **monitors = NULL, *monitor = NULL;
  int monitor_count;
  if(info.Length() >= 4 && monitor_idx >= 0){
    monitors = glfwGetMonitors(&monitor_count);
    if(monitor_idx >= monitor_count){
      return ThrowError("Invalid monitor");
    }
    monitor = monitors[monitor_idx];
  }

  if(!windowCreated) {
    window = glfwCreateWindow(width, height, *str, monitor, NULL);
    if(!window) {
      // can't create window, throw error
      return ThrowError("Can't create GLFW window");
    }

    glfwMakeContextCurrent(window);

    GLenum err = glewInit();
    if (err)
    {
      /* Problem: glewInit failed, something is seriously wrong. */
      string msg="Can't init GLEW (glew error ";
      msg+=(const char*) glewGetErrorString(err);
      msg+=")";

      fprintf(stderr, "%s", msg.c_str());
      return ThrowError(msg.c_str());
    }
    // fprintf(stdout, "Status: Using GLEW %s\n", glewGetString(GLEW_VERSION));
  }
  else
    glfwSetWindowSize(window, width,height);

  // Set callback functions
  // glfw_events.Reset(info.This()->Get(JS_STR("events"))->ToObject());
  glfw_events.Reset(Nan::To<v8::Object>(info.This()->Get(JS_STR("events"))).ToLocalChecked());

  SET_RETURN_VALUE(JS_NUM((uint64_t) window));
}

JS_METHOD(DestroyWindow) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwDestroyWindow(window);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(Ortho) {
  int32_t val0=Nan::To<int32_t>(info[0]).FromJust();
  int32_t val1=Nan::To<int32_t>(info[1]).FromJust();
  int32_t val2=Nan::To<int32_t>(info[2]).FromJust();
  int32_t val3=Nan::To<int32_t>(info[3]).FromJust();
  int32_t val4=Nan::To<int32_t>(info[4]).FromJust();
  int32_t val5=Nan::To<int32_t>(info[5]).FromJust();
  glOrtho(val0, val1, val2, val3, val4, val5);
}

JS_METHOD(PushMatrix) {
  glPushMatrix();
}

JS_METHOD(PopMatrix) {
  glPopMatrix();
}

JS_METHOD(ClearColorBuffer) {
  glClear(GL_COLOR_BUFFER_BIT);
}


JS_METHOD(SetWindowTitle) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  Nan::Utf8String str(info[1]);
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwSetWindowTitle(window, *str);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(GetWindowSize) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    int w,h;
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwGetWindowSize(window, &w, &h);
    Local<Array> arr=Array::New(v8::Isolate::GetCurrent(), 2);
    arr->Set(JS_STR("width"),JS_INT(w));
    arr->Set(JS_STR("height"),JS_INT(h));
    SET_RETURN_VALUE(arr);
    return;
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(SetWindowSize) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwSetWindowSize(window, Nan::To<uint32_t>(info[1]).FromJust(), Nan::To<uint32_t>(info[2]).FromJust());
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(SetWindowPos) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwSetWindowPos(window, Nan::To<uint32_t>(info[1]).FromJust(), Nan::To<uint32_t>(info[2]).FromJust());
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(GetWindowPos) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    int xpos, ypos;
    glfwGetWindowPos(window, &xpos, &ypos);
    Local<Array> arr=Array::New(v8::Isolate::GetCurrent(), 2);
    arr->Set(JS_STR("xpos"),JS_INT(xpos));
    arr->Set(JS_STR("ypos"),JS_INT(ypos));
    SET_RETURN_VALUE(arr);
    return;
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(GetFramebufferSize) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    Local<Array> arr=Array::New(v8::Isolate::GetCurrent(), 2);
    arr->Set(JS_STR("width"),JS_INT(width));
    arr->Set(JS_STR("height"),JS_INT(height));
    SET_RETURN_VALUE(arr);
    return;
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(IconifyWindow) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwIconifyWindow(window);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(RestoreWindow) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwRestoreWindow(window);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(HideWindow) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwHideWindow(window);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(ShowWindow) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwShowWindow(window);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(WindowShouldClose) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    SET_RETURN_VALUE(JS_INT(glfwWindowShouldClose(window)));
    return;
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(SetWindowShouldClose) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  int value=Nan::To<uint32_t>(info[1]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwSetWindowShouldClose(window, value);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(GetWindowAttrib) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  int attrib=Nan::To<uint32_t>(info[1]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    SET_RETURN_VALUE(JS_INT(glfwGetWindowAttrib(window, attrib)));
    return;
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(PollEvents) {
  glfwPollEvents();
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(WaitEvents) {
  glfwWaitEvents();
  SET_RETURN_VALUE(Nan::Undefined());
}

//GLFWAPI void GLFWAPIENTRY glfwSetWindowSizeCallback( GLFWwindowsizefun cbfun );
//GLFWAPI void GLFWAPIENTRY glfwSetWindowCloseCallback( GLFWwindowclosefun cbfun );
//GLFWAPI void GLFWAPIENTRY glfwSetWindowRefreshCallback( GLFWwindowrefreshfun cbfun );

/* Input handling */

JS_METHOD(GetKey) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  int key=Nan::To<uint32_t>(info[1]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    SET_RETURN_VALUE(JS_INT(glfwGetKey(window, key)));
    return;
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(GetMouseButton) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  int button=Nan::To<uint32_t>(info[1]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    SET_RETURN_VALUE(JS_INT(glfwGetMouseButton(window, button)));
    return;
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(GetCursorPos) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();;
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    double x,y;
    glfwGetCursorPos(window, &x, &y);
    Local<Array> arr=Array::New(v8::Isolate::GetCurrent(), 2);
    arr->Set(JS_STR("x"),JS_INT(x));
    arr->Set(JS_STR("y"),JS_INT(y));
    SET_RETURN_VALUE(arr);
    return;
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(SetCursorPos) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  int x=Nan::To<int32_t>(info[1]).FromJust();
  int y=Nan::To<uint32_t>(info[2]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwSetCursorPos(window, x, y);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

/* @Module Context handling */
JS_METHOD(MakeContextCurrent) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwMakeContextCurrent(window);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(GetCurrentContext) {
  GLFWwindow* window=glfwGetCurrentContext();
  SET_RETURN_VALUE(JS_NUM((uint64_t) window));
}

JS_METHOD(SwapBuffers) {
  uint64_t handle=Nan::To<int64_t>(info[0]).FromJust();
  if(handle) {
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(handle);
    glfwSwapBuffers(window);
  }
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(SwapInterval) {
  int interval=Nan::To<int32_t>(info[0]).FromJust();
  glfwSwapInterval(interval);
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(ExtensionSupported) {
  Nan::Utf8String str(info[0]);
  SET_RETURN_VALUE(JS_BOOL(glfwExtensionSupported(*str)==1));
}

JS_METHOD(uploadAsTexture) {
  size_t argIndex = 0;
  GLuint tex =
      static_cast<GLuint>(Nan::To<int64_t>(info[argIndex++]).FromJust());
  Nan::TypedArrayContents<uint8_t> buffer0(info[argIndex++].As<Uint8Array>());
  uint8_t* buffer = reinterpret_cast<uint8_t*>(*buffer0);

  uint32_t width = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t height = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  Nan::Utf8String str0(info[argIndex++]);
  std::string format_str = *str0;

  if (buffer)
    upload_texture(tex, buffer, width, height, format_str);
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(showInRect) {
  size_t argIndex = 0;
  GLuint tex =
      static_cast<GLuint>(Nan::To<int64_t>(info[argIndex++]).FromJust());
  uint32_t x = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t y = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t w = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  uint32_t h = Nan::To<uint32_t>(info[argIndex++]).FromJust();
  show(tex, Rect(x, y, w, h));
  SET_RETURN_VALUE(Nan::Undefined());
}

JS_METHOD(genTexture) {
  GLuint tex = 0;
  glGenTextures(1, &tex);
  SET_RETURN_VALUE(JS_NUM(tex));
}

// make sure we close everything when we exit
void AtExit() {
  glfwTerminate();
}

} // namespace glfw

///////////////////////////////////////////////////////////////////////////////
//
// bindings
//
///////////////////////////////////////////////////////////////////////////////
#define JS_GLFW_CONSTANT(name) target->Set(JS_STR( #name ), JS_INT(GLFW_ ## name))
#define JS_GLFW_SET_METHOD(name) Nan::SetMethod(target, #name , glfw::name);

extern "C" {
void init(Local<Object> target) {
  atexit(glfw::AtExit);

  Nan::HandleScope scope;

  /* GLFW initialization, termination and version querying */
  JS_GLFW_SET_METHOD(Init);
  JS_GLFW_SET_METHOD(Terminate);
  JS_GLFW_SET_METHOD(GetVersion);
  JS_GLFW_SET_METHOD(GetVersionString);

  /* Time */
  JS_GLFW_SET_METHOD(GetTime);
  JS_GLFW_SET_METHOD(SetTime);
  
  /* Monitor handling */
  JS_GLFW_SET_METHOD(GetMonitors);

  /* Window handling */
  JS_GLFW_SET_METHOD(CreateGLFWWindow);
  JS_GLFW_SET_METHOD(WindowHint);
  JS_GLFW_SET_METHOD(DefaultWindowHints);
  JS_GLFW_SET_METHOD(DestroyWindow);
  JS_GLFW_SET_METHOD(SetWindowShouldClose);
  JS_GLFW_SET_METHOD(WindowShouldClose);
  JS_GLFW_SET_METHOD(SetWindowTitle);
  JS_GLFW_SET_METHOD(GetWindowSize);
  JS_GLFW_SET_METHOD(SetWindowSize);
  JS_GLFW_SET_METHOD(SetWindowPos);
  JS_GLFW_SET_METHOD(GetWindowPos);
  JS_GLFW_SET_METHOD(GetFramebufferSize);
  JS_GLFW_SET_METHOD(IconifyWindow);
  JS_GLFW_SET_METHOD(RestoreWindow);
  JS_GLFW_SET_METHOD(ShowWindow);
  JS_GLFW_SET_METHOD(HideWindow);
  JS_GLFW_SET_METHOD(GetWindowAttrib);
  JS_GLFW_SET_METHOD(PollEvents);
  JS_GLFW_SET_METHOD(WaitEvents);
  JS_GLFW_SET_METHOD(Ortho);
  JS_GLFW_SET_METHOD(PushMatrix);
  JS_GLFW_SET_METHOD(PopMatrix);
  JS_GLFW_SET_METHOD(ClearColorBuffer);

  /* Input handling */
  JS_GLFW_SET_METHOD(GetKey);
  JS_GLFW_SET_METHOD(GetMouseButton);
  JS_GLFW_SET_METHOD(GetCursorPos);
  JS_GLFW_SET_METHOD(SetCursorPos);

  /* Context handling */
  JS_GLFW_SET_METHOD(MakeContextCurrent);
  JS_GLFW_SET_METHOD(GetCurrentContext);
  JS_GLFW_SET_METHOD(SwapBuffers);
  JS_GLFW_SET_METHOD(SwapInterval);
  JS_GLFW_SET_METHOD(ExtensionSupported);

  /*************************************************************************
   * GLFW version
   *************************************************************************/

  JS_GLFW_CONSTANT(VERSION_MAJOR);
  JS_GLFW_CONSTANT(VERSION_MINOR);
  JS_GLFW_CONSTANT(VERSION_REVISION);


  /*************************************************************************
   * Input handling definitions
   *************************************************************************/

  /* Key and button state/action definitions */
  JS_GLFW_CONSTANT(RELEASE);
  JS_GLFW_CONSTANT(PRESS);
  JS_GLFW_CONSTANT(REPEAT);

  /* These key codes are inspired by the *USB HID Usage Tables v1.12* (p. 53-60),
   * but re-arranged to map to 7-bit ASCII for printable keys (function keys are
   * put in the 256+ range).
   *
   * The naming of the key codes follow these rules:
   *  - The US keyboard layout is used
   *  - Names of printable alpha-numeric characters are used (e.g. "A", "R",
   *    "3", etc.)
   *  - For non-alphanumeric characters, Unicode:ish names are used (e.g.
   *    "COMMA", "LEFT_SQUARE_BRACKET", etc.). Note that some names do not
   *    correspond to the Unicode standard (usually for brevity)
   *  - Keys that lack a clear US mapping are named "WORLD_x"
   *  - For non-printable keys, custom names are used (e.g. "F4",
   *    "BACKSPACE", etc.)
   */

  /* The unknown key */
  JS_GLFW_CONSTANT(KEY_UNKNOWN);

  /* Printable keys */
  JS_GLFW_CONSTANT(KEY_SPACE);
  JS_GLFW_CONSTANT(KEY_APOSTROPHE);
  JS_GLFW_CONSTANT(KEY_COMMA);
  JS_GLFW_CONSTANT(KEY_MINUS);
  JS_GLFW_CONSTANT(KEY_PERIOD);
  JS_GLFW_CONSTANT(KEY_SLASH);
  JS_GLFW_CONSTANT(KEY_0);
  JS_GLFW_CONSTANT(KEY_1);
  JS_GLFW_CONSTANT(KEY_2);
  JS_GLFW_CONSTANT(KEY_3);
  JS_GLFW_CONSTANT(KEY_4);
  JS_GLFW_CONSTANT(KEY_5);
  JS_GLFW_CONSTANT(KEY_6);
  JS_GLFW_CONSTANT(KEY_7);
  JS_GLFW_CONSTANT(KEY_8);
  JS_GLFW_CONSTANT(KEY_9);
  JS_GLFW_CONSTANT(KEY_SEMICOLON);
  JS_GLFW_CONSTANT(KEY_EQUAL);
  JS_GLFW_CONSTANT(KEY_A);
  JS_GLFW_CONSTANT(KEY_B);
  JS_GLFW_CONSTANT(KEY_C);
  JS_GLFW_CONSTANT(KEY_D);
  JS_GLFW_CONSTANT(KEY_E);
  JS_GLFW_CONSTANT(KEY_F);
  JS_GLFW_CONSTANT(KEY_G);
  JS_GLFW_CONSTANT(KEY_H);
  JS_GLFW_CONSTANT(KEY_I);
  JS_GLFW_CONSTANT(KEY_J);
  JS_GLFW_CONSTANT(KEY_K);
  JS_GLFW_CONSTANT(KEY_L);
  JS_GLFW_CONSTANT(KEY_M);
  JS_GLFW_CONSTANT(KEY_N);
  JS_GLFW_CONSTANT(KEY_O);
  JS_GLFW_CONSTANT(KEY_P);
  JS_GLFW_CONSTANT(KEY_Q);
  JS_GLFW_CONSTANT(KEY_R);
  JS_GLFW_CONSTANT(KEY_S);
  JS_GLFW_CONSTANT(KEY_T);
  JS_GLFW_CONSTANT(KEY_U);
  JS_GLFW_CONSTANT(KEY_V);
  JS_GLFW_CONSTANT(KEY_W);
  JS_GLFW_CONSTANT(KEY_X);
  JS_GLFW_CONSTANT(KEY_Y);
  JS_GLFW_CONSTANT(KEY_Z);
  JS_GLFW_CONSTANT(KEY_LEFT_BRACKET);
  JS_GLFW_CONSTANT(KEY_BACKSLASH);
  JS_GLFW_CONSTANT(KEY_RIGHT_BRACKET);
  JS_GLFW_CONSTANT(KEY_GRAVE_ACCENT);
  JS_GLFW_CONSTANT(KEY_WORLD_1);
  JS_GLFW_CONSTANT(KEY_WORLD_2);

  /* Function keys */
  JS_GLFW_CONSTANT(KEY_ESCAPE);
  JS_GLFW_CONSTANT(KEY_ENTER);
  JS_GLFW_CONSTANT(KEY_TAB);
  JS_GLFW_CONSTANT(KEY_BACKSPACE);
  JS_GLFW_CONSTANT(KEY_INSERT);
  JS_GLFW_CONSTANT(KEY_DELETE);
  JS_GLFW_CONSTANT(KEY_RIGHT);
  JS_GLFW_CONSTANT(KEY_LEFT);
  JS_GLFW_CONSTANT(KEY_DOWN);
  JS_GLFW_CONSTANT(KEY_UP);
  JS_GLFW_CONSTANT(KEY_PAGE_UP);
  JS_GLFW_CONSTANT(KEY_PAGE_DOWN);
  JS_GLFW_CONSTANT(KEY_HOME);
  JS_GLFW_CONSTANT(KEY_END);
  JS_GLFW_CONSTANT(KEY_CAPS_LOCK);
  JS_GLFW_CONSTANT(KEY_SCROLL_LOCK);
  JS_GLFW_CONSTANT(KEY_NUM_LOCK);
  JS_GLFW_CONSTANT(KEY_PRINT_SCREEN);
  JS_GLFW_CONSTANT(KEY_PAUSE);
  JS_GLFW_CONSTANT(KEY_F1);
  JS_GLFW_CONSTANT(KEY_F2);
  JS_GLFW_CONSTANT(KEY_F3);
  JS_GLFW_CONSTANT(KEY_F4);
  JS_GLFW_CONSTANT(KEY_F5);
  JS_GLFW_CONSTANT(KEY_F6);
  JS_GLFW_CONSTANT(KEY_F7);
  JS_GLFW_CONSTANT(KEY_F8);
  JS_GLFW_CONSTANT(KEY_F9);
  JS_GLFW_CONSTANT(KEY_F10);
  JS_GLFW_CONSTANT(KEY_F11);
  JS_GLFW_CONSTANT(KEY_F12);
  JS_GLFW_CONSTANT(KEY_F13);
  JS_GLFW_CONSTANT(KEY_F14);
  JS_GLFW_CONSTANT(KEY_F15);
  JS_GLFW_CONSTANT(KEY_F16);
  JS_GLFW_CONSTANT(KEY_F17);
  JS_GLFW_CONSTANT(KEY_F18);
  JS_GLFW_CONSTANT(KEY_F19);
  JS_GLFW_CONSTANT(KEY_F20);
  JS_GLFW_CONSTANT(KEY_F21);
  JS_GLFW_CONSTANT(KEY_F22);
  JS_GLFW_CONSTANT(KEY_F23);
  JS_GLFW_CONSTANT(KEY_F24);
  JS_GLFW_CONSTANT(KEY_F25);
  JS_GLFW_CONSTANT(KEY_KP_0);
  JS_GLFW_CONSTANT(KEY_KP_1);
  JS_GLFW_CONSTANT(KEY_KP_2);
  JS_GLFW_CONSTANT(KEY_KP_3);
  JS_GLFW_CONSTANT(KEY_KP_4);
  JS_GLFW_CONSTANT(KEY_KP_5);
  JS_GLFW_CONSTANT(KEY_KP_6);
  JS_GLFW_CONSTANT(KEY_KP_7);
  JS_GLFW_CONSTANT(KEY_KP_8);
  JS_GLFW_CONSTANT(KEY_KP_9);
  JS_GLFW_CONSTANT(KEY_KP_DECIMAL);
  JS_GLFW_CONSTANT(KEY_KP_DIVIDE);
  JS_GLFW_CONSTANT(KEY_KP_MULTIPLY);
  JS_GLFW_CONSTANT(KEY_KP_SUBTRACT);
  JS_GLFW_CONSTANT(KEY_KP_ADD);
  JS_GLFW_CONSTANT(KEY_KP_ENTER);
  JS_GLFW_CONSTANT(KEY_KP_EQUAL);
  JS_GLFW_CONSTANT(KEY_LEFT_SHIFT);
  JS_GLFW_CONSTANT(KEY_LEFT_CONTROL);
  JS_GLFW_CONSTANT(KEY_LEFT_ALT);
  JS_GLFW_CONSTANT(KEY_LEFT_SUPER);
  JS_GLFW_CONSTANT(KEY_RIGHT_SHIFT);
  JS_GLFW_CONSTANT(KEY_RIGHT_CONTROL);
  JS_GLFW_CONSTANT(KEY_RIGHT_ALT);
  JS_GLFW_CONSTANT(KEY_RIGHT_SUPER);
  JS_GLFW_CONSTANT(KEY_MENU);
  JS_GLFW_CONSTANT(KEY_LAST);

  /*Modifier key flags*/

  /*If this bit is set one or more Shift keys were held down. */
  JS_GLFW_CONSTANT(MOD_SHIFT);
  /*If this bit is set one or more Control keys were held down. */
  JS_GLFW_CONSTANT(MOD_CONTROL);
  /*If this bit is set one or more Alt keys were held down. */
  JS_GLFW_CONSTANT(MOD_ALT);
  /*If this bit is set one or more Super keys were held down. */
  JS_GLFW_CONSTANT(MOD_SUPER);

  /*Mouse buttons*/
  JS_GLFW_CONSTANT(MOUSE_BUTTON_1);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_2);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_3);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_4);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_5);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_6);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_7);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_8);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_LAST);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_LEFT);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_RIGHT);
  JS_GLFW_CONSTANT(MOUSE_BUTTON_MIDDLE);

  /*Joysticks*/
  JS_GLFW_CONSTANT(JOYSTICK_1);
  JS_GLFW_CONSTANT(JOYSTICK_2);
  JS_GLFW_CONSTANT(JOYSTICK_3);
  JS_GLFW_CONSTANT(JOYSTICK_4);
  JS_GLFW_CONSTANT(JOYSTICK_5);
  JS_GLFW_CONSTANT(JOYSTICK_6);
  JS_GLFW_CONSTANT(JOYSTICK_7);
  JS_GLFW_CONSTANT(JOYSTICK_8);
  JS_GLFW_CONSTANT(JOYSTICK_9);
  JS_GLFW_CONSTANT(JOYSTICK_10);
  JS_GLFW_CONSTANT(JOYSTICK_11);
  JS_GLFW_CONSTANT(JOYSTICK_12);
  JS_GLFW_CONSTANT(JOYSTICK_13);
  JS_GLFW_CONSTANT(JOYSTICK_14);
  JS_GLFW_CONSTANT(JOYSTICK_15);
  JS_GLFW_CONSTANT(JOYSTICK_16);
  JS_GLFW_CONSTANT(JOYSTICK_LAST);

  /*errors Error codes*/

  /*GLFW has not been initialized.*/
  JS_GLFW_CONSTANT(NOT_INITIALIZED);
  /*No context is current for this thread.*/
  JS_GLFW_CONSTANT(NO_CURRENT_CONTEXT);
  /*One of the enum parameters for the function was given an invalid enum.*/
  JS_GLFW_CONSTANT(INVALID_ENUM);
  /*One of the parameters for the function was given an invalid value.*/
  JS_GLFW_CONSTANT(INVALID_VALUE);
  /*A memory allocation failed.*/
  JS_GLFW_CONSTANT(OUT_OF_MEMORY);
  /*GLFW could not find support for the requested client API on the system.*/
  JS_GLFW_CONSTANT(API_UNAVAILABLE);
  /*The requested client API version is not available.*/
  JS_GLFW_CONSTANT(VERSION_UNAVAILABLE);
  /*A platform-specific error occurred that does not match any of the more specific categories.*/
  JS_GLFW_CONSTANT(PLATFORM_ERROR);
  /*The clipboard did not contain data in the requested format.*/
  JS_GLFW_CONSTANT(FORMAT_UNAVAILABLE);

  JS_GLFW_CONSTANT(FOCUSED);
  JS_GLFW_CONSTANT(ICONIFIED);
  JS_GLFW_CONSTANT(RESIZABLE);
  JS_GLFW_CONSTANT(VISIBLE);
  JS_GLFW_CONSTANT(DECORATED);

  JS_GLFW_CONSTANT(RED_BITS);
  JS_GLFW_CONSTANT(GREEN_BITS);
  JS_GLFW_CONSTANT(BLUE_BITS);
  JS_GLFW_CONSTANT(ALPHA_BITS);
  JS_GLFW_CONSTANT(DEPTH_BITS);
  JS_GLFW_CONSTANT(STENCIL_BITS);
  JS_GLFW_CONSTANT(ACCUM_RED_BITS);
  JS_GLFW_CONSTANT(ACCUM_GREEN_BITS);
  JS_GLFW_CONSTANT(ACCUM_BLUE_BITS);
  JS_GLFW_CONSTANT(ACCUM_ALPHA_BITS);
  JS_GLFW_CONSTANT(AUX_BUFFERS);
  JS_GLFW_CONSTANT(STEREO);
  JS_GLFW_CONSTANT(SAMPLES);
  JS_GLFW_CONSTANT(SRGB_CAPABLE);
  JS_GLFW_CONSTANT(REFRESH_RATE);

  JS_GLFW_CONSTANT(CLIENT_API);
  JS_GLFW_CONSTANT(CONTEXT_VERSION_MAJOR);
  JS_GLFW_CONSTANT(CONTEXT_VERSION_MINOR);
  JS_GLFW_CONSTANT(CONTEXT_REVISION);
  JS_GLFW_CONSTANT(CONTEXT_ROBUSTNESS);
  JS_GLFW_CONSTANT(OPENGL_FORWARD_COMPAT);
  JS_GLFW_CONSTANT(OPENGL_DEBUG_CONTEXT);
  JS_GLFW_CONSTANT(OPENGL_PROFILE);

  JS_GLFW_CONSTANT(OPENGL_API);
  JS_GLFW_CONSTANT(OPENGL_ES_API);

  JS_GLFW_CONSTANT(NO_ROBUSTNESS);
  JS_GLFW_CONSTANT(NO_RESET_NOTIFICATION);
  JS_GLFW_CONSTANT(LOSE_CONTEXT_ON_RESET);

  JS_GLFW_CONSTANT(OPENGL_ANY_PROFILE);
  JS_GLFW_CONSTANT(OPENGL_CORE_PROFILE);
  JS_GLFW_CONSTANT(OPENGL_COMPAT_PROFILE);

  JS_GLFW_CONSTANT(CURSOR);
  JS_GLFW_CONSTANT(STICKY_KEYS);
  JS_GLFW_CONSTANT(STICKY_MOUSE_BUTTONS);

  JS_GLFW_CONSTANT(CURSOR_NORMAL);
  JS_GLFW_CONSTANT(CURSOR_HIDDEN);
  JS_GLFW_CONSTANT(CURSOR_DISABLED);

  JS_GLFW_CONSTANT(CONNECTED);
  JS_GLFW_CONSTANT(DISCONNECTED);

  JS_GLFW_SET_METHOD(testScene);
  JS_GLFW_SET_METHOD(drawImage2D);
  JS_GLFW_SET_METHOD(draw2x2Streams);
  JS_GLFW_SET_METHOD(drawDepthAndColorAsPointCloud);
  JS_GLFW_SET_METHOD(setKeyCallback);
  JS_GLFW_SET_METHOD(uploadAsTexture);
  JS_GLFW_SET_METHOD(showInRect);
  JS_GLFW_SET_METHOD(genTexture);
}

NODE_MODULE(glfw, init)
}

