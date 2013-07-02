#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <enet/enet.h>
#include <GLFW/glfw3.h>
#include <glhck/glhck.h>

#include "bams.h"
#include "types.h"

#include <float.h>

#define IFDO(f, x) { if (x) f(x); x = NULL; }

static int RUNNING = 0;
static int WIDTH = 800, HEIGHT = 480;

static glhckObject *wall = NULL;

#if 0
/* internal method */
kmBool kmTrianglePointIsOnSameSide(const kmVec3 *p1, const kmVec3 *p2, const kmVec3 *a, const kmVec3 *b)
{
   static kmVec3 zero = {0,0,0};
   kmVec3 bminusa, p1minusa, p2minusa, cp1, cp2;
   kmScalar res;

   kmVec3Subtract(&bminusa, b, a);
   kmVec3Subtract(&p1minusa, p1, a);
   kmVec3Subtract(&p2minusa, p2, a);
   kmVec3Cross(&cp1, &bminusa, &p1minusa);
   kmVec3Cross(&cp2, &bminusa, &p2minusa);

   res = kmVec3Dot(&cp1, &cp2);
#if !USE_DOUBLE_PRECISION
   if (res < 0) {
      /* This catches some floating point troubles. */
      kmVec3Normalize(&bminusa, &bminusa);
      kmVec3Normalize(&p1minusa, &p1minusa);
      kmVec3Cross(&cp1, &bminusa, &p1minusa);
      if (kmVec3AreEqual(&cp1, &zero)) res = 0.0;
   }
#endif
   return (res >= 0.0?KM_TRUE:KM_FALSE);
}

/* assumes that the point is already on the plane of the triangle. */
static kmEnum kmTriangleContainsPoint(const kmTriangle *pIn, const kmVec3 *pV1)
{
   if (kmTrianglePointIsOnSameSide(pV1, &pIn->v1, &pIn->v2, &pIn->v3) &&
       kmTrianglePointIsOnSameSide(pV1, &pIn->v2, &pIn->v1, &pIn->v3) &&
       kmTrianglePointIsOnSameSide(pV1, &pIn->v3, &pIn->v1, &pIn->v2) == KM_TRUE)
      return KM_CONTAINS_ALL;
   return KM_CONTAINS_NONE;
}

static kmBool _kmEllipseGetLowestRoot(kmScalar a, kmScalar b, kmScalar c, kmScalar maxR, kmScalar *root)
{
   /* check if solution exists */
   const kmScalar determinant = b*b - 4.0f*a*c;

   /* if determinant is negative, no solution */
   if (determinant < 0.0f || a == 0.0f)
      return KM_FALSE;

   /* calculate two roots: (if det == 0 then x1 == x2
    * but lets disregard that slight optimization) */

   const kmScalar sqrtD = sqrtf(determinant);
   const kmScalar invDA = 1.0/(2*a);
   kmScalar r1 = (-b - sqrtD) * invDA;
   kmScalar r2 = (-b + sqrtD) * invDA;

   /* r1 must always be less */
   if (r1 > r2) kmSwap(&r1, &r2);

   /* get lowest root */
   if (r1 > 0 && r1 < maxR) {
      if (root) *root = r1;
      return KM_TRUE;
   }

   /* it's possible that we want r2, this can happen if r1 < 0 */
   if (r2 > 0 && r2 < maxR) {
      if (root) *root = r2;
      return KM_TRUE;
   }

   return KM_FALSE;
}

/* for each edge or vertex a quadratic equation has to be solved:
 * a*t^2 + b*t + c = 0. We calculate a,b, and c for each test. */
static kmBool _kmEllipseTestTrianglePoint(const kmVec3 *base, const kmVec3 *point, const kmVec3 *velocity, kmScalar vsq, kmScalar time, kmScalar *outTime)
{
   kmVec3 tmp;
   kmScalar b, c;
   kmVec3Subtract(&tmp, base, point);
   b = 2.0f * kmVec3Dot(velocity, &tmp);
   kmVec3Subtract(&tmp, point, base);
   c = kmVec3LengthSq(&tmp) - 1.0f;
   return (_kmEllipseGetLowestRoot(vsq, b, c, time, outTime)?KM_TRUE:KM_FALSE);
}

static kmBool _kmEllipseTestEdgePoint(const kmVec3 *base, const kmVec3 *point1, const kmVec3 *point2, const kmVec3 *velocity, kmScalar vsq, kmScalar time, kmScalar *outTime, kmVec3 *outPoint)
{
   kmVec3 edge, baseToVertex;
   kmScalar edgeSquaredLength, edgeDotVelocity, edgeDotBaseToVertex;
   kmScalar a, b, c, f;
   kmVec3Subtract(&edge, point2, point1);
   kmVec3Subtract(&baseToVertex, point1, base);
   edgeSquaredLength = kmVec3LengthSq(&edge);
   edgeDotVelocity = kmVec3Dot(&edge, velocity);
   edgeDotBaseToVertex = kmVec3Dot(&edge, &baseToVertex);

   a = edgeSquaredLength * -vsq + edgeDotVelocity * edgeDotVelocity;
   b = edgeSquaredLength * (2.0 * kmVec3Dot(velocity, &baseToVertex)) - 2.0 * edgeDotVelocity * edgeDotBaseToVertex;
   c = edgeSquaredLength * (1.0 - kmVec3LengthSq(&baseToVertex)) + edgeDotBaseToVertex * edgeDotBaseToVertex;

   // does the swept sphere collide against infinite edge?
   if (_kmEllipseGetLowestRoot(a, b, c, time, outTime)) {
      f = (edgeDotVelocity * (*outTime) - edgeDotBaseToVertex) / edgeSquaredLength;
      if (!(f >= 0.0 && f  <= 1.0)) return KM_FALSE;
   }

   if (outPoint) {
      kmVec3Scale(&edge, &edge, f);
      kmVec3Add(outPoint, point1, &edge);
   }
   return KM_TRUE;
}

/* assumes the triangle is given in ellipse space */
static kmBool kmEllipseCollidesTriangle(const kmVec3 *pIn, const kmTriangle *triangle, const kmVec3 *velocity, kmVec3 *outIntersectionPoint, kmScalar *outDistanceToCollision)
{
   kmPlane plane;
   kmVec3 intersectionPoint, normalizedVelocity;
   kmScalar signedDistToTrianglePlane, normalDotVelocity, time, t0, t1;
   kmBool embeddedInPlane = KM_FALSE, foundCollision = KM_FALSE;
   assert(pIn && triangle && velocity);

   if (outIntersectionPoint) memset(outIntersectionPoint, 0, sizeof(kmVec3));
   if (outDistanceToCollision) *outDistanceToCollision = 0;
   kmVec3Normalize(&normalizedVelocity, velocity);

   /* make plane containing this triangle */
   kmPlaneFromPoints(&plane, &triangle->v1, &triangle->v2, &triangle->v3);

   /* check only front-facing triangles */
   if (kmPlaneClassifyPoint(&plane, &normalizedVelocity) == POINT_BEHIND_PLANE)
      return KM_FALSE;

   /* calculate the signed distance from sphere position to triangle plane */
   signedDistToTrianglePlane = kmPlaneDistanceTo(&plane, pIn);
   normalDotVelocity = kmPlaneDotNormal(&plane, velocity);

   /* if sphere is travelling parrallel to the plane: */
   if (kmAlmostEqual(normalDotVelocity, 0.0)) {
      if (fabsf(signedDistToTrianglePlane) >= 1.0f) {
         /* sphere is not embedded in plane, no collision possible: */
         return KM_FALSE;
      } else {
         /* sphere is embedded in plane, it intersects in the whole range [0..1] */
         embeddedInPlane = 1;
         t0 = 0.0;
         t1 = 1.0;
      }
   } else {
      /* normalDotVelocity is not 0, calculate intersection interval: */
      t0 = (-1.0-signedDistToTrianglePlane)/normalDotVelocity;
      t1 = ( 1.0-signedDistToTrianglePlane)/normalDotVelocity;

      /* t0 must always be less */
      if (t0 > t1) kmSwap(&t0, &t1);

      /* check that at least one result is within range: */
      if (t0 > 1.0f || t1 < 0.0f) {
         /* both t values are outside [0..1], impossibru */
         return KM_FALSE;
      }

      /* clamp to [0..1] */
      t0 = kmClamp(t0, 0.0, 1.0);
      t1 = kmClamp(t1, 0.0, 1.0);
   }

   /* if there is any intersection, it's between t0 && t1 */

   /* first check the easy case: Collision within the triangle;
    * if this happens, it must be at t0 and this is when the sphere
    * rests on the front side of the triangle plane. This can only happen
    * if the sphere is not embedded in the triangle plane. */
   if (embeddedInPlane == KM_FALSE) {
      kmVec3 baseMinusNormal, t0MultVelocity, planeIntersection;
      baseMinusNormal.x = pIn->x-plane.a;
      baseMinusNormal.y = pIn->y-plane.b;
      baseMinusNormal.z = pIn->z-plane.c;
      kmVec3Scale(&t0MultVelocity, velocity, t0);
      kmVec3Add(&planeIntersection, &baseMinusNormal, &t0MultVelocity);
      if (kmTriangleContainsPoint(triangle, &planeIntersection) != KM_CONTAINS_NONE) {
         foundCollision = KM_TRUE;
         time = t0;
         memcpy(&intersectionPoint, &planeIntersection, sizeof(kmVec3));
      }
   }

   /* if we havent found a collision already we will have to sweep
    * the sphere against points and edges of the triangle. Note: A
    * collision inside the triangle will always happen before a
    * vertex or edge collision. */
   if (foundCollision == KM_FALSE) {
      kmVec3 point;
      kmScalar velocitySquaredLength, newTime;
      velocitySquaredLength = kmVec3LengthSq(velocity);

      /* t.v1 */
      if (_kmEllipseTestTrianglePoint(pIn, &triangle->v1, velocity, velocitySquaredLength, time, &newTime)) {
         foundCollision = KM_TRUE;
         time = newTime;
         memcpy(&intersectionPoint, &triangle->v1, sizeof(kmVec3));
      }

      /* t.v2 */
      if (foundCollision == KM_FALSE) {
         if (_kmEllipseTestTrianglePoint(pIn, &triangle->v2, velocity, velocitySquaredLength, time, &newTime)) {
            foundCollision = KM_TRUE;
            time = newTime;
            memcpy(&intersectionPoint, &triangle->v2, sizeof(kmVec3));
         }
      }

      /* t.v3 */
      if (foundCollision == KM_FALSE) {
         if (_kmEllipseTestTrianglePoint(pIn, &triangle->v3, velocity, velocitySquaredLength, time, &newTime)) {
            foundCollision = KM_TRUE;
            time = newTime;
            memcpy(&intersectionPoint, &triangle->v3, sizeof(kmVec3));
         }
      }

      /* t.v1 --- t.v2 */
      if (_kmEllipseTestEdgePoint(pIn, &triangle->v1, &triangle->v2, velocity, velocitySquaredLength, time, &newTime, &point)) {
         foundCollision = KM_TRUE;
         time = newTime;
         memcpy(&intersectionPoint, &point, sizeof(kmVec3));
      }

      /* t.v2 --- t.v3 */
      if (_kmEllipseTestEdgePoint(pIn, &triangle->v2, &triangle->v3, velocity, velocitySquaredLength, time, &newTime, &point)) {
         foundCollision = KM_TRUE;
         time = newTime;
         memcpy(&intersectionPoint, &point, sizeof(kmVec3));
      }

      /* t.v3 --- t.v1 */
      if (_kmEllipseTestEdgePoint(pIn, &triangle->v3, &triangle->v1, velocity, velocitySquaredLength, time, &newTime, &point)) {
         foundCollision = KM_TRUE;
         time = newTime;
         memcpy(&intersectionPoint, &point, sizeof(kmVec3));
      }
   }

   /* no collision */
   if (foundCollision == KM_FALSE)
      return KM_FALSE;

   /* return collision data */
   if (outDistanceToCollision) *outDistanceToCollision = time*kmVec3Length(velocity);
   if (outIntersectionPoint) memcpy(outIntersectionPoint, &intersectionPoint, sizeof(kmVec3));
   return KM_TRUE;
}

