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
};

typedef enum PacketId {
   PACKET_ID_CLIENT_INFORMATION  = 0,
   PACKET_ID_CLIENT_PART         = 1,
   PACKET_ID_ACTOR_STATE         = 2,
   PACKET_ID_ACTOR_FULL_STATE    = 4
} PacketId;

typedef struct {
   char id;
   unsigned int clientId;
} PacketGeneric;

typedef struct {
   char id;
   unsigned int clientId;
   char host[45];
} PacketClientInformation;

typedef struct {
   char id;
   unsigned int clientId;
} PacketClientPart;

typedef struct {
   char id;
   unsigned int clientId;
   unsigned int flags;
} PacketActorState;

typedef struct {
   char id;
   unsigned int clientId;
   unsigned int flags;
   unsigned int rotation;
   Vector3B position;
} PacketActorFullState;

#endif /* SRVBIRTH_TYPES_H */

/* vim: set ts=8 sw=3 tw=0 :*/
