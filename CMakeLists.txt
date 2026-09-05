cmake_minimum_required(VERSION 3.16)
project(sniper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenCV REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
find_library(MOSQUITTO_LIBRARY mosquitto)

add_executable(sniper
	src/main.cpp
	src/daheng_camera/galaxy.cpp
	src/encoder/encoder.cpp
	src/sender/serial.cpp
	src/sender/mqtt.cpp
	src/sender/rmcc.pb.cpp
	src/crc/crc_check.cpp
)

target_include_directories(sniper PRIVATE
	${PROJECT_SOURCE_DIR}/include
	${PROJECT_SOURCE_DIR}/include/protobuf
	${OpenCV_INCLUDE_DIRS}
	${GSTREAMER_INCLUDE_DIRS}
	${MOSQUITTO_INCLUDE_DIR}
	${Protobuf_INCLUDE_DIRS}
)

target_compile_options(sniper PRIVATE ${GSTREAMER_CFLAGS_OTHER})

target_link_libraries(sniper PRIVATE
	${OpenCV_LIBS}
	yaml-cpp
	gxiapi
	${GSTREAMER_LIBRARIES}
	${MOSQUITTO_LIBRARY}
	protobuf::libprotobuf
)