static void _collisionEllipseCollideWithAABB(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
}

static void _collisionAABBCollideWithAABB(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
}

static int axisTest13_13_23(const kmTriangle *tri, const kmVec3 *edge, const kmVec3 *abs, const kmVec3 *boxHalf)
{
   float p1, p2, max, min, rad;

   p1 = edge->z*tri->v1.y - edge->y*tri->v1.z;
   p2 = edge->z*tri->v3.y - edge->y*tri->v3.z;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->z * boxHalf->y + abs->y * boxHalf->z;
   if (min > rad || max < -rad) return 0;

   p1 = -edge->z*tri->v1.x + edge->x*tri->v1.z;
   p2 = -edge->z*tri->v3.x + edge->x*tri->v3.z;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->z * boxHalf->x + abs->x * boxHalf->z;
   if (min > rad || max < -rad) return 0;

   p1 = edge->y*tri->v2.x + edge->x*tri->v2.y;
   p2 = edge->y*tri->v3.x + edge->x*tri->v3.y;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->y * boxHalf->x + abs->x * boxHalf->y;
   if (min > rad || max < -rad) return 0;

   return 1;
}

static int axisTest13_13_12(const kmTriangle *tri, const kmVec3 *edge, const kmVec3 *abs, const kmVec3 *boxHalf)
{
   float p1, p2, max, min, rad;

   p1 = edge->z*tri->v1.y - edge->y*tri->v1.z;
   p2 = edge->z*tri->v3.y - edge->y*tri->v3.z;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->z * boxHalf->y + abs->y * boxHalf->z;
   if (min > rad || max < -rad) return 0;

   p1 = -edge->z*tri->v1.x + edge->x*tri->v1.z;
   p2 = -edge->z*tri->v3.x + edge->x*tri->v3.z;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->z * boxHalf->x + abs->x * boxHalf->z;
   if (min > rad || max < -rad) return 0;

   p1 = edge->y*tri->v1.x + edge->x*tri->v1.y;
   p2 = edge->y*tri->v2.x + edge->x*tri->v2.y;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->y * boxHalf->x + abs->x * boxHalf->y;
   if (min > rad || max < -rad) return 0;

   return 1;
}

static int axisTest12_12_23(const kmTriangle *tri, const kmVec3 *edge, const kmVec3 *abs, const kmVec3 *boxHalf)
{
   float p1, p2, max, min, rad;

   p1 = edge->z*tri->v1.y - edge->y*tri->v1.z;
   p2 = edge->z*tri->v2.y - edge->y*tri->v2.z;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->z * boxHalf->y + abs->y * boxHalf->z;
   if (min > rad || max < -rad) return 0;

   p1 = -edge->z*tri->v1.x + edge->x*tri->v1.z;
   p2 = -edge->z*tri->v2.x + edge->x*tri->v2.z;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->z * boxHalf->x + abs->x * boxHalf->z;
   if (min > rad || max < -rad) return 0;

   p1 = edge->y*tri->v2.x + edge->x*tri->v2.y;
   p2 = edge->y*tri->v3.x + edge->x*tri->v3.y;
   if (p1 < p2) { min = p1; max = p2; } else { min = p2; max = p1; }
   rad = abs->y * boxHalf->x + abs->x * boxHalf->y;
   if (min > rad || max < -rad) return 0;

   return 1;
}

static kmBool kmAABBIntersectsTriangle2(const kmAABB *aabb, const kmTriangle *triangle, kmVec3 *outIntersectionPoint, kmScalar *outDistanceToCollision)
{
   kmVec3 center, absVec, normal, boxHalf, min, max;
   kmTriangle tri, edge;
   kmScalar d;

   if (outIntersectionPoint) memset(outIntersectionPoint, 0, sizeof(kmVec3));
   if (outDistanceToCollision) *outDistanceToCollision = 0;

   kmAABBCentre(aabb, &center);
   boxHalf.x = kmAABBDiameterX(aabb)*0.5;
   boxHalf.y = kmAABBDiameterY(aabb)*0.5;
   boxHalf.z = kmAABBDiameterZ(aabb)*0.5;

   kmVec3Subtract(&tri.v1, &triangle->v1, &center);
   kmVec3Subtract(&tri.v2, &triangle->v2, &center);
   kmVec3Subtract(&tri.v3, &triangle->v3, &center);

   kmVec3Subtract(&edge.v1, &tri.v2, &tri.v1);
   kmVec3Subtract(&edge.v2, &tri.v3, &tri.v2);
   kmVec3Subtract(&edge.v3, &tri.v1, &tri.v3);

   absVec.x = fabsf(edge.v1.x);
   absVec.y = fabsf(edge.v1.y);
   absVec.z = fabsf(edge.v1.z);
   if (!axisTest13_13_23(&tri, &edge.v1, &absVec, &boxHalf)) return KM_FALSE;

   absVec.x = fabsf(edge.v2.x);
   absVec.y = fabsf(edge.v2.y);
   absVec.z = fabsf(edge.v2.z);
   if (!axisTest13_13_12(&tri, &edge.v2, &absVec, &boxHalf)) return KM_FALSE;

   absVec.x = fabsf(edge.v3.x);
   absVec.y = fabsf(edge.v3.y);
   absVec.z = fabsf(edge.v3.z);
   if (!axisTest12_12_23(&tri, &edge.v3, &absVec, &boxHalf)) return KM_FALSE;

   kmVec3Assign(&max, &tri.v1);
   kmVec3Assign(&min, &tri.v1);
   kmVec3Max(&max, &max, &tri.v2);
   kmVec3Min(&min, &min, &tri.v2);
   kmVec3Max(&max, &max, &tri.v3);
   kmVec3Min(&min, &min, &tri.v3);

   if (min.x > boxHalf.x || max.x < -boxHalf.x) return KM_FALSE;
   if (min.y > boxHalf.y || max.y < -boxHalf.y) return KM_FALSE;
   if (min.z > boxHalf.z || max.z < -boxHalf.z) return KM_FALSE;

   kmVec3Cross(&normal, &edge.v1, &edge.v2);
   if (normal.x > 0.0f) {
      min.x = -boxHalf.x - tri.v1.x;
      max.x =  boxHalf.x - tri.v1.x;
   } else {
      min.x =  boxHalf.x - tri.v1.x;
      max.x = -boxHalf.x - tri.v1.x;
   }

   if (normal.y > 0.0f) {
      min.y = -boxHalf.y - tri.v1.y;
      max.y =  boxHalf.y - tri.v1.y;
   } else {
      min.y =  boxHalf.y - tri.v1.y;
      max.y = -boxHalf.y - tri.v1.y;
   }

   if (normal.z > 0.0f) {
      min.z = -boxHalf.z - tri.v1.z;
      max.z =  boxHalf.z - tri.v1.z;
   } else {
      min.z =  boxHalf.z - tri.v1.z;
      max.z = -boxHalf.z - tri.v1.z;
   }

   d = kmVec3Dot(&normal, &tri.v1);
   if (kmVec3Dot(&normal, &min)+d>0.0f) return KM_FALSE;
   if (kmVec3Dot(&normal, &max)+d>=-0.0f) {
      if (outIntersectionPoint) kmVec3Assign(outIntersectionPoint, &normal);
      return KM_TRUE;
   }

   return KM_FALSE;
}

