cmake_minimum_required(VERSION 3.16)

project(Messenger
    VERSION 1.0.0
    DESCRIPTION "Клиент-серверный мессенджер на Qt6/C++17"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network Widgets Sql)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(COMMON_HEADERS
    common/Protocol.h
)

set(SERVER_SOURCES
    server/main.cpp
    server/ServerCore.cpp
    server/ServerCore.h
    server/DatabaseServer.cpp
    server/DatabaseServer.h
    server/ClientHandler.cpp
    server/ClientHandler.h
)

add_executable(messenger_server ${SERVER_SOURCES})

target_link_libraries(messenger_server PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
)

target_include_directories(messenger_server PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)

set(CLIENT_SOURCES
    client/main.cpp
    client/LoginWindow.cpp
    client/LoginWindow.h
    client/MainWindow.cpp
    client/MainWindow.h
    client/NetworkManager.cpp
    client/NetworkManager.h
    client/DatabaseClient.cpp
    client/DatabaseClient.h
    client/styles.qss
)

add_executable(messenger_client ${CLIENT_SOURCES})

target_link_libraries(messenger_client PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
    Qt6::Sql
)

target_include_directories(messenger_client PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)

install(TARGETS messenger_server messenger_client
    RUNTIME DESTINATION bin
)

message(STATUS "Qt6 Version: ${Qt6_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Server target: messenger_server")
message(STATUS "Client target: messenger_client")
