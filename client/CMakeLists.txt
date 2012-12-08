SET(CLIENT_SRC
    src/main.c
    ../common/bams.c)
 INCLUDE_DIRECTORIES(
  ${kazmath_SOURCE_DIR}/src
  ${glhck_SOURCE_DIR}/include
  ${GLFW_SOURCE_DIR}/include
  ${srv.birth_SOURCE_DIR}/common
)

ADD_DEFINITIONS(-DGLHCK_KAZMATH_FLOAT -DUSE_SINGLE_PRECISION)
ADD_EXECUTABLE(srv.birth ${CLIENT_SRC})
TARGET_LINK_LIBRARIES(srv.birth glhck glfw enet ${GLFW_LIBRARIES})