static int _collisionTestAABBTriangle(const kmAABB *aabb, const kmTriangle *triangle, const kmVec3 *center, const CollisionInData *data)
{
   static const kmVec3 zero = {0,0,0};
   if (kmAABBIntersectsTriangle2(aabb, triangle, NULL, NULL) != KM_TRUE)
      return;

   puts("AABB COLLIDES TRIANGLE!");
   if (data->callback) {
      CollisionOutData out;
      memset(&out, 0, sizeof(CollisionOutData));

      kmPlane plane;
      kmRay3 ray;
      kmPlaneFromPoints(&plane, &triangle->v1, &triangle->v2, &triangle->v3);
      if (kmVec3AreEqual(&data->velocity, &zero)) {
         memcpy(&out.intersectionPoint, center, sizeof(kmVec3));
      } else {
         kmRay3FromPointAndDirection(&ray, center, &data->velocity);
         kmRay3IntersectPlane(&out.intersectionPoint, &ray, &plane);
      }

      kmVec3 diff;
      kmVec3Subtract(&diff, center, &out.intersectionPoint);
      kmScalar L = kmVec3Length(&diff);
      out.planeNormal.x = plane.a * L;
      out.planeNormal.y = plane.b * L;
      out.planeNormal.z = plane.c * L;
      kmVec3Normalize(&out.planeNormal, &out.planeNormal);
      printf("T1: %f, %f, %f\n", triangle->v1.x, triangle->v1.y, triangle->v1.z);
      printf("T2: %f, %f, %f\n", triangle->v2.x, triangle->v2.y, triangle->v2.z);
      printf("T3: %f, %f, %f\n", triangle->v3.x, triangle->v3.y, triangle->v3.z);
      printf("PLANE:  %f, %f, %f\n", plane.a, plane.b, plane.c);
      printf("NORMAL: %f, %f, %f\n", out.planeNormal.x, out.planeNormal.y, out.planeNormal.z);

      glhckObjectPosition(wall, &triangle->v1);
      glhckObjectRender(wall);
      glhckObjectPosition(wall, &triangle->v2);
      glhckObjectRender(wall);
      glhckObjectPosition(wall, &triangle->v3);
      glhckObjectRender(wall);
      glhckObjectPosition(wall, &out.intersectionPoint);
      glhckObjectRender(wall);

      out.triangle = triangle;
      out.userdata = data->userdata;
      data->callback(&out);
   }
}

static int _collisionMeshCollideWithAABB(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
   const CollisionInData *data = packet->data;
   const kmAABB *aabb = &packet->aabb;
   const kmMat4 *matrix;
   kmTriangle triangle, tt;
   kmVec3 center, bPosVel;
   kmAABB bAABB, tAABB;
   glhckGeometry *g;
   int i, ix;

   /* get geometry for better check */
   if (!(g = glhckObjectGetGeometry(primitive->data.mesh)))
      return 0;

   /* prepare test aabb */
   kmAABBCentre(aabb, &center);
   kmVec3Add(&bPosVel, &center, &data->velocity);
   kmAABBAssign(&bAABB, aabb);
   kmVec3Min(&bAABB.min, &bAABB.min, &bPosVel);
   kmVec3Max(&bAABB.max, &bAABB.max, &bPosVel);

   matrix = glhckObjectGetMatrix(primitive->data.mesh);
   for (i = 0; i+2 < g->indexCount; i+=3) {
      ix = glhckGeometryGetVertexIndexForIndex(g, i+0);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v1, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+1);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v2, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+2);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v3, NULL, NULL, NULL);
      kmVec3MultiplyMat4(&tt.v1, &triangle.v1, matrix);
      kmVec3MultiplyMat4(&tt.v2, &triangle.v2, matrix);
      kmVec3MultiplyMat4(&tt.v3, &triangle.v3, matrix);
      _collisionTestAABBTriangle(&bAABB, &tt, &center, data);

      ix = glhckGeometryGetVertexIndexForIndex(g, i+2);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v1, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+1);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v2, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+0);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v3, NULL, NULL, NULL);
      kmVec3MultiplyMat4(&tt.v1, &triangle.v1, matrix);
      kmVec3MultiplyMat4(&tt.v2, &triangle.v2, matrix);
      kmVec3MultiplyMat4(&tt.v3, &triangle.v3, matrix);
      _collisionTestAABBTriangle(&bAABB, &tt, &center, data);

      ix = glhckGeometryGetVertexIndexForIndex(g, i+1);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v1, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+0);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v2, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+2);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v3, NULL, NULL, NULL);
      kmVec3MultiplyMat4(&tt.v1, &triangle.v1, matrix);
      kmVec3MultiplyMat4(&tt.v2, &triangle.v2, matrix);
      kmVec3MultiplyMat4(&tt.v3, &triangle.v3, matrix);
      _collisionTestAABBTriangle(&bAABB, &tt, &center, data);
   }
}

static void _collisionEllipseCollideWithEllipse(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
}

static int _collisionAABBCollideWithEllipse(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
}

#if 0
static void _collisionResponseWithWorld(CollisionWorld *object)
{
   kmScalar unitScale = object->unitsPerMeter / 100.0f;
   kmScalar veryCloseDistance = 0.005f * unitScale;

   /* recursion check here */
   if (packet->collisionRecursionDepth > 5)
      return;

   /* check collision */

   /* if no collision, move and return */
}
#endif

static int _collisionMeshCollideWithEllipse(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
   const CollisionInData *data = packet->data;
   const kmEllipse *ellipse = packet->primitive.ellipse;
   const kmMat4 *matrix;
   kmVec3 eSpacePosition, eSpaceVelocity, ePosVel;
   kmTriangle triangle, tt;
   kmAABB eAABB, tAABB;
   glhckGeometry *g;
   kmMat4 smatrix;
   int i, ix;

   /* get geometry for better check */
   if (!(g = glhckObjectGetGeometry(primitive->data.mesh)))
      return 0;

   /* convert to ellipse space */
   kmVec3Divide(&eSpacePosition, &ellipse->point, &ellipse->radius);
   kmVec3Divide(&eSpaceVelocity, &data->velocity, &ellipse->radius);
   kmVec3Add(&ePosVel, &eSpacePosition, &eSpaceVelocity);
   kmMat4Scaling(&smatrix, 1.0/ellipse->radius.x, 1.0/ellipse->radius.y, 1.0/ellipse->radius.z);

   kmVec3Min(&eAABB.min, &eAABB.min, &eSpacePosition);
   kmVec3Min(&eAABB.min, &eAABB.min, &ePosVel);
   kmVec3Max(&eAABB.max, &eAABB.max, &eSpacePosition);
   kmVec3Max(&eAABB.max, &eAABB.max, &ePosVel);
   kmVec3Subtract(&eAABB.min, &eAABB.min, &ellipse->radius);
   kmVec3Add(&eAABB.max, &eAABB.max, &ellipse->radius);

   matrix = glhckObjectGetMatrix(primitive->data.mesh);
   for (i = 0; i+2 < g->indexCount; i+=3) {
      ix = glhckGeometryGetVertexIndexForIndex(g, i);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v1, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+1);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v2, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+2);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v3, NULL, NULL, NULL);

      /* transform to object matrix */
      kmVec3MultiplyMat4(&triangle.v1, &triangle.v1, matrix);
      kmVec3MultiplyMat4(&triangle.v2, &triangle.v2, matrix);
      kmVec3MultiplyMat4(&triangle.v3, &triangle.v3, matrix);
      kmVec3MultiplyMat4(&tt.v1, &triangle.v1, &smatrix);
      kmVec3MultiplyMat4(&tt.v2, &triangle.v2, &smatrix);
      kmVec3MultiplyMat4(&tt.v3, &triangle.v3, &smatrix);

      kmVec3Min(&tAABB.min, &tAABB.min, &tt.v1);
      kmVec3Min(&tAABB.min, &tAABB.min, &tt.v2);
      kmVec3Min(&tAABB.min, &tAABB.min, &tt.v3);
      kmVec3Max(&tAABB.max, &tAABB.max, &tt.v1);
      kmVec3Max(&tAABB.max, &tAABB.max, &tt.v2);
      kmVec3Max(&tAABB.max, &tAABB.max, &tt.v3);

      if (kmAABBContainsAABB(&tAABB, &eAABB) == KM_CONTAINS_NONE)
         continue;

      if (kmEllipseCollidesTriangle(&eSpacePosition, &tt, &eSpaceVelocity, NULL, NULL)) {
         puts("ELLIPSE COLLIDES TRIANGLE!");
         glhckObjectPosition(wall, &triangle.v1);
         glhckObjectRender(wall);
         glhckObjectPosition(wall, &triangle.v2);
         glhckObjectRender(wall);
         glhckObjectPosition(wall, &triangle.v3);
         glhckObjectRender(wall);
      }
   }
}

static void _collisionWorldCollideWithAABB(CollisionWorld *object, _CollisionPacket *packet)
{
   _CollisionPrimitive *p;
   for (p = object->primitives; p; p = p->next) {
      // if (kmAABBContainsAABB(p->aabb, &packet->aabb) == KM_CONTAINS_NONE)
         // continue;

      switch (p->type) {
         case COLLISION_ELLIPSE:
            _collisionEllipseCollideWithAABB(p, packet);
            break;
         case COLLISION_AABB:
            _collisionAABBCollideWithAABB(p, packet);
            break;
         case COLLISION_MESH:
            _collisionMeshCollideWithAABB(p, packet);
            break;
         default:break;
      }
   }
}

static void _collisionWorldCollideWithEllipse(CollisionWorld *object, _CollisionPacket *packet)
{
   _CollisionPrimitive *p;
   for (p = object->primitives; p; p = p->next) {
      if (kmAABBContainsAABB(p->aabb, &packet->aabb) == KM_CONTAINS_NONE)
         continue;

      switch (p->type) {
         case COLLISION_ELLIPSE:
            _collisionEllipseCollideWithEllipse(p, packet);
            break;
         case COLLISION_AABB:
            _collisionAABBCollideWithEllipse(p, packet);
            break;
         case COLLISION_MESH:
            _collisionMeshCollideWithEllipse(p, packet);
            break;
         default:break;
      }
   }
}

