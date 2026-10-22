cmake_minimum_required(VERSION 3.20)
project(tc2vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(dinput8 SHARED
    src/main.cpp
    src/log.cpp
    src/game.cpp
    src/stereo.cpp
    src/present.cpp
    src/render_hook.cpp
    src/ring_fix.cpp
    src/xr.cpp
    src/mirror.cpp
    src/dinput8.def
)

set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")
target_include_directories(dinput8 PRIVATE third_party/openxr/include)
target_compile_options(dinput8 PRIVATE -O2 -Wall -Wextra -fno-exceptions
    -Wno-cast-function-type -Wno-missing-field-initializers -mcx16)
target_link_options(dinput8 PRIVATE -static -static-libgcc -static-libstdc++ -s)
target_link_libraries(dinput8 PRIVATE dxguid advapi32)
