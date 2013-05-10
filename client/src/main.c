#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <enet/enet.h>
#include <GL/glfw3.h>
#include <glhck/glhck.h>

#include "bams.h"
#include "types.h"

#include <float.h>

#define IFDO(f, x) { if (x) f(x); x = NULL; }

static int RUNNING = 0;
static int WIDTH = 800, HEIGHT = 480;

typedef struct kmEllipse {
   kmVec3 point;
   kmVec3 radius;
} kmEllipse;

typedef struct kmTriangle {
   kmVec3 v1, v2, v3;
} kmTriangle;

kmVec3* kmVec3Divide(kmVec3 *pOut, const kmVec3 *pV1, const kmVec3 *pV2)
{
   pOut->x = pV1->x / pV2->x;
   pOut->y = pV1->y / pV2->y;
   pOut->z = pV1->z / pV2->z;
   return pOut;
}

kmVec3* kmVec3Multiply(kmVec3 *pOut, const kmVec3 *pV1, const kmVec3 *pV2)
{
   pOut->x = pV1->x * pV2->x;
   pOut->y = pV1->y * pV2->y;
   pOut->z = pV1->z * pV2->z;
   return pOut;
}

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

static glhckObject *wall = NULL;
static glhckObject *town = NULL;

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

void gameActorUpdate(ClientData *data, GameActor *actor)
{
   float speed = actor->speed * data->delta;

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
      actor->toPosition.y += speed;
      actor->fallingSpeed = 0.0f;;
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

/***
 * Collision manager
 * When done and working, move this to glhck.
 * Pull request the new methods and datatypes to kazmath.
 *
 * 1. Create collision world
 * 2. Setup world primitives
 * 3. Collide with primitive against world
 *    a. Test against AABB<->AABB first no matter what primitive is used
 *    b. Use the primitive<->primtive route against the found AABB collisions
 * 4. Handle or call response method with the collision data
 ***/

/* public */
typedef struct CollisionInData {
   kmVec3 velocity;
} CollisionInData;

typedef struct CollisionOutData {
   kmTriangle triangle;
   kmVec3 intersectionPoint;
} CollisionOutData;

typedef struct CollisionResponseInData {
   kmVec3 gravity;
} CollisionReponseInData;

typedef struct CollisionResponseOutData {
   kmVec3 position;
   char falling;
} CollisionResponseOutData;

/* internal */
typedef enum _CollisionType {
   COLLISION_ELLIPSE,
   COLLISION_AABB,
   COLLISION_MESH,
} _CollisionType;

typedef struct _CollisionPacket {
   _CollisionType type;
   union {
      const kmEllipse *ellipse;
      const kmAABB *aabb;
   } primitive;

   /* input data we use to calculate out data */
   const CollisionInData *data;

   /* out data that can be handled by user
    * or sent to collision response handler */
   CollisionOutData *out;

   /* internal data */
   kmScalar nearestDistance;
} _CollisionPacket;

typedef struct _CollisionPrimitive {
   _CollisionType type;
   union {
      kmEllipse *ellipse;
      kmAABB *aabb;
      glhckObject *mesh;
      void *any;
   } data;
   struct _CollisionPrimitive *next;
} _CollisionPrimitive;

typedef struct _CollisionWorld {
   _CollisionPrimitive *primitives;
} _CollisionWorld;

/* public */
typedef _CollisionWorld CollisionWorld;
typedef _CollisionPrimitive CollisionPrimitive;

static void kmSwap(kmScalar *a, kmScalar *b) {
   const kmScalar tmp = *a;
   *a = *b;
   *b = tmp;
}

kmScalar kmPlaneDistanceTo(const kmPlane *pIn, const kmVec3 *pV1)
{
   const kmVec3 normal = {pIn->a, pIn->b, pIn->c};
   return kmVec3Dot(pV1, &normal) * pIn->d;
}

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

static kmBool getLowestRoot(kmScalar a, kmScalar b, kmScalar c, kmScalar maxR, kmScalar *root)
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
static kmBool testTrianglePoint(const kmVec3 *base, const kmVec3 *point, const kmVec3 *velocity, kmScalar vsq, kmScalar time, kmScalar *outTime)
{
   kmVec3 tmp;
   kmScalar b, c;
   kmVec3Subtract(&tmp, base, point);
   b = 2.0f * kmVec3Dot(velocity, &tmp);
   kmVec3Subtract(&tmp, point, base);
   c = kmVec3LengthSq(&tmp) - 1.0f;
   return (getLowestRoot(vsq, b, c, time, outTime)?KM_TRUE:KM_FALSE);
}

static kmBool testEdgePoint(const kmVec3 *base, const kmVec3 *point1, const kmVec3 *point2, const kmVec3 *velocity, kmScalar vsq, kmScalar time, kmScalar *outTime, kmVec3 *outPoint)
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
   if (getLowestRoot(a, b, c, time, outTime)) {
      f = (edgeDotVelocity * (*outTime) - edgeDotBaseToVertex) / edgeSquaredLength;
      if (!(f >= 0.0 && f  <= 1.0)) return KM_FALSE;
   }

   if (outPoint) {
      kmVec3Scale(&edge, &edge, f);
      kmVec3Add(outPoint, point1, &edge);
   }
   return KM_TRUE;
}