static void _collisionWorldAddPrimitive(CollisionWorld *object, _CollisionPrimitive *primitive)
{
   _CollisionPrimitive *p;
   assert(object && primitive);

   if (!(p = object->primitives))
      object->primitives = primitive;
   else {
      for (; p && p->next; p = p->next);
      p->next = primitive;
   }
}

CollisionWorld* collisionWorldNew(void)
{
   CollisionWorld *object;

   if (!(object = calloc(1, sizeof(CollisionWorld))))
      goto fail;

   object->unitsPerMeter = 100.0f;
   return object;

fail:
   return NULL;
}

void collisionWorldFree(CollisionWorld *object)
{
   _CollisionPrimitive *p, *pn;
   assert(object);

   for (p = object->primitives; p; p = pn) {
      pn = p->next;
      free(p);
   }

   free(object);
}

CollisionPrimitive* collisionWorldAddEllipse(CollisionWorld *object, const kmEllipse *ellipse)
{
   kmAABB *aabb = NULL;
   kmEllipse *ellipseCopy = NULL;
   _CollisionPrimitive *primitive = NULL;
   assert(object && ellipse);

   if (!(primitive = calloc(1, sizeof(_CollisionPrimitive))))
      goto fail;

   if (!(ellipseCopy = malloc(sizeof(kmEllipse))))
      goto fail;

   if (!(aabb = malloc(sizeof(kmAABB))))
      goto fail;

   kmAABBInitialize(aabb, &ellipse->point, ellipse->radius.x, ellipse->radius.y, ellipse->radius.z);
   memcpy(ellipseCopy, ellipse, sizeof(kmEllipse));
   primitive->type = COLLISION_ELLIPSE;
   primitive->data.ellipse = ellipseCopy;
   primitive->aabb = aabb;
   _collisionWorldAddPrimitive(object, primitive);
   return primitive;

fail:
   IFDO(free, primitive);
   IFDO(free, ellipseCopy);
   IFDO(free, aabb);
   return NULL;
}

CollisionPrimitive* collisionWorldAddAABB(CollisionWorld *object, const kmAABB *aabb)
{
   kmAABB *aabbCopy = NULL;
   _CollisionPrimitive *primitive = NULL;
   assert(object && aabb);

   if (!(primitive = calloc(1, sizeof(_CollisionPrimitive))))
      goto fail;

   if (!(aabbCopy = malloc(sizeof(kmAABB))))
      goto fail;

   memcpy(aabbCopy, aabb, sizeof(kmAABB));
   primitive->type = COLLISION_AABB;
   primitive->data.aabb = aabbCopy;
   primitive->aabb = aabbCopy;
   _collisionWorldAddPrimitive(object, primitive);
   return primitive;

fail:
   IFDO(free, primitive);
   IFDO(free, aabbCopy);
   return NULL;
}

const CollisionPrimitive* collisionWorldAddObject(CollisionWorld *object, glhckObject *gobject)
{
   _CollisionPrimitive *primitive = NULL;
   assert(object && gobject);

   if (!(primitive = calloc(1, sizeof(_CollisionPrimitive))))
      goto fail;

   primitive->type = COLLISION_MESH;
   primitive->data.mesh = glhckObjectRef(gobject);
   primitive->aabb = (kmAABB*)glhckObjectGetAABB(gobject);
   _collisionWorldAddPrimitive(object, primitive);
   return primitive;

fail:
   IFDO(free, primitive);
   return NULL;
}

void collisionWorldRemovePrimitive(CollisionWorld *object, CollisionPrimitive *primitive)
{
   _CollisionPrimitive *p;
   assert(object && primitive);

   if (primitive == (p = object->primitives))
      object->primitives = primitive->next;
   else {
      for (; p && p->next != primitive; p = p->next);
      if (p) p->next = primitive->next;
      else object->primitives = NULL;
   }

   switch (primitive->type) {
      case COLLISION_MESH:
         IFDO(glhckObjectFree, primitive->data.mesh);
         break;
      case COLLISION_ELLIPSE:
         IFDO(free, primitive->aabb);
      default:
         IFDO(free, primitive->data.any);
         break;
   }
   IFDO(free, primitive);
}

void collisionWorldCollideEllipse(CollisionWorld *object, const kmEllipse *ellipse, const CollisionInData *data)
{
   static kmVec3 zero = {0,0,0};
   _CollisionPacket packet;
   if (kmVec3AreEqual(&data->velocity, &zero))
      return;

   packet.type = COLLISION_ELLIPSE;
   packet.primitive.ellipse = ellipse;
   kmAABBInitialize(&packet.aabb, &ellipse->point, ellipse->radius.x, ellipse->radius.y, ellipse->radius.z);
   packet.data = data;
   packet.nearestDistance = FLT_MAX;
   _collisionWorldCollideWithEllipse(object, &packet);
}

void collisionWorldCollideAABB(CollisionWorld *object, const kmAABB *aabb, const CollisionInData *data)
{
   static kmVec3 zero = {0,0,0};
   _CollisionPacket packet;
   if (kmVec3AreEqual(&data->velocity, &zero))
      return;

   packet.type = COLLISION_AABB;
   memcpy(&packet.aabb, aabb, sizeof(kmAABB));
   packet.data = data;
   packet.nearestDistance = FLT_MAX;
   _collisionWorldCollideWithAABB(object, &packet);
}
static CollisionWorld *world = NULL;
#endif

enum {
   CAMERA_NONE       = 0,
   CAMERA_UP         = 1,
   CAMERA_DOWN       = 2,
   CAMERA_LEFT       = 4,
   CAMERA_RIGHT      = 8,
   CAMERA_TURN_LEFT  = 16,
   CAMERA_TURN_RIGHT = 32,
   CAMERA_SLIDE      = 64,
};

typedef struct GameActor {
   kmVec3 rotation;
   kmVec3 position;
   kmVec3 lastPosition;
   kmVec3 toPosition;
   float speed;
   float fallingSpeed;
   float toRotation;
   float lastActTime;
   float swordY;
   char swordD;
   glhckObject *object;
   glhckObject *sword;
   unsigned char flags, lastFlags;
   char shouldInterpolate;
} GameActor;

typedef struct GameCamera {
   glhckCamera *object;
   kmVec3 rotation;
   kmVec3 addRotation;
   kmVec3 position;
   kmVec3 offset;
   float radius;
   float speed;
   float rotationSpeed;
   unsigned char flags, lastFlags;
} GameCamera;

typedef struct Client {
   GameActor actor;
   unsigned int clientId;
   char host[45];
   struct Client *next;
} Client;

typedef struct ClientMaterials {
   glhckMaterial *me;
   glhckMaterial *player;
   glhckMaterial *wall;
} ClientMaterials;

typedef struct ClientData {
   GameCamera camera;
   float delta;
   ENetHost *client;
   ENetPeer *peer;
   Client *me;
   Client *clients;
   ClientMaterials materials;
} ClientData;

static inline kmVec3* kmVec3Interpolate(kmVec3* pOut, const kmVec3* pIn, const kmVec3* other, float d)
{
   const float inv = 1.0f - d;
   pOut->x = pIn->x*inv + other->x*d;
   pOut->y = pIn->y*inv + other->y*d;
   pOut->z = pIn->z*inv + other->z*d;
   return pOut;
}

static float floatInterpolate(float f, float o, float d)
{
   const float inv = 1.0f -d;
   return f*inv + o*d;
}

static void closeCallback(GLFWwindow* window)
{
   RUNNING = 0;
}

static void resizeCallback(GLFWwindow* window, int width, int height)
{
   WIDTH = width; HEIGHT = height;
   glhckDisplayResize(width, height);
}

static Client* gameNewClient(ClientData *data, Client *params)
{
   Client *c;

   /* add to list */
   for (c = data->clients; c && c->next; c = c->next);
   if (c) c = c->next = malloc(sizeof(Client));
   else c = data->clients = malloc(sizeof(Client));

   memcpy(c, params, sizeof(Client));
   return c;
}

static void gameFreeClient(ClientData *data, Client *client)
{
   Client *c;

   /* remove from list */
   for (c = data->clients; c != client && c->next != client; c = c->next);
   if (c == client) data->clients = client->next;
   else if (c) c->next = client->next;

   free(client);
}

static Client* clientForId(ClientData *data, unsigned int id)
{
   Client *c;
   for (c = data->clients; c && c->clientId != id; c = c->next);
   printf("%s %u\n", c?"Found id":"Did not found id", id);
   return c;
}

static void initClientData(ClientData *data)
{
   Client client;
   assert(data);
   memset(data, 0, sizeof(ClientData));
   memset(&client, 0, sizeof(Client));
   data->me = gameNewClient(data, &client);
}

static void gameSend(ClientData *data, unsigned char *pdata, size_t size, ENetPacketFlag flag)
{
   ENetPacket *packet;
   PacketGeneric *generic = (PacketGeneric*)pdata;
   packet = enet_packet_create(pdata, size, flag);
   enet_peer_send(data->peer, 0, packet);
}

