#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h> // for usleep
#include <vector>

#include "command_socket.hpp"
#include "glasses.hpp"
#include "viture.h"

struct Framebuffer {
  XImage *img;
  int width, height;
  GLuint tex;
};

struct MyMonitor {
  int x, y, width, height;
  int index;
};

float screen_angle_offset_degrees = 0.0f;
bool center_dot_enabled = true;

Display *dpy;
Window root;
Window win;
GLXContext glc;
std::vector<MyMonitor> monitors;
std::vector<MyMonitor *> focusedmonitors;

void grabMonitor(MyMonitor &m);

Framebuffer framebuffer{};

XShmSegmentInfo shmInfo;

void initShm(Display *dpy, Framebuffer &fb);

void cleanupShm(Display *dpy);

void grabFramebuffer(Framebuffer &fb);
void uploadFramebufferTexture(Framebuffer &fb);

void getMonitorUVs(const MyMonitor &m, const Framebuffer &fb, float &u0,
                   float &v0, float &u1, float &v1);

bool initGL();
void cleanup();
void grabMonitor(MyMonitor &m);
void uploadTexture(MyMonitor &m);
void render();

void draw_filled_center_rect(float half_width, float half_height) {
  int viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  int screenWidth = viewport[2];
  int screenHeight = viewport[3];

  float centerX = screenWidth / 2.0f;
  float centerY = screenHeight / 2.0f;

  // Switch to orthographic projection
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  {
    glLoadIdentity();
    glOrtho(0, screenWidth, 0, screenHeight, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    {
      glPushMatrix();
      glLoadIdentity();

      glDisable(GL_LIGHTING);
      glDisable(GL_TEXTURE_2D);
      glDisable(GL_DEPTH_TEST);

      glColor3f(1.0f, 0.0f, 0.0f); // Red

      glBegin(GL_QUADS);
      glVertex2f(centerX - half_width, centerY - half_height);
      glVertex2f(centerX + half_width, centerY - half_height);
      glVertex2f(centerX + half_width, centerY + half_height);
      glVertex2f(centerX - half_width, centerY + half_height);
      glEnd();

      glEnable(GL_DEPTH_TEST);
    }
    glPopMatrix(); // Modelview
    glMatrixMode(GL_PROJECTION);
  }
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glColor3f(1.0f, 1.0f, 1.0f);
  glEnable(GL_TEXTURE_2D);
  // glEnable(GL_LIGHTING);
}

void on_align() {
  glasses.oroll = -glasses.roll;
  glasses.opitch = -glasses.pitch;
  glasses.oyaw = -glasses.yaw;
}

void on_push() {
  for (int i = int(focusedmonitors.size()) - 1; i >= 0; i--) {
    if (i == int(focusedmonitors.size()) - 1) {
      focusedmonitors.push_back(focusedmonitors[i]);
    } else {
      focusedmonitors[i + 1] = focusedmonitors[i];
    }
  }
  if (focusedmonitors.size() > 0) {
    focusedmonitors[0] = nullptr;
  }
}

void on_pop() {
  for (int i = 0; i < focusedmonitors.size(); i++) {
    if (i == int(focusedmonitors.size()) - 1) {
      focusedmonitors[i] = nullptr;
    } else {
      focusedmonitors[i] = focusedmonitors[i + 1];
    }
  }
  if (focusedmonitors.size() > 0) {
    focusedmonitors.pop_back();
  }
}

void on_zoom_in() { glasses.fov *= 0.99;  }

void on_zoom_out() { glasses.fov *= 1.01; }

void on_shift_left() { screen_angle_offset_degrees += 5.0f; }
void on_shift_right() { screen_angle_offset_degrees -= 5.0f; }

void on_toggle_center_dot() { center_dot_enabled = !center_dot_enabled; }

int main(int argc, char **argv) {
  if (init_glasses() != ERR_SUCCESS) {
    fprintf(stderr, "Failed to setup glasses\n");
    return 1;
  }

  glasses.fov = 45.0;
  on_align();

  on_align_command = on_align;
  on_push_command = on_push;
  on_pop_command = on_pop;
  on_zoom_in_command = on_zoom_in;
  on_zoom_out_command = on_zoom_out;
  on_shift_left_command = on_shift_left;
  on_shift_right_command = on_shift_right;
  on_toggle_center_dot_command = on_toggle_center_dot;
  int command_sockfd = setup_command_socket();
  if (command_sockfd < 0) {
    fprintf(stderr, "Failed to create command socket\n");
    // handle error
  }
  if (make_socket_non_blocking(command_sockfd) < 0) {
    perror("Failed to set socket non-blocking");
    // handle error
  }

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "Cannot open display\n");
    return 1;
  }
  int screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);

  // Get XRR monitors
  int n;
  XRRMonitorInfo *xrrmonitors = XRRGetMonitors(dpy, root, True, &n);
  if (!xrrmonitors || n == 0) {
    fprintf(stderr, "No monitors found\n");
    return 1;
  }

  int excludeIndex = -1;
  if (argc == 1) {
    // No argument: list monitors and exit
    printf("Detected monitors:\n");
    for (int i = 0; i < n; i++) {
      auto outputInfo =
          XRRGetOutputInfo(dpy, XRRGetScreenResourcesCurrent(dpy, root),
                           xrrmonitors[i].outputs[0]);
      printf("Monitor %d (%s): x=%d y=%d width=%d height=%d\n", i,
             outputInfo->name, xrrmonitors[i].x, xrrmonitors[i].y,
             xrrmonitors[i].width, xrrmonitors[i].height);
      std::string name(outputInfo->name);
      if (name == "DP-1" || name == "DP-2") {
        std::cout << "Detected viture glasses probably, continuing\n";
        excludeIndex = i;
        goto ok;
      }
    }
    XRRFreeMonitors(xrrmonitors);
    XCloseDisplay(dpy);
    return 0;
  } else {
    // Parse excluded monitor index from first argument
    excludeIndex = atoi(argv[1]);
    if (excludeIndex < 0 || excludeIndex >= n) {
      fprintf(stderr, "Invalid exclude monitor index %d (0 to %d allowed)\n",
              excludeIndex, n - 1);
      XRRFreeMonitors(xrrmonitors);
      XCloseDisplay(dpy);
      return 1;
    }
  }