/* assumes the triangle is given in point's space (ellipse, aabb, etc..) */
static kmBool kmVec3CollidesTriangle(const kmVec3 *pIn, const kmTriangle *triangle, const kmVec3 *velocity, kmVec3 *outIntersectionPoint, kmScalar *outDistanceToCollision)
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
   if (normalDotVelocity == 0.0f) {
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
         return;
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
      if (testTrianglePoint(pIn, &triangle->v1, velocity, velocitySquaredLength, time, &newTime)) {
         foundCollision = KM_TRUE;
         time = newTime;
         memcpy(&intersectionPoint, &triangle->v1, sizeof(kmVec3));
      }

      /* t.v2 */
      if (!foundCollision) {
         if (testTrianglePoint(pIn, &triangle->v2, velocity, velocitySquaredLength, time, &newTime)) {
            foundCollision = KM_TRUE;
            time = newTime;
            memcpy(&intersectionPoint, &triangle->v2, sizeof(kmVec3));
         }
      }

      /* t.v3 */
      if (!foundCollision) {
         if (testTrianglePoint(pIn, &triangle->v3, velocity, velocitySquaredLength, time, &newTime)) {
            foundCollision = KM_TRUE;
            time = newTime;
            memcpy(&intersectionPoint, &triangle->v3, sizeof(kmVec3));
         }
      }

      /* t.v1 --- t.v2 */
      if (testEdgePoint(pIn, &triangle->v1, &triangle->v2, velocity, velocitySquaredLength, time, &newTime, &point)) {
         foundCollision = KM_TRUE;
         time = newTime;
         memcpy(&intersectionPoint, &point, sizeof(kmVec3));
      }

      /* t.v2 --- t.v3 */
      if (testEdgePoint(pIn, &triangle->v2, &triangle->v3, velocity, velocitySquaredLength, time, &newTime, &point)) {
         foundCollision = KM_TRUE;
         time = newTime;
         memcpy(&intersectionPoint, &point, sizeof(kmVec3));
      }

      /* t.v3 --- t.v1 */
      if (testEdgePoint(pIn, &triangle->v3, &triangle->v1, velocity, velocitySquaredLength, time, &newTime, &point)) {
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

static void _collisionMeshCollideWithAABB(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
}

static void _collisionEllipseCollideWithEllipse(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
}

static void _collisionAABBCollideWithEllipse(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
}

static void _collisionMeshCollideWithEllipse(CollisionPrimitive *primitive, _CollisionPacket *packet)
{
   static kmVec3 zero = {0,0,0};
   const CollisionInData *data = packet->data;
   const kmEllipse *ellipse = packet->primitive.ellipse;
   kmVec3 eSpacePosition, eSpaceVelocity;
   kmTriangle triangle;
   glhckGeometry *g;
   int i, ix;

   if (!(g = glhckObjectGetGeometry(primitive->data.mesh)))
      return;

   /* convert to ellipse space */
   kmVec3Divide(&eSpacePosition, &ellipse->point, &ellipse->radius);
   kmVec3Divide(&eSpaceVelocity, &data->velocity, &ellipse->radius);

   for (i = 0; i+2 < g->indexCount; i+=3) {
      ix = glhckGeometryGetVertexIndexForIndex(g, i);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v1, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+1);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v2, NULL, NULL, NULL);
      ix = glhckGeometryGetVertexIndexForIndex(g, i+2);
      glhckGeometryGetVertexDataForIndex(g, ix, (glhckVector3f*)&triangle.v3, NULL, NULL, NULL);
   }
}