static int initEnet(const char *host_ip, const int host_port, ClientData *data)
{
   ENetAddress address;
   ENetEvent event;
   assert(host_ip && data && data->me);

   if (enet_initialize() != 0) {
      fprintf (stderr, "An error occurred while initializing ENet.\n");
      return RETURN_FAIL;
   }

   data->client = enet_host_create(NULL,
         1     /* 1 outgoing connection */,
         2     /* max channels */,
         0     /* download bandwidth */,
         0     /* upload bandwidth */);

   if (!data->client) {
      fprintf (stderr,
            "An error occurred while trying to create an ENet client host.\n");
      return RETURN_FAIL;
   }

   /* Enable compression */
   data->client->checksum = enet_crc32;
   enet_host_compress_with_range_coder(data->client);

   /* Connect to some.server.net:1234. */
   enet_address_set_host(&address, host_ip);
   address.port = host_port;

   /* Initiate the connection, allocating the two channels 0 and 1. */
   data->peer = enet_host_connect(data->client, &address, 2, 0);

   if (!data->peer) {
      fprintf (stderr,
            "No available peers for initiating an ENet connection.\n");
      return RETURN_FAIL;;
   }

   /* Wait up to 5 seconds for the connection attempt to succeed. */
   if (enet_host_service(data->client, &event, 5000) > 0 &&
         event.type == ENET_EVENT_TYPE_CONNECT)
   {
      fprintf(stderr,
            "Connection to %s:%d succeeded.\n", host_ip, host_port);
   } else {
      /* Either the 5 seconds are up or a disconnect event was */
      /* received. Reset the peer in the event the 5 seconds   */
      /* had run out without any significant event.            */
      enet_peer_reset(data->peer);
      fprintf(stderr,
            "Connection to %s:%d failed.\n", host_ip, host_port);
      return RETURN_FAIL;
   }

   /* store our client id */
   data->me->clientId = data->peer->connectID;
   strncpy(data->me->host, "127.0.0.1", sizeof(data->me->host));
   printf("My ID is %u\n", data->me->clientId);

   return RETURN_OK;
}

static void disconnectEnet(ClientData *data)
{
   ENetEvent event;
   assert(data);
   enet_peer_disconnect(data->peer, 0);

   while (enet_host_service(data->client, &event, 3000) > 0) {
      switch (event.type) {
         case ENET_EVENT_TYPE_RECEIVE:
            enet_packet_destroy(event.packet);
            break;

         case ENET_EVENT_TYPE_DISCONNECT:
            puts("Disconnection succeeded.");
            return;
      }
   }

   /* We've arrived here, so the disconnect attempt didn't */
   /* succeed yet.  Force the connection down.             */
   enet_peer_reset(data->peer);
}

static int deinitEnet(ClientData *data)
{
   assert(data);
   disconnectEnet(data);
   enet_host_destroy(data->client);
   return RETURN_OK;
}

static void handleJoin(ClientData *data, ENetEvent *event)
{
   Client client;
   PacketServerClientInformation *packet = (PacketServerClientInformation*)event->packet->data;

   memset(&client, 0, sizeof(Client));
   client.actor.object = glhckCubeNew(1.0f);
   glhckObjectScalef(client.actor.object, 1.0f, 3.0f, 1.0f);
   client.actor.speed = data->me->actor.speed;
   strncpy(client.host, packet->host, sizeof(client.host));
   client.clientId = packet->clientId;

   glhckObjectMaterial(client.actor.object, data->materials.player);
   gameNewClient(data, &client);
   printf("Client [%u] (%s) joined!\n", client.clientId, client.host);
}

static void handlePart(ClientData *data, ENetEvent *event)
{
   Client *client;
   PacketServerClientPart *packet = (PacketServerClientPart*)event->packet->data;

   if (!(client = clientForId(data, packet->clientId)))
      return;

   printf("Client [%u] (%s) parted!\n", client->clientId, client->host);
   glhckObjectFree(client->actor.object);
   gameFreeClient(data, client);
   event->peer->data = NULL;
}

static void gameActorApplyPacket(ClientData *data, GameActor *actor, PacketServerActorState *packet)
{
   actor->flags = packet->flags;
   actor->toRotation = TODEGS(packet->rotation);
}

static void handleState(ClientData *data, ENetEvent *event)
{
   Client *client;
   PacketServerActorState *packet = (PacketServerActorState*)event->packet->data;

   if (!(client = clientForId(data, packet->clientId)))
      return;

   gameActorApplyPacket(data, &client->actor, packet);
}

static void handleFullState(ClientData *data, ENetEvent *event)
{
   Client *client;
   PacketServerActorFullState *packet = (PacketServerActorFullState*)event->packet->data;

   if (!(client = clientForId(data, packet->clientId)))
      return;

   gameActorApplyPacket(data, &client->actor, (PacketServerActorState*)packet); /* handle the delta part */
   client->actor.toPosition.x = packet->position.x;
   client->actor.toPosition.y = packet->position.y;
   client->actor.toPosition.z = packet->position.z;
   if (!client->actor.shouldInterpolate) {
      client->actor.rotation.y = client->actor.toRotation;
      memcpy(&client->actor.position, &client->actor.toPosition, sizeof(kmVec3));
      client->actor.shouldInterpolate = 1;
   }
   printf("GOT FULL STATE\n");
}

static int manageEnet(ClientData *data)
{
   ENetEvent event;
   PacketServerGeneric *packet;
   assert(data);

   /* Wait up to 1000 milliseconds for an event. */
   while (enet_host_service(data->client, &event, 0) > 0) {
      switch (event.type) {
         case ENET_EVENT_TYPE_RECEIVE:
            /* discard bad packets */
            if (event.packet->dataLength < sizeof(PacketServerGeneric))
               break;

            printf("A packet of length %u was received on channel %u.\n",
                  event.packet->dataLength,
                  event.channelID);

            /* handle packet */
            packet = (PacketServerGeneric*)event.packet->data;
            packet->clientId = ntohl(packet->clientId);
            switch (packet->id) {
               case PACKET_ID_CLIENT_INFORMATION:
                  handleJoin(data, &event);
                  break;
               case PACKET_ID_CLIENT_PART:
                  handlePart(data, &event);
                  break;
               case PACKET_ID_ACTOR_STATE:
                  handleState(data, &event);
                  break;
               case PACKET_ID_ACTOR_FULL_STATE:
                  handleFullState(data, &event);
                  break;
            }

            /* Clean up the packet now that we're done using it. */
            enet_packet_destroy(event.packet);
            break;
      }
   }

   return RETURN_OK;
}

int gameActorFlagsIsMoving(unsigned char flags)
{
   return (flags & ACTOR_FORWARD || flags & ACTOR_BACKWARD);
}

void gameCameraUpdate(ClientData *data, GameCamera *camera, GameActor *target)
{
   float speed = camera->speed * data->delta;
   float rotationSpeed = camera->rotationSpeed * data->delta;
   kmVec3Assign(&camera->position, &target->position);
   camera->rotation.z = target->rotation.z;

   if (camera->flags & CAMERA_UP) {
      camera->rotation.x += speed;
      camera->flags &= ~CAMERA_SLIDE;
   }
   if (camera->flags & CAMERA_DOWN) {
      camera->rotation.x -= speed;
      camera->flags &= ~CAMERA_SLIDE;
   }

   if (camera->flags & CAMERA_RIGHT) {
      camera->rotation.y -= rotationSpeed;
      target->flags |= ACTOR_RIGHT;
   }
   if (camera->flags & CAMERA_LEFT) {
      camera->rotation.y += rotationSpeed;
      target->flags |= ACTOR_LEFT;
   }

   if (!gameActorFlagsIsMoving(target->flags)) {
      if (camera->flags & CAMERA_TURN_RIGHT) {
         camera->rotation.y -= rotationSpeed;
         target->flags |= ACTOR_RIGHT;
      }
      if (camera->flags & CAMERA_TURN_LEFT) {
         camera->rotation.y += rotationSpeed;
         target->flags |= ACTOR_LEFT;
      }
   } else {
      if (camera->flags & CAMERA_TURN_RIGHT) {
         camera->addRotation.y -= rotationSpeed;
         camera->flags &= ~CAMERA_SLIDE;
      }

      if (camera->flags & CAMERA_TURN_LEFT) {
         camera->addRotation.y += rotationSpeed;
         camera->flags &= ~CAMERA_SLIDE;
      }
   }

   if (camera->flags & CAMERA_SLIDE) {
      kmVec3 targetRotation = {0.0f,0.0f,0.0f};
      kmVec3Interpolate(&camera->addRotation, &camera->addRotation, &targetRotation, 0.08f);
   }

   camera->position.x += cos(kmDegreesToRadians(camera->addRotation.y + camera->rotation.y + 90)) * camera->radius;
   camera->position.z -= sin(kmDegreesToRadians(camera->addRotation.y + camera->rotation.y + 90)) * camera->radius;
   camera->position.y += camera->rotation.x;

   if (camera->rotation.x > 30) camera->rotation.x = 30;
   if (camera->rotation.x < 0) camera->rotation.x  = 0;

   kmVec3 targetPosition = {0.0f,0.0f,0.0f};
   kmVec3Assign(&targetPosition, &target->position);
   kmVec3Add(&targetPosition, &targetPosition, &camera->offset);
   glhckObject *internalObject = glhckCameraGetObject(camera->object);
   glhckObjectTarget(internalObject, &targetPosition);
   glhckObjectPosition(internalObject, &camera->position);
}

#if 0
void gameActorCollisionResponse(const CollisionOutData *data)
{
   GameActor *actor = (GameActor*)data->userdata;
   kmVec3 diff;
   if (data->planeNormal.y > 0.0f) actor->fallingSpeed = 0.0f;
   kmVec3Subtract(&actor->toPosition, &actor->toPosition, &data->planeNormal);
}
#endif

