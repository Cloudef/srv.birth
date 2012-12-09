#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <enet/enet.h>
#include <GL/glfw3.h>
#include <glhck/glhck.h>

#include "bams.h"
#include "types.h"

static int RUNNING = 0;
static int WIDTH = 800, HEIGHT = 480;

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
   glhckObject *object;
   kmVec3 rotation;
   kmVec3 position;
   float speed;
   float toRotation;
   kmVec3 toPosition;
   float interpolation;
   unsigned int flags, lastFlags;
} GameActor;

typedef struct GameCamera {
   glhckCamera *object;
   kmVec3 rotation;
   kmVec3 addRotation;
   kmVec3 position;
   kmVec3 offset;
   float radius;
   float speed;
   unsigned int flags, lastFlags;
} GameCamera;

typedef struct Client {
   GameActor actor;
   char host[45];
   unsigned int clientId;
   unsigned int ping;
   struct Client *next;
} Client;

typedef struct ClientData {
   ENetHost *client;
   ENetPeer *peer;
   Client *me;
   GameCamera camera;
   Client *clients;
   float delta;
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

static int closeCallback(GLFWwindow window)
{
   RUNNING = 0;
   return 1;
}

static void resizeCallback(GLFWwindow window, int width, int height)
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

static void gameSend(ClientData *data, unsigned char *pdata, size_t size)
{
   ENetPacket *packet;
   PacketGeneric *generic = (PacketGeneric*)pdata;
   generic->clientId = htonl(data->me->clientId);
   packet = enet_packet_create(pdata, size, ENET_PACKET_FLAG_RELIABLE);
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
   PacketClientInformation *packet = (PacketClientInformation*)event->packet->data;

   memset(&client, 0, sizeof(Client));
   client.actor.object = glhckCubeNew(1);
   client.actor.speed  = 40;
   client.actor.interpolation = 1.0f;
   strncpy(client.host, packet->host, sizeof(client.host));
   client.clientId = packet->clientId;
   glhckObjectColorb(client.actor.object, 0, 255, 0, 255);
   gameNewClient(data, &client);
   printf("Client [%u] (%s) joined!\n", client.clientId, client.host);
}

static void handlePart(ClientData *data, ENetEvent *event)
{
   Client *client;
   PacketClientPart *packet = (PacketClientPart*)event->packet->data;

   if (!(client = clientForId(data, packet->clientId)))
      return;

   printf("Client [%u] (%s) parted!\n", client->clientId, client->host);
   glhckObjectFree(client->actor.object);
   gameFreeClient(data, client);
   event->peer->data = NULL;
}

static void handlePing(ClientData *data, ENetEvent *event)
{
   PacketClientPing ping;
   memcpy(&ping, event->packet->data, sizeof(PacketClientPing));
   gameSend(data, (unsigned char*)&ping, sizeof(PacketClientPing));
   puts("PING!");
}

static void gameActorApplyPacket(ClientData *data, GameActor *actor, PacketActorState *packet)
{
   actor->flags = ntohl(packet->flags);
}

static void handleState(ClientData *data, ENetEvent *event)
{
   Client *client;
   PacketActorState *packet = (PacketActorState*)event->packet->data;

   if (!(client = clientForId(data, packet->clientId)))
      return;

   gameActorApplyPacket(data, &client->actor, packet);
}

static void handleFullState(ClientData *data, ENetEvent *event)
{
   Client *client;
   Vector3f pos;
   kmVec3 kpos;
   PacketActorFullState *packet = (PacketActorFullState*)event->packet->data;

   if (!(client = clientForId(data, packet->clientId)))
      return;

   gameActorApplyPacket(data, &client->actor, (PacketActorState*)packet); /* handle the delta part */
   client->ping = ntohl(packet->ping);
   client->actor.interpolation = (client->ping?(float)1.0f/client->ping:1.0f);
   BamsToV3F(&pos, &packet->position);
   kpos.x = pos.x; kpos.y = pos.y; kpos.z = pos.z;

   if (gameActorFlagsIsMoving(client->actor.flags)) {
      client->actor.toRotation = floatInterpolate(client->actor.toRotation, BamsToF(packet->rotation), client->actor.interpolation);
      kmVec3Interpolate(&client->actor.toPosition, &client->actor.toPosition, &kpos, client->actor.interpolation);
   } else {
      client->actor.toRotation = BamsToF(packet->rotation);
      kmVec3Assign(&client->actor.toPosition, &kpos);
   }

   printf("GOT FULL STATE [%f]\n", client->actor.interpolation);
}

static int manageEnet(ClientData *data)
{
   ENetEvent event;
   PacketGeneric *packet;
   assert(data);

   /* Wait up to 1000 milliseconds for an event. */
   while (enet_host_service(data->client, &event, 0) > 0) {
      switch (event.type) {
         case ENET_EVENT_TYPE_RECEIVE:
            printf("A packet of length %u containing %s was received on channel %u.\n",
                  event.packet->dataLength,
                  event.packet->data,
                  event.channelID);

            /* handle packet */
            packet = (PacketGeneric*)event.packet->data;
            packet->clientId = ntohl(packet->clientId);
            switch (packet->id) {
               case PACKET_ID_CLIENT_INFORMATION:
                  handleJoin(data, &event);
                  break;
               case PACKET_ID_CLIENT_PART:
                  handlePart(data, &event);
                  break;
               case PACKET_ID_CLIENT_PING:
                  handlePing(data, &event);
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

int gameActorFlagsIsMoving(unsigned int flags)
{
   return (flags & ACTOR_FORWARD || flags & ACTOR_BACKWARD);
}

void gameCameraUpdate(ClientData *data, GameCamera *camera, GameActor *target)
{
   float speed = camera->speed * data->delta; /* multiply by interpolation */
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
      camera->rotation.y -= speed*2;
      target->flags |= ACTOR_RIGHT;
   }
   if (camera->flags & CAMERA_LEFT) {
      camera->rotation.y += speed*2;
      target->flags |= ACTOR_LEFT;
   }

   if (!gameActorFlagsIsMoving(target->flags)) {
      if (camera->flags & CAMERA_TURN_RIGHT) {
         camera->rotation.y -= speed*2;
         target->flags |= ACTOR_RIGHT;
      }
      if (camera->flags & CAMERA_TURN_LEFT) {
         camera->rotation.y += speed*2;
         target->flags |= ACTOR_LEFT;
      }
   } else {
      if (camera->flags & CAMERA_TURN_RIGHT) {
         camera->addRotation.y -= speed*2;
         camera->flags &= ~CAMERA_SLIDE;
      }

      if (camera->flags & CAMERA_TURN_LEFT) {
         camera->addRotation.y += speed*2;
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

   glhckObject *internalObject = glhckCameraGetObject(camera->object);
   kmVec3 targetPosition = target->position;
   kmVec3Add(&targetPosition, &targetPosition, &camera->offset);
   glhckObjectTarget(internalObject, &targetPosition);
   glhckObjectPosition(internalObject, &camera->position);
}

void gameActorUpdate(ClientData *data, GameActor *actor)
{
   float speed = actor->speed * data->delta; /* multiply by interpolation */

   if (actor != &data->me->actor) {
      float cspeed = data->camera.speed * data->delta; /* camera speed for other players */
      if (actor->flags & ACTOR_LEFT) {
         actor->toRotation += cspeed*2;
      }
      if (actor->flags & ACTOR_RIGHT) {
         actor->toRotation -= cspeed*2;
      }
   }

   if (actor->flags & ACTOR_FORWARD) {
      actor->toPosition.x -= speed * cos(kmDegreesToRadians(actor->toRotation + 90));
      actor->toPosition.z += speed * sin(kmDegreesToRadians(actor->toRotation + 90));

      kmVec3 targetRotation = {0.0f,actor->toRotation,0.0f};
      kmVec3Interpolate(&actor->rotation, &actor->rotation, &targetRotation, actor->interpolation);
   }

   if (actor->flags & ACTOR_BACKWARD) {
      actor->toPosition.x += speed * cos(kmDegreesToRadians(actor->toRotation + 90));
      actor->toPosition.z -= speed * sin(kmDegreesToRadians(actor->toRotation + 90));

      kmVec3 targetRotation = {0.0f,actor->toRotation + 180,0.0f};
      kmVec3Interpolate(&actor->rotation, &actor->rotation, &targetRotation, actor->interpolation);
   }

#if 0
   if (gameActorFlagsIsMoving(actor->flags)) {
      kmVec3Interpolate(&actor->position, &actor->position, &actor->toPosition, actor->interpolation);
   } else {
      kmVec3Interpolate(&actor->position, &actor->position, &actor->toPosition, 0.02f);
   }
#else
   kmVec3Interpolate(&actor->position, &actor->position, &actor->toPosition, actor->interpolation);
#endif

   glhckObjectRotation(actor->object, &actor->rotation);
   glhckObjectPosition(actor->object, &actor->position);
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

   actor->toRotation = camera->rotation.y;
   gameActorUpdate(data, actor);
}

void gameSendPlayerAndCameraState(ClientData *data, GameCamera *camera)
{
   PacketActorState state;
   memset(&state, 0, sizeof(PacketActorState));
   state.id = PACKET_ID_ACTOR_STATE;
   state.flags = htonl(data->me->actor.flags);
   gameSend(data, (unsigned char*)&state, sizeof(PacketActorState));
}

void gameSendFullPlayerAndCameraState(ClientData *data, GameCamera *camera)
{
   Vector3f pos;
   PacketActorFullState state;
   memset(&state, 0, sizeof(PacketActorFullState));
   state.id = PACKET_ID_ACTOR_FULL_STATE;
   state.flags = htonl(data->me->actor.flags);
   state.rotation = FToBams(camera->rotation.y);

   pos.x = data->me->actor.position.x;
   pos.y = data->me->actor.position.y;
   pos.z = data->me->actor.position.z;
   V3FToBams(&state.position, &pos);
   gameSend(data, (unsigned char*)&state, sizeof(PacketActorFullState));
}

int main(int argc, char **argv)
{
   /* global data */
   ClientData data;
   GLFWwindow window;
   float          now          = 0;
   float          last         = 0;
   unsigned int   frameCounter = 0;
   unsigned int   FPS          = 0;
   unsigned int   fpsDelay     = 0;
   float          duration     = 0;
   initClientData(&data);

   if (!glfwInit())
      return EXIT_FAILURE;

   if (!(window = glfwCreateWindow(WIDTH, HEIGHT, GLFW_WINDOWED, "srv.birth", NULL)))
      return EXIT_FAILURE;

   glfwSwapInterval(1);
   glfwMakeContextCurrent(window);

   if (!glhckInit(argc, argv))
      return EXIT_FAILURE;

   if (!glhckDisplayCreate(WIDTH, HEIGHT, 0))
      return EXIT_FAILURE;

   if (initEnet("localhost", 1234, &data) != RETURN_OK)
      return EXIT_FAILURE;

   GameCamera *camera = &data.camera;
   camera->object = glhckCameraNew();
   camera->radius = 30;
   camera->speed  = 60;
   camera->rotation.x = 10.0f;
   kmVec3Fill(&camera->offset, 0.0f, 5.0f, 0.0f);
   glhckCameraRange(camera->object, 1.0f, 500.0f);

   GameActor *player = &data.me->actor;
   player->object = glhckCubeNew(1);
   player->speed  = 40;
   player->interpolation = 1.0f;
   glhckObjectColorb(player->object, 255, 0, 0, 255);

   glhckObject *ground = glhckPlaneNew(100);
   glhckObjectRotatef(ground, 90.0f, 0.0f, 0.0f);
   glhckObjectPositionf(ground, 0.0f, -1.0f, 0.0f);

   unsigned int i, r, c = 1521;
   glhckObject *cubes[c];
   for (i = 0, r = 0; i != c; ++i) {
      cubes[i] = glhckCubeNew(1);
      glhckObjectColorb(cubes[i], 0, 0, 255, 255);
      glhckObjectPositionf(cubes[i], 5.0f * (i % 39) - 95.0f, -1.0f, 5.0f * r - 95.0f);
      r += ((i+1) % 39 == 0);
   }

   RUNNING = 1;
   float fullStateTime = glfwGetTime() + 5.0f;
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

      gameCameraUpdate(&data, camera, player);
      gameActorUpdateFrom3rdPersonCamera(&data, player, camera);

      glhckCameraUpdate(camera->object);
      glhckObjectDraw(ground);
      glhckObjectDraw(player->object);

      Client *c2;
      for (c2 = data.clients; c2; c2 = c2->next) {
         if (&c2->actor != player) gameActorUpdate(&data, &c2->actor);
         glhckObjectDraw(c2->actor.object);
      }

      for (i = 0; i != c; ++i) {
         glhckObjectDraw(cubes[i]);
      }

      glhckRender();
      glfwSwapBuffers(window);
      glhckClear();

      manageEnet(&data);
      if (gameActorFlagsIsMoving(player->flags) && fullStateTime < now) {
         gameSendFullPlayerAndCameraState(&data, camera);
         fullStateTime = now + 5.0f;
         puts("SEND FULL");
      } else if (player->flags != player->lastFlags) {
         if (gameActorFlagsIsMoving(player->flags) != gameActorFlagsIsMoving(player->lastFlags)) {
            gameSendFullPlayerAndCameraState(&data, camera);
            fullStateTime = now + 5.0f;
            puts("SEND FULL");
         } else {
            gameSendPlayerAndCameraState(&data, camera);
         }
      }
      enet_host_flush(data.client);

      if (fpsDelay < now) {
         if (duration > 0.0f) {
            FPS = (float)frameCounter / duration;
            frameCounter = 0; fpsDelay = now + 1; duration = 0;
         }
      }

      ++frameCounter;
      duration += data.delta;
   }

   deinitEnet(&data);
   glhckTerminate();
   glfwTerminate();
   return EXIT_SUCCESS;
}
