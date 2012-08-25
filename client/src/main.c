#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <enet/enet.h>
#include <GL/glfw3.h>
#include <GL/glhck.h>

#include "types.h"

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

int main(int argc, char **argv)
{
   /* global data */
   client_data data;
   GLFWwindow window;
   init_data(&data);

   if (!glfwInit())
      return EXIT_FAILURE;

   if (!(window = glfwOpenWindow(WIDTH, HEIGHT, GLFW_WINDOWED, "srv.birth", NULL)))
      return EXIT_FAILURE;

   if (!glhckInit(argc, argv))
      return EXIT_FAILURE;

   if (!glhckDisplayCreate(WIDTH, HEIGHT, 0))
      return EXIT_FAILURE;

   if (init_enet("localhost", 1234, &data) != RETURN_OK)
      return EXIT_FAILURE;

   RUNNING = 1;
   while (RUNNING && glfwGetKey(window, GLFW_KEY_ESCAPE) != GLFW_PRESS) {
      manage_enet(&data);
      glfwSwapBuffers();
      glhckClear();
   }

   deinit_enet(&data);
   glhckTerminate();
   glfwTerminate();
   return EXIT_SUCCESS;
}
