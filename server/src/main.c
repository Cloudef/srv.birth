#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <enet/enet.h>

#include "../common/bams.h"
#include "../common/types.h"

typedef struct GameActor {
   unsigned char flags;
   unsigned char rotation;
   Vector3f position;
} GameActor;

typedef struct Client {
   char host[46];
   unsigned int clientId;
   ENetPeer *peer;
   GameActor actor;
   struct Client *next;
} Client;

typedef struct ServerData {
   ENetHost *server;
   Client *clients;
} ServerData;

static Client* serverNewClient(ServerData *data, Client *params)
{
   Client *c;

   /* add to list */
   for (c = data->clients; c && c->next; c = c->next);
   if (c) c = c->next = malloc(sizeof(Client));
   else c = data->clients = malloc(sizeof(Client));

   memcpy(c, params, sizeof(Client));
   return c;
}

static void serverFreeClient(ServerData *data, Client *client)
{
   Client *c;

   /* remove from list */
   for (c = data->clients; c != client && c->next != client; c = c->next);
   if (c == client) data->clients = client->next;
   else if (c) c->next = client->next;

   free(client);
}

static void initServerData(ServerData *data)
{
   assert(data);
   memset(data, 0, sizeof(ServerData));
}

static void serverSend(Client *client, unsigned char *pdata, size_t size, ENetPacketFlag flag)
{
   ENetPacket *packet;
   packet = enet_packet_create(pdata, size, flag);
   enet_peer_send(client->peer, 0, packet);
}

static int initEnet(const char *host_ip, const int host_port, ServerData *data)
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

   /* enable compression */
   data->server->checksum = enet_crc32;
   enet_host_compress_with_range_coder(data->server);

   return RETURN_OK;
}

static int deinitEnet(ServerData *data)
{
   assert(data);
   enet_host_destroy(data->server);
   return RETURN_OK;
}

static void sendFullState(ServerData *data, Client *target, Client *client)
{
   PacketServerActorFullState state;
   memset(&state, 0, sizeof(PacketActorFullState));
   state.id = PACKET_ID_ACTOR_FULL_STATE;
   state.clientId = htonl(target->clientId);
   state.flags = target->actor.flags;
   state.rotation = target->actor.rotation;
   memcpy(&state.position, &target->actor.position, sizeof(Vector3f));
   serverSend(client, (unsigned char*)&state, sizeof(PacketActorFullState), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
}

static void sendJoin(ServerData *data, ENetEvent *event)
{
   Client client, *c;
   memset(&client, 0, sizeof(Client));
   client.peer = event->peer;
   client.clientId = event->peer->connectID;
   enet_address_get_host_ip(&event->peer->address, client.host, sizeof(client.host));
   event->peer->data = serverNewClient(data, &client);

   PacketServerClientInformation info;
   memset(&info, 0, sizeof(PacketServerClientInformation));
   info.id = PACKET_ID_CLIENT_INFORMATION;
   strncpy(info.host, client.host, sizeof(info.host));
   info.clientId = htonl(client.clientId);
   for (c = data->clients; c; c = c->next) {
      if (c == event->peer->data) continue;
      PacketServerClientInformation info2;
      memset(&info2, 0, sizeof(PacketServerClientInformation));
      info2.id = PACKET_ID_CLIENT_INFORMATION;
      strncpy(info2.host, c->host, sizeof(info2.host));
      info2.clientId = htonl(c->clientId);
      serverSend(event->peer->data, (unsigned char*)&info2, sizeof(PacketServerClientInformation), ENET_PACKET_FLAG_RELIABLE);
      sendFullState(data, c, event->peer->data);
      serverSend(c, (unsigned char*)&info, sizeof(PacketServerClientInformation), ENET_PACKET_FLAG_RELIABLE);
   }

   c = (Client*)event->peer->data;
   printf("%s [%u] connected.\n", c->host, c->clientId);
}

static void sendPart(ServerData *data, ENetEvent *event)
{
   Client *c;
   PacketServerClientPart part;

   c = (Client*)event->peer->data;
   memset(&part, 0, sizeof(PacketServerClientPart));
   part.id = PACKET_ID_CLIENT_PART;
   part.clientId = htonl(c->clientId);
   for (c = data->clients; c; c = c->next) {
      if (c == event->peer->data) continue;
      serverSend(c, (unsigned char*)&part, sizeof(PacketServerClientPart), ENET_PACKET_FLAG_RELIABLE);
   }

   c = (Client*)event->peer->data;
   printf("%s [%u] disconnected.\n", c->host, c->clientId);
}

static void handleState(ServerData *data, ENetEvent *event)
{
   PacketServerActorState state;
   PacketActorState *p = (PacketActorState*)event->packet->data;
   Client *c, *client = (Client*)event->peer->data;

   state.id = p->id;
   state.clientId = htonl(client->clientId);
   state.flags = p->flags;
   state.rotation = p->rotation;
   for (c = data->clients; c; c = c->next) {
      if (c == client) continue;
      serverSend(c, (unsigned char*)&state, sizeof(PacketServerActorState), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
   }

   client->actor.flags = p->flags;
   client->actor.rotation = p->rotation;
}

static void handleFullState(ServerData *data, ENetEvent *event)
{
   PacketServerActorFullState state;
   PacketActorFullState *p = (PacketActorFullState*)event->packet->data;
   Client *c, *client = (Client*)event->peer->data;

   state.id = p->id;
   state.clientId = htonl(client->clientId);
   state.flags = p->flags;
   state.rotation = p->rotation;
   memcpy(&state.position, &p->position, sizeof(Vector3f));
   for (c = data->clients; c; c = c->next) {
      if (c == client) continue;
      serverSend(c, (unsigned char*)&state, sizeof(PacketServerActorFullState), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
   }

   client->actor.flags = p->flags;
   client->actor.rotation = p->rotation;
   memcpy(&client->actor.position, &p->position, sizeof(Vector3f));
}

static int manageEnet(ServerData *data)
{
   ENetEvent event;
   PacketGeneric *packet;
   Client *client;
   assert(data);

   /* Wait up to 1000 milliseconds for an event. */
   while (enet_host_service(data->server, &event, 1000) > 0) {
      switch (event.type) {
         case ENET_EVENT_TYPE_CONNECT:
            printf("A new client connected from %x:%u.\n",
                  event.peer->address.host,
                  event.peer->address.port);

            /* broadcast join message to others */
            sendJoin(data, &event);
            client = (Client*)event.peer->data;
            break;

         case ENET_EVENT_TYPE_RECEIVE:
            /* discard bad packets */
            if (event.packet->dataLength < sizeof(PacketGeneric))
               break;

            printf("A packet of length %u was received on channel %u.\n",
                  event.packet->dataLength,
                  event.channelID);

            /* handle packet */
            client = (Client*)event.peer->data;
            packet = (PacketGeneric*)event.packet->data;
            switch (packet->id) {
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

         case ENET_EVENT_TYPE_DISCONNECT:
            /* broadcast part message to others */
            sendPart(data, &event);

            /* Reset the peer's client information. */
            serverFreeClient(data, event.peer->data);
            event.peer->data = NULL;
      }
   }

   /* send all response packets */
   enet_host_flush(data->server);
   return RETURN_OK;
}

int main(int argc, char **argv)
{
   /* global data */
   ServerData data;
   initServerData(&data);

   if (initEnet(NULL, 1234, &data) != RETURN_OK)
      return EXIT_FAILURE;

   while (1) {
      manageEnet(&data);
   }

   deinitEnet(&data);
   return EXIT_SUCCESS;
}
