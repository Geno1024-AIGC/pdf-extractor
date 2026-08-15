cmake_minimum_required(VERSION 3.16)
project(pdf-extractor LANGUAGES CXX)

if(DEFINED MY_SDK)
    # Point find_package(Qt5)/ZLIB at a local dev tree (no system Qt required).
    list(APPEND CMAKE_PREFIX_PATH "${MY_SDK};${MY_SDK}/lib/x86_64-linux-gnu")
    list(APPEND CMAKE_INCLUDE_PATH
        "${MY_SDK}/include/x86_64-linux-gnu/qt5"
        "${MY_SDK}/include")
    list(APPEND CMAKE_LIBRARY_PATH "${MY_SDK}/lib/x86_64-linux-gnu")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets)
find_package(ZLIB REQUIRED)

add_executable(pdfx
    src/main.cpp
    src/pdfparser.cpp
    src/pdfparser.h
    src/cli.cpp
    src/cli.h
    src/extractor.cpp
    src/extractor.h
    src/filters.cpp
    src/filters.h
    src/mainwindow.cpp
    src/mainwindow.h
)

target_include_directories(pdfx PRIVATE src)
target_link_libraries(pdfx PRIVATE Qt5::Widgets ZLIB::ZLIB)