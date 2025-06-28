#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h> // for usleep
#include <vector>

#include "command_socket.hpp"
#include "glasses.hpp"
#include "viture.h"

struct MyMonitor {
  int x, y, width, height;
  int index;
  XImage *img = nullptr;
  XShmSegmentInfo shmInfo;
  GLuint tex = 0;
};

Display *dpy;
Window root;
Window win;
GLXContext glc;
std::vector<MyMonitor> monitors;
std::vector<MyMonitor *> focusedmonitors;

bool initGL();
void cleanup();
void grabMonitor(MyMonitor &m);
void uploadTexture(MyMonitor &m);
void render();

void on_align() {
  glasses.oroll = -glasses.roll;
  glasses.opitch = -glasses.pitch;
  glasses.oyaw = -glasses.yaw;
}

void on_push() {
  for (int i = focusedmonitors.size() - 1; i >= 0; i--) {
    if (i == focusedmonitors.size() - 1) {
      focusedmonitors.push_back(focusedmonitors[i]);
    } else {
      focusedmonitors[i + 1] = focusedmonitors[i];
    }
  }
  focusedmonitors[0] = nullptr;
}

void on_pop() {
  for (int i = 0; i < focusedmonitors.size(); i++) {
    if (i == focusedmonitors.size() - 1) {
      focusedmonitors[i] = nullptr;
    } else {
      focusedmonitors[i] = focusedmonitors[i + 1];
    }
  }
  if (focusedmonitors.size() > 0) {
    focusedmonitors.pop_back();
  }
}

void on_zoom_in() { glasses.fov *= 0.99; }

void on_zoom_out() { glasses.fov *= 1.01; }

int main(int argc, char **argv) {
  if (init_glasses() != ERR_SUCCESS) {
    fprintf(stderr, "Failed to setup glasses\n");
    return 1;
  }

  glasses.fov = 30.0;
  on_align();

  on_align_command = on_align;
  on_push_command = on_push;
  on_pop_command = on_pop;
  on_zoom_in_command = on_zoom_in;
  on_zoom_out_command = on_zoom_out;
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

  if (argc == 1) {
    // No argument: list monitors and exit
    printf("Detected monitors:\n");
    for (int i = 0; i < n; i++) {
      printf("Monitor %d: x=%d y=%d width=%d height=%d\n", i, xrrmonitors[i].x,
             xrrmonitors[i].y, xrrmonitors[i].width, xrrmonitors[i].height);
    }
    XRRFreeMonitors(xrrmonitors);
    XCloseDisplay(dpy);
    return 0;
  }

  // Parse excluded monitor index from first argument
  int excludeIndex = atoi(argv[1]);
  if (excludeIndex < 0 || excludeIndex >= n) {
    fprintf(stderr, "Invalid exclude monitor index %d (0 to %d allowed)\n",
            excludeIndex, n - 1);
    XRRFreeMonitors(xrrmonitors);
    XCloseDisplay(dpy);
    return 1;
  }

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

  // Prepare XShm images and GL textures for remaining monitors
  int i = 0;
  for (MyMonitor &m : monitors) {
    m.img = XShmCreateImage(dpy, DefaultVisual(dpy, screen),
                            DefaultDepth(dpy, screen), ZPixmap, NULL,
                            &m.shmInfo, m.width, m.height);
    if (!m.img) {
      fprintf(stderr, "Failed to create XImage for monitor %d\n", i);
      cleanup();
      return 1;
    }

    m.shmInfo.shmid = shmget(IPC_PRIVATE, m.img->bytes_per_line * m.img->height,
                             IPC_CREAT | 0777);
    if (m.shmInfo.shmid < 0) {
      fprintf(stderr, "Failed to get shm segment for monitor %d\n", i);
      cleanup();
      return 1;
    }

    m.shmInfo.shmaddr = m.img->data = (char *)shmat(m.shmInfo.shmid, 0, 0);
    m.shmInfo.readOnly = False;

    if (!XShmAttach(dpy, &m.shmInfo)) {
      fprintf(stderr, "XShmAttach failed for monitor %d\n", i);
      cleanup();
      return 1;
    }

    shmctl(m.shmInfo.shmid, IPC_RMID, 0);

    glGenTextures(1, &m.tex);
    glBindTexture(GL_TEXTURE_2D, m.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m.width, m.height, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, NULL);

    ++i;
  }

  while (true) {
    for (MyMonitor &m : monitors) {
      grabMonitor(m);
      uploadTexture(m);
    }

    poll_commands(command_sockfd);
    render();

    static __useconds_t second = 1000000;
    static __useconds_t fps = 200;
    static __useconds_t us = second / fps;
    usleep(us);
  }

  cleanup();
  destroy_command_socket(command_sockfd);
  return 0;
}

void grabMonitor(MyMonitor &m) {
  XShmGetImage(dpy, root, m.img, m.x, m.y, AllPlanes);
}

