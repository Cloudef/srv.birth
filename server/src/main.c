#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <enet/enet.h>

#include "../common/bams.h"
#include "../common/types.h"

typedef struct GameActor {
   unsigned int flags;
   unsigned int rotation;
   Vector3B position;
} GameActor;

typedef struct Client {
   char host[46];
   unsigned int clientId;
   unsigned int ping, pingTime;
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

static void serverSend(Client *client, unsigned char *pdata, size_t size)
{
   ENetPacket *packet;
   packet = enet_packet_create(pdata, size, ENET_PACKET_FLAG_RELIABLE);
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

   return RETURN_OK;
}

static int deinitEnet(ServerData *data)
{
   assert(data);
   enet_host_destroy(data->server);
   return RETURN_OK;
}

#define REDIRECT_PACKET_TO_OTHERS(cast, data, event)        \
   Client *c;                                               \
   cast copy;                                               \
   cast *packet = (cast*)event->packet->data;               \
   memcpy(&copy, packet, sizeof(cast));                     \
   for (c = data->clients; c; c = c->next) {                \
      if (c == event->peer->data) continue;                 \
      serverSend(c, (unsigned char*)&copy, sizeof(cast));   \
   }

static void sendFullState(ServerData *data, Client *target, Client *client)
{
   PacketActorFullState state;
   memset(&state, 0, sizeof(PacketActorFullState));
   state.id = PACKET_ID_ACTOR_FULL_STATE;
   state.clientId = htonl(target->clientId);
   state.ping = htonl(target->ping);
   state.flags = target->actor.flags;
   state.rotation = target->actor.rotation;
   memcpy(&state.position, &target->actor.position, sizeof(Vector3B));
   serverSend(client, (unsigned char*)&state, sizeof(PacketActorFullState));
}

static void sendJoin(ServerData *data, ENetEvent *event)
{
   Client client, *c;
   memset(&client, 0, sizeof(Client));
   client.peer = event->peer;
   client.clientId = event->peer->connectID;
   enet_address_get_host_ip(&event->peer->address, client.host, sizeof(client.host));
   event->peer->data = serverNewClient(data, &client);

   PacketClientInformation info;
   memset(&info, 0, sizeof(PacketClientInformation));
   info.id = PACKET_ID_CLIENT_INFORMATION;
   strncpy(info.host, client.host, sizeof(info.host));
   info.clientId = htonl(client.clientId);
   for (c = data->clients; c; c = c->next) {
      if (c == event->peer->data) continue;
      PacketClientInformation info2;
      memset(&info2, 0, sizeof(PacketClientInformation));
      info2.id = PACKET_ID_CLIENT_INFORMATION;
      strncpy(info2.host, c->host, sizeof(info2.host));
      info2.clientId = htonl(c->clientId);
      serverSend(event->peer->data, (unsigned char*)&info2, sizeof(PacketClientInformation));
      sendFullState(data, c, event->peer->data);
      serverSend(c, (unsigned char*)&info, sizeof(PacketClientInformation));
   }

   c = (Client*)event->peer->data;
   printf("%s [%u] connected.\n", c->host, c->clientId);
}

static void sendPart(ServerData *data, ENetEvent *event)
{
   Client *c;
   PacketClientPart part;

   c = (Client*)event->peer->data;
   memset(&part, 0, sizeof(PacketClientPart));
   part.id = PACKET_ID_CLIENT_PART;
   part.clientId = htonl(c->clientId);
   for (c = data->clients; c; c = c->next) {
      if (c == event->peer->data) continue;
      serverSend(c, (unsigned char*)&part, sizeof(PacketClientPart));
   }

   c = (Client*)event->peer->data;
   printf("%s [%u] disconnected.\n", c->host, c->clientId);
}

static unsigned int timeclock()
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return now.tv_sec*1000000 + now.tv_nsec/1000;
}

static void sendPing(ServerData *data, ENetEvent *event)
{
   Client *c;
   PacketClientPing ping;
   c = (Client*)event->peer->data;
   memset(&ping, 0, sizeof(PacketClientPing));
   ping.id = PACKET_ID_CLIENT_PING;
   ping.clientId = htonl(c->clientId);
   ping.time = htonl(timeclock());
   serverSend(c, (unsigned char*)&ping, sizeof(PacketClientPing));
}

static void handlePing(ServerData *data, ENetEvent *event)
{
   Client *c;
   PacketClientPing *ping = (PacketClientPing*)event->packet->data;
   c = (Client*)event->peer->data;
   c->ping = (timeclock() - ntohl(ping->time))/1000.0f;
}

static void handleState(ServerData *data, ENetEvent *event)
{
   REDIRECT_PACKET_TO_OTHERS(PacketActorState, data, event);
   c = (Client*)event->peer->data;
   c->actor.flags = packet->flags;
}

static void handleFullState(ServerData *data, ENetEvent *event)
{
   PacketActorFullState *p = (PacketActorFullState*)event->packet->data;
   Client *client = (Client*)event->peer->data;
   p->ping = htonl(client->ping);
   REDIRECT_PACKET_TO_OTHERS(PacketActorFullState, data, event);
   client->actor.flags = packet->flags;
   client->actor.rotation = packet->rotation;
   memcpy(&client->actor.position, &packet->position, sizeof(Vector3B));
}

#undef REDIRECT_PACKET_TO_OTHERS

static int manageEnet(ServerData *data)
{
   ENetEvent event;
   PacketGeneric *packet;
   Client *client;
   unsigned int now;

   assert(data);
   now = timeclock();

   /* Wait up to 1000 milliseconds for an event. */
   while (enet_host_service(data->server, &event, 1000) > 0) {
      switch (event.type) {
         case ENET_EVENT_TYPE_CONNECT:
            printf("A new client connected from %x:%u.\n",
                  event.peer->address.host,
                  event.peer->address.port);

            /* broadcast join message to others */
            sendJoin(data, &event);
            sendPing(data, &event);
            client = (Client*)event.peer->data;
            client->pingTime = now + 10000;
            break;

         case ENET_EVENT_TYPE_RECEIVE:
            printf("A packet of length %u containing %s was received on channel %u.\n",
                  event.packet->dataLength,
                  event.packet->data,
                  event.channelID);

            /* handle packet */
            client = (Client*)event.peer->data;
            packet = (PacketGeneric*)event.packet->data;
            /* we have no reason to ntohl the clientId here.
             * server can use the peer->data to find out the client pointer.
             * this is so we can redirect the packets like they are for echo packets. */
            switch (packet->id) {
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

            if (client->pingTime < now) {
               sendPing(data, &event);
               client->pingTime = now + 10000;
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
