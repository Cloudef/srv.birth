SET(SERVER_SRC
   src/main.c
   ../common/bams.c)
INCLUDE_DIRECTORIES(
  ${kazmath_SOURCE_DIR}/src
  ${srv.birth_SOURCE_DIR}/common
  ${enet_SOURCE_DIR}/src/include
)

ADD_EXECUTABLE(server ${SERVER_SRC})
TARGET_LINK_LIBRARIES(server enet rt)
