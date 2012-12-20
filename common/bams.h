#ifndef SRVBIRTH_BAMS_H
#define SRVBIRTH_BAMS_H

typedef struct Vector3f {
   float x, y, z;
} Vector3f;

typedef struct Vector2f {
   float x, y;
} Vector2f;

/* 8-bit bams */
#define TOBAMS(x) (((x)/360.0) * 256)
#define TODEGS(b) (((b)/256.0) * 360)

#endif /* SRVBIRTH_BAMS_H */

/* vim: set ts=8 sw=3 tw=0 :*/