static void _collisionWorldCollideWithAABB(CollisionWorld *object, _CollisionPacket *packet)
{
   _CollisionPrimitive *p;
   const CollisionInData *data = packet->data;
   const kmEllipse *ellipse = packet->primitive.ellipse;

   for (p = object->primitives; p; p = p->next) {
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
   kmEllipse *ellipseCopy = NULL;
   _CollisionPrimitive *primitive = NULL;
   assert(object && ellipse);

   if (!(primitive = calloc(1, sizeof(_CollisionPrimitive))))
      goto fail;

   if (!(ellipseCopy = malloc(sizeof(kmEllipse))))
      goto fail;

   memcpy(ellipseCopy, ellipse, sizeof(kmEllipse));
   primitive->type = COLLISION_ELLIPSE;
   primitive->data.ellipse = ellipseCopy;
   _collisionWorldAddPrimitive(object, primitive);
   return primitive;

fail:
   IFDO(free, primitive);
   IFDO(free, ellipseCopy);
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
      default:
         IFDO(free, primitive->data.any);
         break;
   }
   IFDO(free, primitive);
}

void collisionWorldCollideEllipse(CollisionWorld *object, const kmEllipse *ellipse, const CollisionInData *data, CollisionOutData *out)
{
   _CollisionPacket packet;
   packet.type = COLLISION_ELLIPSE;
   packet.primitive.ellipse = ellipse;
   packet.data = data;
   packet.out = out;
   packet.nearestDistance = FLT_MAX;
   _collisionWorldCollideWithEllipse(object, &packet);
}

void collisionWorldCollideAABB(CollisionWorld *object, const kmAABB *aabb, const CollisionInData *data, CollisionOutData *out)
{
   _CollisionPacket packet;
   packet.type = COLLISION_AABB;
   packet.primitive.aabb = aabb;
   packet.data = data;
   packet.out = out;
   packet.nearestDistance = FLT_MAX;
   _collisionWorldCollideWithAABB(object, &packet);
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

   glfwWindowHint(GLFW_SAMPLES, 4);
   glfwWindowHint(GLFW_DEPTH_BITS, 24);
   if (!(window = glfwCreateWindow(WIDTH, HEIGHT, "srv.birth", NULL, NULL)))
      return EXIT_FAILURE;

   glfwMakeContextCurrent(window);

   if (!glhckContextCreate(argc, argv))
      return EXIT_FAILURE;

   if (!glhckDisplayCreate(WIDTH, HEIGHT, GLHCK_RENDER_OPENGL))
      return EXIT_FAILURE;

   if (initEnet("localhost", 1234, &data) != RETURN_OK)
      return EXIT_FAILURE;

   glfwSwapInterval(0);
   glEnable(GL_MULTISAMPLE);

   data.materials.me = glhckMaterialNew(NULL);
   data.materials.player = glhckMaterialNew(NULL);
   data.materials.wall = glhckMaterialNew(NULL);
   glhckMaterialDiffuseb(data.materials.me, 255, 0, 0, 255);
   glhckMaterialDiffuseb(data.materials.player, 0, 255, 0, 255);
   glhckMaterialDiffuseb(data.materials.wall, 0, 255, 0, 255);

   glhckText *text = glhckTextNew(512, 512);
   glhckTextColorb(text, 255, 255, 255, 255);
   unsigned int font = glhckTextNewFont(text, "media/DejaVuSans.ttf");

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
   params.flatten = 0;
   town = glhckModelNewEx("media/towns/town1.obj", 5.5f, &params, GLHCK_INDEX_SHORT, GLHCK_VERTEX_V3S);
   glhckMaterial *townMat = glhckMaterialNew(NULL);
   glhckMaterialDiffuseb(townMat, 50, 50, 50, 255);
   glhckObjectMaterial(town, townMat);
   glhckObjectMovef(town, 0, -5.0f, -8.0f);
   glhckObjectDrawAABB(town, 1);

   glfwSetWindowCloseCallback(window, closeCallback);
   glfwSetWindowSizeCallback(window, resizeCallback);

   int li, numLights = 1;
   glhckLight *light[numLights];
   for (li = 0; li != numLights; ++li) {
      light[li] = glhckLightNew();
      glhckLightAttenf(light[li], 0.0f, 0.0f, 0.0025f);
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

      glhckTextStash(text, font, 18, 0,  HEIGHT-4, WIN_TITLE, NULL);
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