void gameActorUpdate(ClientData *data, GameActor *actor)
{
   float speed = actor->speed * data->delta * ((actor->flags & ACTOR_SPRINT)?2.0f:1.0f);

   if (actor != &data->me->actor) {
      float cspeed = data->camera.rotationSpeed * data->delta; /* camera speed for other players */
      if (actor->flags & ACTOR_LEFT) {
         actor->toRotation += cspeed;
      }
      if (actor->flags & ACTOR_RIGHT) {
         actor->toRotation -= cspeed;
      }
   }

   if (actor->flags & ACTOR_JUMP) {
      actor->toPosition.y += speed*8;
      actor->fallingSpeed = 0.0f;
      actor->flags &= ~ACTOR_JUMP;
   } else {
      actor->toPosition.y -= actor->fallingSpeed;
      actor->fallingSpeed += speed * 0.01f;
   }

   if (actor->flags & ACTOR_FORWARD) {
      actor->toPosition.x -= speed * cosf(kmDegreesToRadians(actor->toRotation + 90));
      actor->toPosition.z += speed * sinf(kmDegreesToRadians(actor->toRotation + 90));

      kmVec3 targetRotation = {0.0f,actor->toRotation,0.0f};
#if 0 /* the interpolation needs to wrap around 360 */
      kmVec3Interpolate(&actor->rotation, &actor->rotation, &targetRotation, 0.18f);
#else
      kmVec3Assign(&actor->rotation, &targetRotation);
#endif
   }

   if (actor->flags & ACTOR_BACKWARD) {
      actor->toPosition.x += speed * cosf(kmDegreesToRadians(actor->toRotation + 90));
      actor->toPosition.z -= speed * sinf(kmDegreesToRadians(actor->toRotation + 90));

      kmVec3 targetRotation = {0.0f,actor->toRotation + 180.0f,0.0f};
#if 0 /* the interpolation needs to wrap around 360 */
      kmVec3Interpolate(&actor->rotation, &actor->rotation, &targetRotation, 0.18f);
#else
      kmVec3Assign(&actor->rotation, &targetRotation);
#endif
   }

   /* awesome attack */
   if (actor->flags & ACTOR_ATTACK) {
      if (!actor->sword) {
         actor->sword = glhckObjectNew();
         glhckObjectAddChild(actor->object, actor->sword);

         actor->swordY = 0.0f;
         actor->swordD = !actor->swordD;
         glhckObject *sword = glhckCubeNew(1.0f);
         glhckObjectAddChild(actor->sword, sword);
         glhckObjectScalef(sword, 0.1f, 0.1f, 5.0f);
         glhckObjectPositionf(sword, 0, 0, 8.0f);
         glhckObjectFree(sword);
      }
   }

#if 0
   CollisionInData colInData;
   memset(&colInData, 0, sizeof(CollisionInData));
   colInData.userdata = actor;
   colInData.callback = gameActorCollisionResponse;
   kmVec3Subtract(&colInData.velocity, &actor->toPosition, &actor->position);

#if 1
   collisionWorldCollideAABB(world, glhckObjectGetAABB(actor->object), &colInData);
#else
   kmEllipse ellipse;
   const kmAABB *aabb = glhckObjectGetAABB(actor->object);
   kmVec3Assign(&ellipse.point, &actor->position);
   ellipse.radius.x = kmAABBDiameterX(aabb)*0.5;
   ellipse.radius.y = kmAABBDiameterY(aabb)*0.5;
   ellipse.radius.z = kmAABBDiameterZ(aabb)*0.5;
   collisionWorldCollideEllipse(world, &ellipse, &colInData);
#endif
#endif

   /* assign last position */
   kmVec3Assign(&actor->lastPosition, &actor->position);

   /* limit */
   if (actor->toPosition.y < 0.0f) {
      actor->toPosition.y = 0.0f;
      actor->fallingSpeed = 0.0f;
   }

   /* movement interpolation */
   if (gameActorFlagsIsMoving(actor->flags)) {
      kmVec3Interpolate(&actor->position, &actor->position, &actor->toPosition, 0.25f);
   } else {
      kmVec3Interpolate(&actor->position, &actor->position, &actor->toPosition, 0.1f);
   }

   glhckObjectRotation(actor->object, &actor->rotation);
   glhckObjectPosition(actor->object, &actor->position);

   if (actor->sword) {
      if (!actor->swordD) {
         glhckObjectRotationf(actor->sword, 0, cosf(actor->swordY)*140.0f, 0);
      } else {
         glhckObjectRotationf(actor->sword, -120.0f+sinf(actor->swordY)*140.0f, 0, 0);
      }

      if (!actor->swordD) actor->swordY += 15.0f * data->delta;
      else actor->swordY += 8.0f * data->delta;
      if (actor->swordY > (!actor->swordD?3.0f:1.5f)) {
         glhckObjectRemoveChildren(actor->sword);
         glhckObjectRemoveChild(actor->object, actor->sword);
         glhckObjectFree(actor->sword);
         actor->sword = NULL;
      }
   }
}

void gameActorUpdateFrom3rdPersonCamera(ClientData *data, GameActor *actor, GameCamera *camera)
{
   if (gameActorFlagsIsMoving(actor->flags) != gameActorFlagsIsMoving(actor->lastFlags)) {
      if (!gameActorFlagsIsMoving(actor->flags)) {
         camera->flags |= CAMERA_SLIDE;
      } else {
         camera->rotation.y = camera->rotation.y + camera->addRotation.y;
         camera->addRotation.y = 0.0f;
         camera->flags &= ~CAMERA_SLIDE;
      }
   }

   /* convert bams and back to resemble the rotation sent to server */
   unsigned char bams = TOBAMS(camera->rotation.y); bams &= 0xff;
   actor->toRotation  = TODEGS(bams);
   gameActorUpdate(data, actor);
}

