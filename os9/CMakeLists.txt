# The Mac OS 9 receiver, built with Retro68's PowerPC toolchain:
#   cmake -S os9 -B os9/build -DCMAKE_TOOLCHAIN_FILE=<Retro68-build>/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build os9/build
# Makes "PowerPC Display.bin" (MacBinary) and .dsk (a disk image with the app).
cmake_minimum_required(VERSION 3.10)
project(PowerPCDisplayOS9 C)

set(CODEC ${CMAKE_CURRENT_SOURCE_DIR}/../host/Sources/CCodec)

add_application(PowerPCDisplay
    mac9.c
    rx_core.c
    ${CODEC}/g4codec.c
    ${CODEC}/g4auth.c
    ${CODEC}/g4nacl.c
    ${CODEC}/g4yuv_altivec.c
    PowerPCDisplay.r
    CREATOR "PPDs"
    )
target_include_directories(PowerPCDisplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CODEC}/include)
# G4_RANDOM_EXTERNAL: mac9.c supplies g4_random (no /dev/urandom on OS 9)
target_compile_definitions(PowerPCDisplay PRIVATE G4_RANDOM_EXTERNAL=1)
target_compile_options(PowerPCDisplay PRIVATE -O2 -mtune=750 -ffunction-sections -Wall -Wno-unused-function -Wno-pointer-sign -Wno-multichar -Wno-array-bounds)
set_target_properties(PowerPCDisplay PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