ok:

  // Copy monitors except the excluded one
  for (int i = 0, index = 0; i < n; i++) {
    if (i == excludeIndex) {
      continue;
    }
    MyMonitor m;
    m.x = xrrmonitors[i].x;
    m.y = xrrmonitors[i].y;
    m.width = xrrmonitors[i].width;
    m.height = xrrmonitors[i].height;
    m.index = index++;
    monitors.push_back(m);
  }

  XRRFreeMonitors(xrrmonitors);

  if (!initGL()) {
    fprintf(stderr, "Failed to init GL\n");
    cleanup();
    return 1;
  }

  long highest = 0;
  int delay_highest_check_frames = 1000;
  int frame = 0;

  while (true) {
    auto start = std::chrono::high_resolution_clock::now();
    // std::cout << "fb size: " << framebuffer.width << "x" <<
    // framebuffer.height
    //           << "\n";
    // auto grabStart = std::chrono::high_resolution_clock::now();
    //  for (MyMonitor &m : monitors) {
    //    grabMonitor(m);
    //  }
    grabFramebuffer(framebuffer);
    // auto grabEnd = std::chrono::high_resolution_clock::now();
    // auto grabMs = std::chrono::duration_cast<std::chrono::microseconds>(
    //                   grabEnd - grabStart)
    //                   .count();
    // std::cout << "grab took " << grabMs << "us\n";

    auto uploadStart = std::chrono::high_resolution_clock::now();
    uploadFramebufferTexture(framebuffer);
    auto uploadEnd = std::chrono::high_resolution_clock::now();
    auto uploadMs = std::chrono::duration_cast<std::chrono::microseconds>(
                        uploadEnd - uploadStart)
                        .count();
    // std::cout << "upload took " << uploadMs << "us\n";

    auto pollStart = std::chrono::high_resolution_clock::now();
    poll_commands(command_sockfd);
    auto pollEnd = std::chrono::high_resolution_clock::now();
    auto pollMs = std::chrono::duration_cast<std::chrono::microseconds>(
                      pollEnd - pollStart)
                      .count();
    // std::cout << "poll took " << pollMs << "us\n";

    auto renderStart = std::chrono::high_resolution_clock::now();
    render();
    auto renderEnd = std::chrono::high_resolution_clock::now();
    auto renderMs = std::chrono::duration_cast<std::chrono::microseconds>(
                        renderEnd - renderStart)
                        .count();
    // std::cout << "render took " << renderMs << "us\n";

    static __useconds_t second = 1000000;
    static __useconds_t fps = 120;
    static __useconds_t us = second / fps;
    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count();

    if (frame > delay_highest_check_frames) {
      if (highest < duration_us) {
        std::cout << "new highest frame time: " << duration_us << " us\n";
        highest = duration_us;
      }
    } else {
      ++frame;
    }

    if (us > duration_us) {
      // std::cout << "sleeping for " << (us - duration_us) << "us\n";
      usleep(us - duration_us);
    }
  }

  cleanup();
  destroy_command_socket(command_sockfd);
  return 0;
}