void gameSendPlayerState(ClientData *data)
{
   PacketActorState state;
   memset(&state, 0, sizeof(PacketActorState));
   state.id = PACKET_ID_ACTOR_STATE;
   state.flags = data->me->actor.flags;
   state.rotation = TOBAMS(data->me->actor.toRotation);
   state.rotation &= 0xff;
   gameSend(data, (unsigned char*)&state, sizeof(PacketActorState), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
}

void gameSendFullPlayerState(ClientData *data)
{
   PacketActorFullState state;
   memset(&state, 0, sizeof(PacketActorFullState));
   state.id = PACKET_ID_ACTOR_FULL_STATE;
   state.flags = data->me->actor.flags;
   state.rotation = TOBAMS(data->me->actor.toRotation);
   state.rotation &= 0xff;
   state.position.x = data->me->actor.toPosition.x;
   state.position.y = data->me->actor.toPosition.y;
   state.position.z = data->me->actor.toPosition.z;
   gameSend(data, (unsigned char*)&state, sizeof(PacketActorFullState), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
}

int main(int argc, char **argv)
{
   /* global data */
   ClientData data;
   GLFWwindow* window;
   float          now          = 0;
   float          last         = 0;
   unsigned int   frameCounter = 0;
   unsigned int   FPS          = 0;
   unsigned int   fpsDelay     = 0;
   float          duration     = 0;
   char           WIN_TITLE[256];
   memset(WIN_TITLE, 0, sizeof(WIN_TITLE));
   initClientData(&data);

   if (!glfwInit())
      return EXIT_FAILURE;

   glhckCompileFeatures features;
   glhckGetCompileFeatures(&features);
   if (features.render.glesv1 || features.render.glesv2) {
      glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
      glfwWindowHint(GLFW_DEPTH_BITS, 16);
   }
   if (features.render.glesv2) {
      glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
   }
   if (features.render.opengl) {
      glfwWindowHint(GLFW_SAMPLES, 4);
      glfwWindowHint(GLFW_DEPTH_BITS, 24);
   }
   if (!(window = glfwCreateWindow(WIDTH, HEIGHT, "srv.birth", NULL, NULL)))
      return EXIT_FAILURE;

   glfwSetWindowCloseCallback(window, closeCallback);
   glfwSetWindowSizeCallback(window, resizeCallback);
   glfwMakeContextCurrent(window);

   if (!glhckContextCreate(argc, argv))
      return EXIT_FAILURE;

   if (!glhckDisplayCreate(WIDTH, HEIGHT,
            (argc>1&&!strcmp(argv[1], "-fixpipe")?GLHCK_RENDER_OPENGL_FIXED_PIPELINE:GLHCK_RENDER_AUTO)))
      return EXIT_FAILURE;

   glfwSwapInterval(0);
   glEnable(GL_MULTISAMPLE);

   glhckText *text = glhckTextNew(512, 512);
   glhckTextColorb(text, 255, 255, 255, 255);
   unsigned int font = glhckTextFontNewKakwafont(text, NULL);

   glhckObject *cube = glhckCubeNew(1.0);
   glhckMaterial *cubeMat = glhckMaterialNew(NULL);
   glhckObjectMaterial(cube, cubeMat);
   glhckMaterialFree(cubeMat);

   glhckObject *horizon = glhckPlaneNew(128, 1);
   glhckObjectPositionf(horizon, 0, -7, -53);
   glhckMaterial *horizonMat = glhckMaterialNew(glhckTextureNewFromFile("media/gradient.png", NULL, glhckTextureDefaultSpriteParameters()));
   glhckObjectMaterial(horizon, horizonMat);
   glhckMaterialFree(horizonMat);

   glhckObject *water = glhckPlaneNew(128, 128);
   glhckObjectPositionf(water, 0, -8, 0);
   glhckObjectRotatef(water, -90, 0, 0);

   glhckMaterial *waterMat = glhckMaterialNew(glhckTextureNewFromFile("media/water.jpg", NULL, NULL));
   glhckObjectMaterial(water, waterMat);
   glhckMaterialFree(waterMat);
   glhckMaterialTextureScalef(waterMat, 4.0f, 4.0f);
   glhckMaterialDiffuseb(waterMat, 120, 120, 120, 255);
   glhckMaterialBlendFunc(waterMat, GLHCK_ONE, GLHCK_ONE);

   glhckCamera *menuCamera = glhckCameraNew();
   glhckCameraRange(menuCamera, 1.0f, 500.0f);
   glhckCameraFov(menuCamera, 32.0f);

   RUNNING = 1;
   float waterPos = 0.0f, horizonPos = 0.0f, textPos = 0.0f;
   while (RUNNING && glfwGetKey(window, GLFW_KEY_ESCAPE) != GLFW_PRESS && glfwGetKey(window, GLFW_KEY_ENTER) != GLFW_PRESS) {
      last       = now;
      now        = glfwGetTime();
      data.delta = now - last;
      glfwPollEvents();

      glhckCameraUpdate(menuCamera);
      glhckRenderPass(glfwGetKey(window, GLFW_KEY_O)?GLHCK_PASS_OVERDRAW:glhckRenderPassDefaults());

      horizonPos = floatInterpolate(horizonPos, 53.0f, data.delta*1.8);
      if (horizonPos < 53.0f) {
         glhckObjectPositionf(horizon, 0, -7, -horizonPos);
         glhckObjectScalef(horizon, 128, 1*53/horizonPos, 1.0f);
         glhckObjectPositionf(water, 0, -8, 53-horizonPos);
         glhckMaterialDiffuseb(waterMat, 120*horizonPos/53, 120*horizonPos/53, 120*horizonPos/53, 255);
      }

      if (horizonPos > 46.0f) {
         textPos = floatInterpolate(textPos, 1.0f, data.delta*4.0);
      }

      waterPos += 0.02f * data.delta;
      glhckMaterialTextureOffsetf(waterMat, sin(waterPos), 0);
      glhckObjectRender(water);
      glhckMaterialTextureOffsetf(waterMat, -sin(waterPos), 0);
      glhckObjectRender(water);
      glhckObjectRender(horizon);

      glhckObjectScalef(cube, 1.0+sin(waterPos*8.0)*0.5, 1.5, 1.0+sin(waterPos*8.0)*0.5);
      glhckMaterialDiffuseb(cubeMat, 0, 0, 0, 75*horizonPos/53);
      glhckMaterialBlendFunc(cubeMat, GLHCK_SRC_ALPHA, GLHCK_ONE_MINUS_SRC_ALPHA);
      glhckObjectPositionf(cube, -128*0.06, -9.5, -128*0.3);
      glhckObjectRender(cube);

      glhckObjectScalef(cube, 1.5, 1.5, 1.5);
      glhckMaterialDiffuseb(cubeMat, 250, 250, 250, 255*horizonPos/53);
      glhckMaterialBlendFunc(cubeMat, GLHCK_SRC_ALPHA, GLHCK_ONE_MINUS_SRC_ALPHA);
      glhckObjectPositionf(cube, -128*0.06, -2.0*horizonPos/53-sin(waterPos*8.0), -128*0.3);
      glhckObjectRender(cube);

      glhckTextColorb(text, 0, 0, 0, 255);
      glhckTextStash(text, font, 48, WIDTH-259, 86*textPos, "srv.birth", NULL);
      glhckTextStash(text, font, 12, WIDTH-79*textPos, HEIGHT*0.95-18*2+1, "New Game", NULL);
      glhckTextStash(text, font, 12, WIDTH-79*textPos*0.8, HEIGHT*0.95-18*1+1, "Continue", NULL);
      glhckTextStash(text, font, 12, WIDTH-79*textPos*0.6, HEIGHT*0.95-18*0+1, "Exit", NULL);
      glhckTextRender(text);
      glhckTextClear(text);

      glhckTextColorb(text, 255, 255, 255, 255);
      glhckTextStash(text, font, 48, WIDTH-260, 85*horizonPos/53, "srv.birth", NULL);
      glhckTextStash(text, font, 12, WIDTH-80*textPos, HEIGHT*0.95-18*2, "New Game", NULL);
      glhckTextStash(text, font, 12, WIDTH-80*textPos*0.8, HEIGHT*0.95-18*1, "Continue", NULL);
      glhckTextStash(text, font, 12, WIDTH-80*textPos*0.6, HEIGHT*0.95-18*0, "Exit", NULL);
      glhckTextStash(text, font, 12, 0, HEIGHT, WIN_TITLE, NULL);
      glhckTextRender(text);
      glhckTextClear(text);

      glfwSwapBuffers(window);
      glhckRenderClear(GLHCK_DEPTH_BUFFER | GLHCK_COLOR_BUFFER);

      if (fpsDelay < now) {
         if (duration > 0.0f) {
            FPS = (float)frameCounter / duration;
            snprintf(WIN_TITLE, sizeof(WIN_TITLE)-1, "OpenGL [FPS: %d]", FPS);
            glfwSetWindowTitle(window, WIN_TITLE);
            frameCounter = 0; fpsDelay = now + 1; duration = 0;
         }
      }

      ++frameCounter;
      duration += data.delta;
   }

   const char *host = getenv("SRVBIRTH_SERVER");
   if (!host) host = "localhost";
   if (initEnet(host, 1234, &data) != RETURN_OK)
      return EXIT_FAILURE;

   data.materials.me = glhckMaterialNew(NULL);
   data.materials.player = glhckMaterialNew(NULL);
   data.materials.wall = glhckMaterialNew(NULL);
   glhckMaterialDiffuseb(data.materials.me, 255, 0, 0, 255);
   glhckMaterialDiffuseb(data.materials.player, 0, 255, 0, 255);
   glhckMaterialDiffuseb(data.materials.wall, 0, 255, 0, 255);

   GameCamera *camera = &data.camera;
   camera->object = glhckCameraNew();
   camera->radius = 20;
   camera->speed  = 60;
   camera->rotationSpeed = 180;
   camera->rotation.x = 10.0f;
   kmVec3Fill(&camera->offset, 0.0f, 5.0f, 0.0f);
   glhckCameraRange(camera->object, 1.0f, 500.0f);
   glhckCameraFov(camera->object, 92.0f);

   glhckObject *playerText = glhckTextPlane(text, font, 42, "Player", NULL);
   if (playerText) glhckObjectScalef(playerText, 0.05f, 0.05f, 1.0f);
   GameActor *player = &data.me->actor;
   player->object = glhckCubeNew(1.0f);
   player->speed  = 30;
   glhckObjectScalef(player->object, 1.0f, 3.0f, 1.0f);
   glhckObjectMaterial(player->object, data.materials.me);
   glhckObjectDrawAABB(player->object, 1);
   glhckObjectDrawOBB(player->object, 1);

   glhckAnimator *animator = NULL;
   unsigned int numBones = 0;
   glhckBone **bones = glhckObjectBones(player->object, &numBones);

   if (bones) {
      animator = glhckAnimatorNew();
      glhckAnimatorAnimation(animator, glhckObjectAnimations(player->object, NULL)[0]);
      glhckAnimatorInsertBones(animator, bones, numBones);
      glhckAnimatorUpdate(animator, 0);
      glhckAnimatorTransform(animator, player->object);
   }

   typedef struct DungeonPart {
      char *file;
      float w, h;
   } DungeonPart;

   DungeonPart parts[4];
   parts[0].file = "media/tiles/lattia.obj";
   parts[0].w = 0.0f;
   parts[0].h = 24.0f;
   parts[1].file = "media/tiles/lattia_kulma.obj";
   parts[1].w = 100.0f;
   parts[1].h = 100.0f;
   parts[2].file = "media/tiles/seina.obj";
   parts[2].w = 0.0f;
   parts[2].h = 24.0f;
   parts[3].file = "media/tiles/seina_kulma.obj";
   parts[3].w = 67.3f;
   parts[3].h = 67.3f;

   unsigned int i, c = 15;

   char loopBit = 1;
   float frameDelay = 0;
   unsigned int frame = 0, totalFrames = 29;
   glhckTexture *frames[totalFrames];
   char path[256];
   for (i = 0; i < totalFrames; ++i) {
      snprintf(path, sizeof(path)-1, "media/loli/frame%.3d.png", i+1);
      frames[i] = glhckTextureNewFromFile(path, NULL, glhckTextureDefaultSpriteParameters());
   }

   glhckObject *screen = glhckSpriteNew(frames[0], 0, 0);
   glhckMaterial *screenMaterial = glhckObjectGetMaterial(screen);
   glhckMaterialOptions(screenMaterial, 0);
   glhckObjectScalef(screen, 0.2f, 0.2f, 1.0f);
   glhckObjectRotatef(screen, 0, -90, 0);
   glhckObjectMovef(screen, 45, 35, 0);
   glhckObjectCull(screen, 0);


#if 0
   glhckObject *cubes[c*2];
   unsigned int p = 2;
   float x = -parts[p].w, sx = x;
   float y = -parts[p].h, sy = y;
   int flip = 1;

   for (i = 0; i != 10; ++i) {
      cubes[i] = glhckModelNewEx(parts[p].file, 0.3f, NULL, GLHCK_INDEX_BYTE, GLHCK_VERTEX_V3S);
      glhckObjectPositionf(cubes[i], x, -1.0f, y);
      if (flip) glhckObjectRotationf(cubes[i], 0, 180.0f, 0);

      y += parts[p].h;
      if ((i+1) % 5 == 0) {
         y = sy;
         x += parts[p].w;
         flip = !flip;
      }
   }

   p = 0;
   x = -parts[p].w, sx = x;
   y = -parts[p].h, sy = y;
   flip = 1;
   for (i = 10; i != c; ++i) {
      cubes[i] = glhckModelNewEx(parts[p].file, 0.3f, NULL, GLHCK_INDEX_BYTE, GLHCK_VERTEX_V3S);
      glhckObjectPositionf(cubes[i], x, -1.0f, y);
      if (flip) glhckObjectRotationf(cubes[i], 0, 180.0f, 0);

      y += parts[p].h;
      if ((i+1) % 5 == 0) {
         y = sy;
         x += parts[p].w;
         flip = !flip;
      }
   }

    p = 0;
   x = -parts[p].w, sx = x;
   y = -parts[p].h, sy = y;
   flip = 1;
   for (i = 20; i != c; ++i) {
      cubes[i] = glhckModelNewEx(parts[p].file, 0.3f, NULL, GLHCK_INDEX_BYTE, GLHCK_VERTEX_V3S);
      glhckObjectPositionf(cubes[i], x, -1.0f, y);
      if (flip) glhckObjectRotationf(cubes[i], 0, 180.0f, 0);

      y += parts[p].h;
      if ((i+1) % 5 == 0) {
         y = sy;
         x += parts[p].w;
         flip = !flip;
      }
   }

   glhckObject *gate = glhckModelNewEx("media/chaosgate/chaosgate.obj", 1.8f, NULL, GLHCK_INDEX_SHORT, GLHCK_VERTEX_V3S);
   glhckObjectRotatef(gate, 0, 35.0f, 0);
   glhckObjectPositionf(gate, 3.0f, 1.5f, 0);
#endif

   wall = glhckCubeNew(1.0f);
   glhckObjectScalef(wall, 0.1f, 10.0f, 0.1f);
   glhckObjectMaterial(wall, data.materials.wall);

   glhckImportModelParameters params;
   memcpy(&params, glhckImportDefaultModelParameters(), sizeof(glhckImportModelParameters));
   params.flatten = 1;
   glhckObject *town = glhckModelNewEx("media/towns/town1.obj", 5.5f, &params, GLHCK_INDEX_SHORT, GLHCK_VERTEX_V3S);
   glhckMaterial *townMat = glhckMaterialNew(NULL);
   glhckMaterialDiffuseb(townMat, 50, 50, 50, 255);
   glhckObjectMaterial(town, townMat);
   glhckObjectMovef(town, 0, -5.0f, -8.0f);
   glhckObjectDrawAABB(town, 1);

#if 0
   world = collisionWorldNew();
   unsigned int numChild;
   glhckObject **childs = glhckObjectChildren(town, &numChild);
   for (i = 0; i != numChild; ++i) {
      collisionWorldAddObject(world, childs[i]);
   }
#endif

   int li, numLights = 1;
   glhckLight *light[numLights];
   for (li = 0; li != numLights; ++li) {
      light[li] = glhckLightNew();
      glhckLightAttenf(light[li], 0.0f, 0.0f, 0.01f);
      glhckLightCutoutf(light[li], 45.0f, 0.0f);
      glhckLightPointLightFactor(light[li], 0.4f);
      glhckLightColorb(light[li], 155, 155, 255, 255);
      glhckObjectPositionf(glhckLightGetObject(light[li]), 0.0f, 130.0f, 25.0f);
      glhckObjectTargetf(glhckLightGetObject(light[li]), 0.0f, -80.0f, 25.0f);
   }

   int bot = 0, ac;
   for (ac = 0; ac != argc; ++ac)
      if (!strcmp(argv[ac], "bot")) bot = 1;

   RUNNING = 1;
   int col = 0;
   float anim = 0.0f;
   float fullStateTime = glfwGetTime() + 5.0f;
   float botTime = glfwGetTime();
   unsigned char botFlags = 0;
   srand(time(NULL));
   while (RUNNING && glfwGetKey(window, GLFW_KEY_ESCAPE) != GLFW_PRESS) {
      last       = now;
      now        = glfwGetTime();
      data.delta = now - last;
      glfwPollEvents();

      camera->lastFlags = camera->flags;
      camera->flags = CAMERA_NONE;
      if (camera->lastFlags & CAMERA_SLIDE) camera->flags |= CAMERA_SLIDE;
      player->lastFlags = player->flags;
      player->flags = ACTOR_NONE;

      if (glfwGetKey(window, GLFW_KEY_UP)) {
         camera->flags |= CAMERA_UP;
      }
      if (glfwGetKey(window, GLFW_KEY_DOWN)) {
         camera->flags |= CAMERA_DOWN;
      }
      if (glfwGetKey(window, GLFW_KEY_RIGHT)) {
         camera->flags |= CAMERA_TURN_RIGHT;
      }
      if (glfwGetKey(window, GLFW_KEY_LEFT)) {
         camera->flags |= CAMERA_TURN_LEFT;
      }
      if (glfwGetKey(window, GLFW_KEY_E)) {
         camera->flags |= CAMERA_SLIDE;
      }
      if (glfwGetKey(window, GLFW_KEY_D)) {
         camera->flags |= CAMERA_RIGHT;
      }
      if (glfwGetKey(window, GLFW_KEY_A)) {
         camera->flags |= CAMERA_LEFT;
      }

      if (glfwGetKey(window, GLFW_KEY_W)) {
         player->flags |= ACTOR_FORWARD;
      }
      if (glfwGetKey(window, GLFW_KEY_S)) {
         player->flags |= ACTOR_BACKWARD;
      }
      if (glfwGetKey(window, GLFW_KEY_SPACE)) {
         player->flags |= ACTOR_JUMP;
      }
      if (glfwGetKey(window, GLFW_KEY_Q)) {
         player->flags |= ACTOR_ATTACK;
      }
      if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)) {
         player->flags |= ACTOR_SPRINT;
      }

      if (glfwGetKey(window, GLFW_KEY_B)) {
         bot = !bot;
      }

      /* bot mode */
      if (bot) {
         player->flags |= ACTOR_FORWARD;
         player->flags |= ACTOR_ATTACK;
         camera->flags |= botFlags;
         if (botTime < now) {
            botFlags = 0;
            if (rand() % 2 == 0)
               botFlags |= CAMERA_RIGHT;
            else if (rand() % 2 == 0)
               botFlags |= CAMERA_LEFT;
            botTime = now + 1.0f;
         }
      }

      size_t v, vi;
      int lcol = col; col = 0;
      Client *c2;

      if (animator && (player->flags & ACTOR_FORWARD || player->flags & ACTOR_BACKWARD)) {
         anim += 1.5 * data.delta;
         float min = 2.3;
         float max = 3.2;
         if (anim > max) anim = min;
         if (anim < min) anim = min;
         glhckAnimatorUpdate(animator, anim);
         glhckAnimatorTransform(animator, player->object);
      }

      /* update me */
      gameCameraUpdate(&data, camera, player);
      gameActorUpdateFrom3rdPersonCamera(&data, player, camera);

      for (c2 = data.clients; c2; c2 = c2->next) {
         if (&c2->actor != player) gameActorUpdate(&data, &c2->actor);
      }

      glhckCameraUpdate(camera->object);
      for (li = 0; li != numLights; ++li) {
         glhckLightBeginProjectionWithCamera(light[li], camera->object);
         glhckLightBind(light[li]);
         glhckLightEndProjectionWithCamera(light[li], camera->object);

         /* player text */
         if (playerText) {
            glhckObjectPosition(playerText, &player->position);
            glhckObjectMovef(playerText, 0, 8, 0);
            glhckObjectTarget(playerText, &camera->position);
            //glhckObjectDraw(playerText);
         }

#if 0
         /* draw world */
         for (i = 0; i != c; ++i) {
            glhckObjectDraw(cubes[i]);
         }

         glhckObjectDraw(gate);
#endif

         glhckObjectDraw(town);

         if (frameDelay < now) {
            if (loopBit && ++frame >= totalFrames) loopBit = !loopBit, --frame;
            if (!loopBit && --frame <= 0) loopBit = !loopBit, ++frame;
            glhckMaterialTexture(screenMaterial, frames[frame]);
            frameDelay = now + 0.03;
         }
         glhckObjectDraw(screen);

         /* draw all actors */
         for (c2 = data.clients; c2; c2 = c2->next) {
            if (c2->actor.sword) glhckObjectDraw(c2->actor.sword);
            glhckObjectDraw(c2->actor.object);
         }

         /* render */
         if (li) glhckRenderBlendFunc(GLHCK_ONE, GLHCK_ONE);
         glhckRender();
      }
      glhckRenderBlendFunc(GLHCK_ZERO, GLHCK_ZERO);

      glhckTextStash(text, font, 12, 0,  HEIGHT, WIN_TITLE, NULL);
      glhckTextRender(text);
      glhckTextClear(text);

      glfwSwapBuffers(window);
      glhckRenderClear(GLHCK_DEPTH_BUFFER | GLHCK_COLOR_BUFFER);

      /* manage packets */
      manageEnet(&data);
      if (gameActorFlagsIsMoving(player->flags) && fullStateTime < now) {
         gameSendFullPlayerState(&data);
         fullStateTime = now + 5.0f;
         puts("SEND FULL");
      } else if (player->flags != player->lastFlags) {
         if (!gameActorFlagsIsMoving(player->flags) &&
              gameActorFlagsIsMoving(player->lastFlags) &&
              now-player->lastActTime > 2.0f) {
            gameSendFullPlayerState(&data);
            fullStateTime = now + 5.0f;
            printf("SEND FULL: %.0f\n", now-player->lastActTime);
         } else if (gameActorFlagsIsMoving(player->flags) !=
               gameActorFlagsIsMoving(player->lastFlags)) {
            player->lastActTime = now;
            gameSendPlayerState(&data);
         } else {
            gameSendPlayerState(&data);
         }
      }
      enet_host_flush(data.client);

      if (fpsDelay < now) {
         if (duration > 0.0f) {
            FPS = (float)frameCounter / duration;
            snprintf(WIN_TITLE, sizeof(WIN_TITLE)-1, "OpenGL [FPS: %d]", FPS);
            glfwSetWindowTitle(window, WIN_TITLE);
            frameCounter = 0; fpsDelay = now + 1; duration = 0;
         }
      }

      ++frameCounter;
      duration += data.delta;
   }

   deinitEnet(&data);
   glhckContextTerminate();
   glfwTerminate();
   return EXIT_SUCCESS;
}
