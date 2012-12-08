#ifndef SRVBIRTH_TYPES_H
#define SRVBIRTH_TYPES_H

enum {
   RETURN_FAIL    = -1,
   RETURN_OK      =  0,
   RETURN_TRUE    =  1,
   RETURN_FALSE   =  !RETURN_TRUE
};

typedef enum PacketId {
   PACKET_ID_CLIENT_INFORMATION  = 0,
   PACKET_ID_PART                = 1,
} PacketId;

typedef struct {
   char id;
} PacketGeneric;

typedef struct {
   char id;
   char host[45];
   unsigned int clientId;
} PacketClientInformation;

typedef struct {
   char id;
   unsigned int clientId;
} PacketClientPart;

#endif /* SRVBIRTH_TYPES_H */
