cmake_minimum_required(VERSION 3.22.1)
project("shem")

add_library(shem SHARED
    shem_jni.c
    shem_engine.c
    shem_udp.c
)

find_library(log-lib log)
find_library(android-lib android)
find_library(aaudio-lib aaudio)

target_link_libraries(shem
    ${log-lib}
    ${android-lib}
    ${aaudio-lib}
    m
)
