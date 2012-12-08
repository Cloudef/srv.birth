#ifndef SRVBIRTH_BAMS_H
#define SRVBIRTH_BAMS_H

typedef struct Vector3f {
   float x, y, z;
} Vector3f;

typedef struct Vector2f {
   float x, y;
} Vector2f;

typedef struct Vector3B {
   unsigned int x, y, z;
} Vector3B;

typedef struct Vector2B {
   unsigned int x, y;
} Vector2B;

unsigned int FToBams(float f);
float BamsToF(unsigned int b);
void V2FToBams(Vector2B *bv2, const Vector2f *v2);
void V3FToBams(Vector3B *bv3, const Vector3f *v3);
void BamsToV2F(Vector2f *v2, const Vector2B *bv2);
void BamsToV3F(Vector3f *v3, const Vector3B *bv3);

#endif /* SRVBIRTH_BAMS_H */

/* vim: set ts=8 sw=3 tw=0 :*/
