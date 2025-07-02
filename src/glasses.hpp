#pragma once

#include <GL/gl.h>
#include <cstdio>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "viture.h"

struct Glasses {
  float roll, pitch, yaw;
  float qw, qx, qy, qz;

  float oroll, opitch, oyaw;
  float oqw, oqx, oqy, oqz;

  GLdouble fov;
};

static Glasses glasses{};

static float get_roll(Glasses g) { return g.roll + g.oroll; }

static float get_pitch(Glasses g) { return g.pitch + g.opitch; }

static float get_yaw(Glasses g) { return g.yaw + g.oyaw; }

static float makeFloat(uint8_t *data) {
  float value = 0;
  uint8_t tem[4];
  tem[0] = data[3];
  tem[1] = data[2];
  tem[2] = data[1];
  tem[3] = data[0];
  memcpy(&value, tem, 4);
  return value;
}

static void imuCallback(uint8_t *data, uint16_t len, uint32_t ts) {
  glasses.roll = makeFloat(data);
  glasses.pitch = makeFloat(data + 4);
  glasses.yaw = makeFloat(data + 8);

  if (len >= 36) {
    glasses.qw = makeFloat(data + 20);
    glasses.qx = makeFloat(data + 24);
    glasses.qy = makeFloat(data + 28);
    glasses.qz = makeFloat(data + 32);
  }
}

static void mcuCallback(uint16_t msgid, uint8_t *data, uint16_t len,
                        uint32_t ts) {}

// Returns ERR_SUCCESS if succeeded, otherwise something else.
static int init_glasses() {
  if (!init(imuCallback, mcuCallback)) {
    fprintf(stderr, "Failed to init glasses\n");
    return ERR_FAILURE;
  }

  int result = set_imu(true);
  if (result != ERR_SUCCESS) {
    fprintf(stderr, "Failed to set imu=true on glasses\n");
    return result;
  }

  // result = set_imu_fq(IMU_FREQUENCE_240);
  // if (result != ERR_SUCCESS) {
  //   fprintf(stderr, "Failed to set imufq=240 on glasses\n");
  //   return result;
  // }
  result = set_imu_fq(IMU_FREQUENCE_120);
  if (result != ERR_SUCCESS) {
    fprintf(stderr, "Failed to set imufq=120 on glasses\n");
    return result;
  }

  set_3d(false);

  return ERR_SUCCESS;
}
