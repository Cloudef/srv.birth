#ifndef SRVBIRTH_TYPES_H
#define SRVBIRTH_TYPES_H

#include "bams.h"

enum {
   RETURN_FAIL    = -1,
   RETURN_OK      =  0,
   RETURN_TRUE    =  1,
   RETURN_FALSE   =  !RETURN_TRUE
};

enum {
   ACTOR_NONE        = 0,
   ACTOR_FORWARD     = 1,
   ACTOR_BACKWARD    = 2,
   ACTOR_LEFT        = 4,
   ACTOR_RIGHT       = 8,
   ACTOR_JUMP        = 16,
};

typedef enum PacketId {
   PACKET_ID_CLIENT_INFORMATION  = 0,
   PACKET_ID_CLIENT_PART         = 1,
   PACKET_ID_ACTOR_STATE         = 2,
   PACKET_ID_ACTOR_FULL_STATE    = 4
} PacketId;

/* Server will send PacketServer<packet name> packets,
 * and recieves Packet<packet name> packets.
 *
 * Client sends Packet<packet name> packets,
 * and receives PacketServer<packet name> packets.
 *
 * This is because client does not need to send as much information
 * to server as server has to send to clients. */

#pragma pack(push,1)

#define PACKET_SERVER_HEADER \
   unsigned int clientId;    \
   unsigned char id;

#define PACKET_CLIENT_HEADER \
   unsigned char id;

#define DEFINE_PACKET(name, types) \
   typedef struct {                \
      PACKET_CLIENT_HEADER         \
      types                        \
   } Packet##name;                 \
   typedef struct {                \
      PACKET_SERVER_HEADER         \
      types                        \
   } PacketServer##name;           \

/* generic packets */
DEFINE_PACKET(Generic, ;);

/* server only packets */
typedef struct {
   PACKET_SERVER_HEADER
   char host[45];
} PacketServerClientInformation;
typedef PacketServerGeneric PacketServerClientPart;

/* client<->server packets */
DEFINE_PACKET(ActorState,
      unsigned char flags;
      unsigned char rotation;);

DEFINE_PACKET(ActorFullState,
      unsigned char flags;
      unsigned char rotation;
      Vector3f position;);

#pragma pack(pop)

#endif /* SRVBIRTH_TYPES_H */

/* vim: set ts=8 sw=3 tw=0 :*/