void initShm(Display *dpy, Framebuffer &fb) {
  fb.img = XShmCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
                           ZPixmap, NULL, &shmInfo, fb.width, fb.height);
  if (!fb.img) {
    std::cerr << "oh no error\n";
    return;
  }

  shmInfo.shmid =
      shmget(IPC_PRIVATE, fb.img->bytes_per_line * fb.height, IPC_CREAT | 0777);
  if (shmInfo.shmid < 0) {
    perror("shmget failed");
    exit(1);
  }

  shmInfo.shmaddr = (char *)shmat(shmInfo.shmid, 0, 0);
  if (shmInfo.shmaddr == (char *)-1) {
    perror("shmat failed");
    exit(1);
  }

  fb.img->data = shmInfo.shmaddr;
  shmInfo.readOnly = False;

  if (!XShmAttach(dpy, &shmInfo)) {
    fprintf(stderr, "XShmAttach failed\n");
    exit(1);
  }

  // Optional: Mark the shared memory for removal (will be deleted once
  // detached)
  shmctl(shmInfo.shmid, IPC_RMID, 0);
}

void cleanupShm(Display *dpy) { XShmDetach(dpy, &shmInfo); }

void grabFramebuffer(Framebuffer &fb) {
  fb.width = DisplayWidth(dpy, DefaultScreen(dpy));
  fb.height = DisplayHeight(dpy, DefaultScreen(dpy));

  if (!fb.img || fb.img->width != fb.width || fb.img->height != fb.height) {
    if (fb.img) {
      XDestroyImage(fb.img);
      fb.img = nullptr;

      cleanupShm(dpy);
    }

    initShm(dpy, fb);
  }

  XShmGetImage(dpy, root, fb.img, 0, 0, AllPlanes);
}

