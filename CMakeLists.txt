cmake_minimum_required(VERSION 3.25)
project(server)

set(CMAKE_CXX_STANDARD 17)

add_executable(server main.cpp)

#target_link_libraries(untitled hiredis) # the same as -l hiredis
target_link_libraries(server mysqlclient)
