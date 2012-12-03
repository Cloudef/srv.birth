#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <enet/enet.h>
#include <GL/glfw3.h>
#include <glhck/glhck.h>

#include "types.h"

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

enum {
   ACTOR_NONE        = 0,
   ACTOR_FORWARD     = 1,
   ACTOR_BACKWARD    = 2,
   ACTOR_LEFT        = 4,
   ACTOR_RIGHT       = 8,
};

typedef struct GameActor {
   glhckObject *object;
   kmVec3 rotation;
   kmVec3 position;
   float speed;
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

kmVec3* kmVec3Interpolate(kmVec3* pOut, const kmVec3* pIn, const kmVec3* other, float d)
{
   const float inv = 1.0f - d;
   pOut->x = other->x*inv + pIn->x*d;
   pOut->y = other->y*inv + pIn->y*d;
   pOut->z = other->z*inv + pIn->z*d;
   return pOut;
}

static int RUNNING = 0;
static int WIDTH = 800, HEIGHT = 480;
int close_callback(GLFWwindow window)
{
   RUNNING = 0;
   return 1;
}

void resize_callback(GLFWwindow window, int width, int height)
{
   WIDTH = width; HEIGHT = height;
   glhckDisplayResize(width, height);
}

typedef struct client_data {
   ENetHost *client;
   ENetPeer *peer;
} client_data;

static void init_data(client_data *data)
{
   assert(data);
   data->client = NULL;
}

static int init_enet(const char *host_ip, const int host_port, client_data *data)
{
   ENetAddress address;
   ENetEvent event;
   assert(host_ip && data);

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

   return RETURN_OK;
}

static void disconnect_enet(client_data *data)
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

static int deinit_enet(client_data *data)
{
   assert(data);
   disconnect_enet(data);
   enet_host_destroy(data->client);
   return RETURN_OK;
}

static int manage_enet(client_data *data)
{
   ENetEvent event;
   assert(data);

   /* Wait up to 1000 milliseconds for an event. */
   while (enet_host_service(data->client, &event, 0) > 0) {
      switch (event.type) {
         case ENET_EVENT_TYPE_RECEIVE:
            printf("A packet of length %u containing %s was received from %s on channel %u.\n",
                  event.packet->dataLength,
                  event.packet->data,
                  event.peer->data,
                  event.channelID);

            /* Clean up the packet now that we're done using it. */
            enet_packet_destroy(event.packet);
            break;

         case ENET_EVENT_TYPE_DISCONNECT:
            printf("%s disconected.\n", event.peer->data);

            /* Reset the peer's client information. */
            event.peer->data = NULL;
      }
   }

   return RETURN_OK;
}

int gameActorFlagsIsMoving(unsigned int flags)
{
   return (flags & ACTOR_FORWARD || flags & ACTOR_BACKWARD);
}

void gameCameraUpdate(GameCamera *camera, GameActor *target)
{
   float speed = camera->speed * 1.0f; /* multiply by interpolation */
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
      camera->rotation.y -= speed;
   }
   if (camera->flags & CAMERA_LEFT) {
      camera->rotation.y += speed;
   }

   if (!gameActorFlagsIsMoving(target->flags)) {
      if (camera->flags & CAMERA_TURN_RIGHT) {
         camera->rotation.y -= speed;
      }
      if (camera->flags & CAMERA_TURN_LEFT) {
         camera->rotation.y += speed;
      }
   } else {
      if (camera->flags & CAMERA_TURN_RIGHT) {
         camera->addRotation.y -= speed;
         camera->flags &= ~CAMERA_SLIDE;
      }

      if (camera->flags & CAMERA_TURN_LEFT) {
         camera->addRotation.y += speed;
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

void gameActorUpdateFrom3rdPersonCamera(GameActor *actor, GameCamera *camera)
{
   float speed = actor->speed * 1.0f; /* multiply by interpolation */

   if (gameActorFlagsIsMoving(actor->flags) != gameActorFlagsIsMoving(actor->lastFlags)) {
      if (!gameActorFlagsIsMoving(actor->flags)) {
         camera->flags |= CAMERA_SLIDE;
      } else {
         camera->rotation.y = camera->rotation.y + camera->addRotation.y;
         camera->addRotation.y = 0.0f;
         camera->flags &= ~CAMERA_SLIDE;
      }
   }

   if (actor->flags & ACTOR_FORWARD) {
      actor->position.x -= speed * cos(kmDegreesToRadians(camera->rotation.y + 90));
      actor->position.z += speed * sin(kmDegreesToRadians(camera->rotation.y + 90));

      kmVec3 targetRotation = {0.0f,camera->rotation.y,0.0f};
      kmVec3Interpolate(&actor->rotation, &actor->rotation, &targetRotation, 0.18f);
   }

   if (actor->flags & ACTOR_BACKWARD) {
      actor->position.x += speed * cos(kmDegreesToRadians(camera->rotation.y + 90));
      actor->position.z -= speed * sin(kmDegreesToRadians(camera->rotation.y + 90));

      kmVec3 targetRotation = {0.0f,camera->rotation.y + 180,0.0f};
      kmVec3Interpolate(&actor->rotation, &actor->rotation, &targetRotation, 0.18f);
   }

   glhckObjectRotation(actor->object, &actor->rotation);
   glhckObjectPosition(actor->object, &actor->position);
}

int main(int argc, char **argv)
{
   /* global data */
   client_data data;
   GLFWwindow window;
   init_data(&data);

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

   // if (init_enet("localhost", 1234, &data) != RETURN_OK)
   //    return EXIT_FAILURE;

   GameCamera camera;
   memset(&camera, 0, sizeof(GameCamera));
   camera.object = glhckCameraNew();
   camera.radius = 30;
   camera.speed  = 3;
   camera.rotation.x = 10.0f;
   kmVec3Fill(&camera.offset, 0.0f, 5.0f, 0.0f);
   glhckCameraRange(camera.object, 1.0f, 500.0f);

   GameActor player;
   memset(&player, 0, sizeof(GameActor));
   player.object = glhckCubeNew(1);
   player.speed  = 1;
   glhckObjectColorb(player.object, 255, 0, 0, 255);

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
   while (RUNNING && glfwGetKey(window, GLFW_KEY_ESCAPE) != GLFW_PRESS) {
      // manage_enet(&data);
      glfwPollEvents();

      camera.lastFlags = camera.flags;
      camera.flags = CAMERA_NONE;
      if (camera.lastFlags & CAMERA_SLIDE) camera.flags |= CAMERA_SLIDE;

      if (glfwGetKey(window, GLFW_KEY_UP)) {
         camera.flags |= CAMERA_UP;
      }
      if (glfwGetKey(window, GLFW_KEY_DOWN)) {
         camera.flags |= CAMERA_DOWN;
      }
      if (glfwGetKey(window, GLFW_KEY_RIGHT)) {
         camera.flags |= CAMERA_TURN_RIGHT;
      }
      if (glfwGetKey(window, GLFW_KEY_LEFT)) {
         camera.flags |= CAMERA_TURN_LEFT;
      }
      if (glfwGetKey(window, GLFW_KEY_E)) {
         camera.flags |= CAMERA_SLIDE;
      }
      if (glfwGetKey(window, GLFW_KEY_D)) {
         camera.flags |= CAMERA_RIGHT;
      }
      if (glfwGetKey(window, GLFW_KEY_A)) {
         camera.flags |= CAMERA_LEFT;
      }

      player.lastFlags = player.flags;
      player.flags = ACTOR_NONE;

      if (glfwGetKey(window, GLFW_KEY_W)) {
         player.flags |= ACTOR_FORWARD;
      }
      if (glfwGetKey(window, GLFW_KEY_S)) {
         player.flags |= ACTOR_BACKWARD;
      }

      gameCameraUpdate(&camera, &player);
      gameActorUpdateFrom3rdPersonCamera(&player, &camera);

      glhckCameraUpdate(camera.object);
      glhckObjectDraw(ground);
      glhckObjectDraw(player.object);

      for (i = 0; i != c; ++i) {
         glhckObjectDraw(cubes[i]);
      }

      glhckRender();
      glfwSwapBuffers(window);
      glhckClear();
   }

   // deinit_enet(&data);
   glhckTerminate();
   glfwTerminate();
   return EXIT_SUCCESS;
}