void uploadFramebufferTexture(Framebuffer &fb) {
  if (fb.tex == 0) {
    glGenTextures(1, &fb.tex);
  }

  // Add cursor to fb.img->data before uploading
  XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
  if (ci) {
    int cursor_x = ci->x - ci->xhot;
    int cursor_y = ci->y - ci->yhot;

    for (unsigned int cy = 0; cy < ci->height; ++cy) {
      int img_y = cursor_y + cy;
      if (img_y < 0 || img_y >= fb.height)
        continue;

      for (unsigned int cx = 0; cx < ci->width; ++cx) {
        int img_x = cursor_x + cx;
        if (img_x < 0 || img_x >= fb.width)
          continue;

        unsigned long cursor_pixel = ci->pixels[cy * ci->width + cx];

        unsigned char a = (cursor_pixel >> 24) & 0xFF;
        if (a == 0)
          continue;

        unsigned char r = (cursor_pixel >> 16) & 0xFF;
        unsigned char g = (cursor_pixel >> 8) & 0xFF;
        unsigned char b = (cursor_pixel) & 0xFF;

        unsigned char *p = (unsigned char *)fb.img->data +
                           img_y * fb.img->bytes_per_line + img_x * 4;

        unsigned char bg_b = p[0];
        unsigned char bg_g = p[1];
        unsigned char bg_r = p[2];

        float alpha = a / 255.0f;
        float inv_alpha = 1.0f - alpha;

        p[0] = (unsigned char)(b * alpha + bg_b * inv_alpha); // B
        p[1] = (unsigned char)(g * alpha + bg_g * inv_alpha); // G
        p[2] = (unsigned char)(r * alpha + bg_r * inv_alpha); // R
        p[3] = 255;                                           // A
      }
    }
    XFree(ci);
  }

  glBindTexture(GL_TEXTURE_2D, fb.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb.width, fb.height, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, fb.img->data);
}

void getMonitorUVs(const MyMonitor &m, const Framebuffer &fb, float &u0,
                   float &v0, float &u1, float &v1) {
  u0 = (float)m.x / fb.width;
  v0 = (float)m.y / fb.height;
  u1 = (float)(m.x + m.width) / fb.width;
  v1 = (float)(m.y + m.height) / fb.height;

  // TODO: Remove
  // u0 = 0.0f;
  // v0 = 0.0f;
  // u1 = 1.0f;
  // v1 = 1.0f;
}

// void uploadTexture(MyMonitor &m) {
//   // Get current cursor image and position
//   XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
//   if (ci) {
//     // Cursor position relative to screen
//     int cursor_x = ci->x - ci->xhot;
//     int cursor_y = ci->y - ci->yhot;
//
//     // Blend cursor pixels into monitor image data if cursor overlaps this
//     // monitor
//     for (unsigned int cy = 0; cy < ci->height; ++cy) {
//       int img_y = cursor_y + cy - m.y;
//       if (img_y < 0 || img_y >= m.height)
//         continue;
//
//       for (unsigned int cx = 0; cx < ci->width; ++cx) {
//         int img_x = cursor_x + cx - m.x;
//         if (img_x < 0 || img_x >= m.width)
//           continue;
//
//         // Cursor pixel ARGB format in ci->pixels[]
//         unsigned long cursor_pixel = ci->pixels[cy * ci->width + cx];
//
//         // Extract ARGB components from cursor pixel
//         unsigned char a = (cursor_pixel >> 24) & 0xFF;
//         unsigned char r = (cursor_pixel >> 16) & 0xFF;
//         unsigned char g = (cursor_pixel >> 8) & 0xFF;
//         unsigned char b = (cursor_pixel) & 0xFF;
//
//         // Skip fully transparent pixels
//         if (a == 0)
//           continue;
//
//         // Calculate pixel offset in XImage data (assuming 32bpp BGRA)
//         unsigned char *p = (unsigned char *)m.img->data +
//                            img_y * m.img->bytes_per_line + img_x * 4;
//
//         // Current background pixel (BGRA)
//         unsigned char bg_b = p[0];
//         unsigned char bg_g = p[1];
//         unsigned char bg_r = p[2];
//         unsigned char bg_a = p[3];
//
//         // Alpha blending (simple over operator)
//         float alpha = a / 255.0f;
//         float inv_alpha = 1.0f - alpha;
//
//         unsigned char out_r = (unsigned char)(r * alpha + bg_r * inv_alpha);
//         unsigned char out_g = (unsigned char)(g * alpha + bg_g * inv_alpha);
//         unsigned char out_b = (unsigned char)(b * alpha + bg_b * inv_alpha);
//         unsigned char out_a = 255; // fully opaque after blending
//
//         // Store blended pixel back as BGRA
//         p[0] = out_b;
//         p[1] = out_g;
//         p[2] = out_r;
//         p[3] = out_a;
//       }
//     }
//
//     XFree(ci);
//   }
//
//   glBindTexture(GL_TEXTURE_2D, m.tex);
//   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m.width, m.height, GL_BGRA,
//                   GL_UNSIGNED_BYTE, m.img->data);
// }

