#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <enet/enet.h>

#include "../common/types.h"

typedef struct server_data {
   ENetHost *server;
} server_data;

static void init_data(server_data *data)
{
   assert(data);
   data->server = NULL;
}

static int init_enet(const char *host_ip, const int host_port, server_data *data)
{
   ENetAddress address;
   assert(data);

   if (enet_initialize() != 0) {
      fprintf (stderr, "An error occurred while initializing ENet.\n");
      return RETURN_FAIL;
   }

   address.host = ENET_HOST_ANY;
   address.port = host_port;

   if (host_ip)
      enet_address_set_host(&address, host_ip);

   data->server = enet_host_create(&address,
         32    /* max clients */,
         2     /* max channels */,
         0     /* download bandwidth */,
         0     /* upload bandwidth */);

   if (!data->server) {
      fprintf (stderr,
            "An error occurred while trying to create an ENet server host.\n");
      return RETURN_FAIL;
   }

   return RETURN_OK;
}

static int deinit_enet(server_data *data)
{
   assert(data);
   enet_host_destroy(data->server);
   return RETURN_OK;
}

static int manage_enet(server_data *data)
{
   ENetEvent event;
   assert(data);

   /* Wait up to 1000 milliseconds for an event. */
   while (enet_host_service(data->server, &event, 1000) > 0) {
      switch (event.type) {
         case ENET_EVENT_TYPE_CONNECT:
            printf("A new client connected from %x:%u.\n",
                  event.peer->address.host,
                  event.peer->address.port);

            /* Store any relevant client information here. */
            event.peer->data = "Client information";
            break;

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
   server_data data;
   init_data(&data);

   if (init_enet(NULL, 1234, &data) != RETURN_OK)
      return EXIT_FAILURE;

   while (1) {
      manage_enet(&data);
   }

   deinit_enet(&data);
   return EXIT_SUCCESS;
}
