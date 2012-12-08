#include <arpa/inet.h>
#include "bams.h"

/* \brief float to bams */
inline unsigned int FToBams(float f)
{
   return htonl(*((unsigned int*)&f)>>16);
}

/* \brief bams to float */
inline float BamsToF(unsigned int b)
{
   unsigned int x = ntohl(b)<<16;
   return *((float*)&x);
}

/* \brief float vector to bams v2 vector */
inline void V2FToBams(Vector2B *bv2, const Vector2f *v2)
{
   bv2->x = FToBams(v2->x);
   bv2->y = FToBams(v2->y);
}

/* \brief float vector to bams v3 vector */
inline void V3FToBams(Vector3B *bv3, const Vector3f *v3)
{
   V2FToBams((Vector2B*)bv3, (Vector2f*)v3);
   bv3->z = FToBams(v3->z);
}

/* \brief bams v2 vector to float vector */
inline void BamsToV2F(Vector2f *v2, const Vector2B *bv2)
{
   v2->x = BamsToF(bv2->x);
   v2->y = BamsToF(bv2->y);
}

/* \brief bams v3 vector to float vector */
inline void BamsToV3F(Vector3f *v3, const Vector3B *bv3)
{
   BamsToV2F((Vector2f*)v3, (Vector2B*)bv3);
   v3->z = BamsToF(bv3->z);
}