void uploadTexture(MyMonitor &m) {
  glBindTexture(GL_TEXTURE_2D, m.tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m.width, m.height, GL_BGRA,
                  GL_UNSIGNED_BYTE, m.img->data);
}

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

// Check ray intersection with a screen quad (simplified)
bool isLookingAt(float rayX, float rayY, float rayZ, float centerX,
                 float centerY, float centerZ, float width, float height) {
  // Project onto plane Z = centerZ
  if (fabs(rayZ) < 1e-5)
    return false;
  float t = (centerZ - 0.0f) / rayZ;
  if (t < 0)
    return false;

  float ix = rayX * t;
  float iy = rayY * t;

  return (ix >= centerX - width / 2 && ix <= centerX + width / 2 &&
          iy >= centerY - height / 2 && iy <= centerY + height / 2);
}

void render() {
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_TEXTURE_2D);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(glasses.fov, 1.6, 0.1, 100.0);

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

  // Get gaze vector
  float rayX, rayY, rayZ;
  getLookVector(rayX, rayY, rayZ);
  float eyeX = rayX * roll_perc;
  float eyeY = rayY * roll_perc;
  float eyeZ = rayZ * roll_perc;
  float tx = eyeX + rayX;
  float ty = eyeY + rayY;
  float tz = eyeZ + rayZ;
  gluLookAt(eyeX, eyeY, eyeZ, tx, ty, tz, 0.0f, 1.0f, 0.0f);

  const float radius = 3.0f; // hexagon radius distance
  const float focused_w = 2.5f;
  const float base_z = -3.0f + roll_perc;

  size_t count = focusedmonitors.size();
  if (count > 6) {
    count = 6;
  }

  for (size_t i = 0; i < count; i++) {
    const MyMonitor *m = focusedmonitors[i];
    if (m == nullptr) {
      continue; // skip null monitors
    }

    float aspect = (float)m->height / m->width;
    float focused_h = focused_w * aspect;

    float x = 0.0f;
    float y = 0.0f;   // keep vertical at zero (or adjust if needed)
    float z = base_z; // base depth from camera

    float angle_deg = 0.0f;

    if (i == 0) {
      // center monitor at origin
      x = 0.0f;
      z = base_z;
      angle_deg = 0.0f;
    } else {
      int idx = i - 1;
      angle_deg = 60.0f * idx;
      float angle_rad = angle_deg * M_PI / 180.0f;

      // place on hexagon ring in XZ plane
      x = radius * cos(angle_rad);
      z = base_z + radius * sin(angle_rad);
    }

    glBindTexture(GL_TEXTURE_2D, m->tex);
    glPushMatrix();

    glTranslatef(x, y, z);
    glRotatef(angle_deg, 0.0f, 1.0f, 0.0f);

    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex3f(-focused_w / 2, focused_h / 2, 0);
    glTexCoord2f(1, 0);
    glVertex3f(focused_w / 2, focused_h / 2, 0);
    glTexCoord2f(1, 1);
    glVertex3f(focused_w / 2, -focused_h / 2, 0);
    glTexCoord2f(0, 1);
    glVertex3f(-focused_w / 2, -focused_h / 2, 0);
    glEnd();

    glPopMatrix();
  }

  // Draw thumbnails above focused screen
  float thumbY = 1.1f;
  float spacing = 0.6f;
  float thumbSize = 0.55f;
  for (size_t i = 0; i < monitors.size(); i++) {
    float x = (i - (monitors.size() - 1) / 2.0f) * spacing;
    float y = thumbY;
    float z = base_z;

    glBindTexture(GL_TEXTURE_2D, monitors[i].tex);
    glPushMatrix();
    glTranslatef(x, y, z);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex3f(-thumbSize / 2, thumbSize / 2, 0);
    glTexCoord2f(1, 0);
    glVertex3f(thumbSize / 2, thumbSize / 2, 0);
    glTexCoord2f(1, 1);
    glVertex3f(thumbSize / 2, -thumbSize / 2, 0);
    glTexCoord2f(0, 1);
    glVertex3f(-thumbSize / 2, -thumbSize / 2, 0);
    glEnd();
    glPopMatrix();

    // Gaze selection
    if (isLookingAt(rayX, rayY, rayZ, x, y, z, thumbSize, thumbSize)) {
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

  glFlush();
  glXSwapBuffers(dpy, win);
}

void cleanup() {
  for (MyMonitor &m : monitors) {
    if (m.img) {
      XShmDetach(dpy, &m.shmInfo);
      shmdt(m.shmInfo.shmaddr);
      XDestroyImage(m.img);
      glDeleteTextures(1, &m.tex);
    }
  }
  focusedmonitors.clear();
  monitors.clear();

  if (glc)
    glXDestroyContext(dpy, glc);
  if (dpy)
    XCloseDisplay(dpy);
}