bool initGL() {
  int screen = DefaultScreen(dpy);

  static int visual_attribs[] = {GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER,
                                 None};

  XVisualInfo *vi = glXChooseVisual(dpy, screen, visual_attribs);
  if (!vi) {
    fprintf(stderr, "No appropriate visual found\n");
    return false;
  }

  Colormap cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);
  XSetWindowAttributes swa;
  swa.colormap = cmap;
  swa.event_mask = ExposureMask | KeyPressMask;

  win = XCreateWindow(dpy, root, 0, 0, 1920, 1080, 0, vi->depth, InputOutput,
                      vi->visual, CWColormap | CWEventMask, &swa);

  XMapWindow(dpy, win);
  XStoreName(dpy, win, "Multi-monitor viewer");

  glc = glXCreateContext(dpy, vi, NULL, GL_TRUE);
  glXMakeCurrent(dpy, win, glc);

  glEnable(GL_TEXTURE_2D);
  glClearColor(0.0f, 0.0f, 0.0f, 1.f);

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(60.0, 1920.0 / 1080.0, 1.0, 1000.0);

  glMatrixMode(GL_MODELVIEW);

  return true;
}

struct Vec3 {
  float x, y, z;

  Vec3 operator+(const Vec3 &b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec3 operator-(const Vec3 &b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

Vec3 normalize(Vec3 v) {
  float len = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len == 0.0f)
    return {0, 0, 0};
  return {v.x / len, v.y / len, v.z / len};
}

Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

int focusIndex = 0;
int focusCandidate = -1;
int focusFrames = 0;
const int FOCUS_HOLD_FRAMES = 20;

// Convert head orientation to direction vector
void getLookVector(float &dx, float &dy, float &dz) {
  float pitchRad = get_pitch(glasses) * M_PI / 180.0;
  float yawRad = get_yaw(glasses) * M_PI / 180.0;

  dx = sin(yawRad) * -cos(pitchRad);
  dy = -sin(pitchRad);
  dz = -cos(yawRad) * cos(pitchRad);
}

bool isLookingAt(float eyeX, float eyeY, float eyeZ, float rayX, float rayY,
                 float rayZ, float centerX, float centerY, float centerZ,
                 float width, float height) {
  // Avoid division by zero for ray parallel to plane
  if (fabs(rayZ) < 1e-5)
    return false;

  // Calculate intersection parameter t for plane Z = centerZ
  float t = (centerZ - eyeZ) / rayZ;

  // Intersection must be in front of the eye
  if (t < 0)
    return false;

  // Calculate intersection point coordinates
  float ix = eyeX + rayX * t;
  float iy = eyeY + rayY * t;

  // Check if intersection lies within quad bounds
  return (ix >= centerX - width / 2 && ix <= centerX + width / 2 &&
          iy >= centerY - height / 2 && iy <= centerY + height / 2);
}

void render() {
  // if (focusedmonitors.size() > 0) {
  //   // Suppose focusedmonitors[0] has these fields:
  //   int x = focusedmonitors[0]->x;
  //   int y = focusedmonitors[0]->y;
  //   int width = focusedmonitors[0]->width;
  //   int height = focusedmonitors[0]->height;

  //  // Warp mouse to center of that monitor:
  //  int target_x = x + width / 2;
  //  int target_y = y + height / 2;

  //  XWarpPointer(dpy, None, root, 0, 0, 0, 0, target_x, target_y);
  //  XFlush(dpy);
  //}

  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_TEXTURE_2D);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(glasses.fov, 1920.0 / 1080.0, 0.1, 100.0);

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  // Head orientation
  float roll = get_roll(glasses);
  // glRotatef(roll, 0.0f, 0.0f, 1.0f);
  // glRotatef(get_pitch(glasses), 1.0f, 0.0f, 0.0f);
  // glRotatef(-get_yaw(glasses), 0.0f, 1.0f, 0.0f);

  float roll_perc = roll / 10.0f;
  float roll_threshold = 0.35f;
  if (abs(roll_perc) < roll_threshold) {
    roll_perc = 0.0f;
  } else {
    if (roll_perc < 0.0f) {
      roll_perc += roll_threshold;
    } else {
      roll_perc -= roll_threshold;
    }
  }
  roll_perc *= abs(roll_perc);
  roll_perc *= 0.5f;
  roll_perc = 0.0f;

  float focused_w = 3.0f;
  // radius of circle inscribed in a hexagon, i.e. the distance to the centre
  // of the edges from the hexagon's centre.
  //
  // radius of inscribed circle in a regular n-gon with sidelength a
  // r = (a / 2) * cot(π/n)
  //
  // for hexagon
  // float r = sqrt(3.0) / 2.0 * focused_w;
  float angle_deg = 20.0f;
  // I'm just assuming it works with non-integers.
  float n = 360.0f / angle_deg;
  float pi_div_n = 3.14159265359 / n;
  float r = (focused_w / 2.0) * (cos(pi_div_n) / sin(pi_div_n));
  float base_z = -r + roll_perc;

  // Get gaze vector

  float flat_z = r - focused_w * 1.05f;

  float rayX, rayY, rayZ;
  getLookVector(rayX, rayY, rayZ);
  float eyeX = rayX * roll_perc;
  float eyeY = rayY * roll_perc;
  float eyeZ = rayZ * roll_perc - flat_z;
  float tx = eyeX + rayX;
  float ty = eyeY + rayY;
  float tz = eyeZ + rayZ;
  gluLookAt(eyeX, eyeY, eyeZ, tx, ty, tz, 0.0f, 1.0f, 0.0f);
  glRotatef(roll, 0.0f, 0.0f, 1.0f);

  while (focusedmonitors.size() > 7) {
    // TODO: some other way
    focusedmonitors.pop_back();
  }

  {

    for (int i = 0; i < int(focusedmonitors.size()) - 1; i++) {
      const MyMonitor *m = focusedmonitors[i + 1];
      if (m == nullptr)
        continue;

      float aspect = (float)m->height / m->width;
      float focused_h = focused_w * aspect;

      glPushMatrix();
      glRotatef(-i * angle_deg + screen_angle_offset_degrees, 0.0f, 1.0f, 0.0f);
      glTranslatef(0.0f, 0.0f, base_z);

      // framebuffer already bound
      // glBindTexture(GL_TEXTURE_2D, m->tex);

      glBegin(GL_QUADS);
      float u0, v0, u1, v1;
      getMonitorUVs(*m, framebuffer, u0, v0, u1, v1);
      glTexCoord2f(u0, v0);
      glVertex3f(-focused_w / 2, focused_h / 2, 0);
      glTexCoord2f(u1, v0);
      glVertex3f(focused_w / 2, focused_h / 2, 0);
      glTexCoord2f(u1, v1);
      glVertex3f(focused_w / 2, -focused_h / 2, 0);
      glTexCoord2f(u0, v1);
      glVertex3f(-focused_w / 2, -focused_h / 2, 0);
      glEnd();

      glPopMatrix();
    }
  }

  if (focusedmonitors.size() > 0 && focusedmonitors[0] != nullptr) {
    // place first focused monitor in our lap
    const auto &m = focusedmonitors.at(0);
    float aspect = (float)m->height / m->width;
    float focused_h = focused_w * aspect;

    glPushMatrix();
    // -36.0f was just tested and seems okay, idk what it should be to be
    // correct geometrically
    glRotatef(-angle_deg * aspect, 1.0f, 0.0f, 0.0f);
    glTranslatef(0.0f, 0.0f, base_z);

    // glBindTexture(GL_TEXTURE_2D, m->tex);

    glBegin(GL_QUADS);
    float u0, v0, u1, v1;
    getMonitorUVs(*m, framebuffer, u0, v0, u1, v1);
    glTexCoord2f(u0, v0);
    glVertex3f(-focused_w / 2, focused_h / 2, 0);
    glTexCoord2f(u1, v0);
    glVertex3f(focused_w / 2, focused_h / 2, 0);
    glTexCoord2f(u1, v1);
    glVertex3f(focused_w / 2, -focused_h / 2, 0);
    glTexCoord2f(u0, v1);
    glVertex3f(-focused_w / 2, -focused_h / 2, 0);
    glEnd();

    glPopMatrix();
  }

  // Draw thumbnails above focused screen
  float thumbY = 1.2f;
  float spacing = 0.6f;
  float thumbSize = 0.55f;
  for (size_t i = 0; i < monitors.size(); i++) {
    float x = (i - (monitors.size() - 1) / 2.0f) * spacing;
    float y = thumbY;
    float z = base_z;

    // glBindTexture(GL_TEXTURE_2D, monitors[i].tex);
    glPushMatrix();
    glTranslatef(x, y, z);

    glBegin(GL_QUADS);
    float u0, v0, u1, v1;
    getMonitorUVs(monitors[i], framebuffer, u0, v0, u1, v1);
    glTexCoord2f(u0, v0);
    glVertex3f(-thumbSize / 2, thumbSize / 2, 0);
    glTexCoord2f(u1, v0);
    glVertex3f(thumbSize / 2, thumbSize / 2, 0);
    glTexCoord2f(u1, v1);
    glVertex3f(thumbSize / 2, -thumbSize / 2, 0);
    glTexCoord2f(u0, v1);
    glVertex3f(-thumbSize / 2, -thumbSize / 2, 0);
    glEnd();
    glPopMatrix();

    // Gaze selection
    if (isLookingAt(eyeX, eyeY, eyeZ, rayX, rayY, rayZ, x, y, z, thumbSize,
                    thumbSize)) {
      if (focusCandidate == i) {
        focusFrames++;
        if (focusFrames >= FOCUS_HOLD_FRAMES) {
          focusIndex = i;
          focusCandidate = -1;
          focusFrames = 0;
          if (focusedmonitors.size() == 0) {
            focusedmonitors.push_back(&monitors[i]);
          } else {
            focusedmonitors[0] = &monitors[i];
          }
        }
      } else {
        focusCandidate = i;
        focusFrames = 1;
      }
    }
  }

  if (center_dot_enabled) {
    draw_filled_center_rect(4.0f, 4.0f);
  }

  glFlush();
  glXSwapBuffers(dpy, win);
}

void cleanup() {
  // for (MyMonitor &m : monitors) {
  //   if (m.img) {
  //     XShmDetach(dpy, &m.shmInfo);
  //     shmdt(m.shmInfo.shmaddr);
  //     XDestroyImage(m.img);
  //     glDeleteTextures(1, &m.tex);
  //   }
  // }
  focusedmonitors.clear();
  monitors.clear();

  if (glc)
    glXDestroyContext(dpy, glc);
  if (dpy)
    XCloseDisplay(dpy);
}
