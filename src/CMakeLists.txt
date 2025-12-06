#If it has both a .c and .h file, make it a library. Also, link
# libraries appropriately here. Targets should be added in dependency order - if
# executable bar needs library foo, then you should do
#   add_library(foo foo.c foo.h)
#   add_executable(bar bar.c)
#   target_link_libraries(bar PRIVATE foo)
# or similar. The PRIVATE/INTERFACE/PUBLIC keyword will depend on whether the
# library is used only in function bodies (PRIVATE), only in function
# signatures/types (INTERFACE), or both (PUBLIC).
add_library(rfs_file client/rfs_file.c include/rfs_file.h)
target_include_directories(rfs_file PUBLIC include)

add_library(socket_client client/socket_client.c include/socket_client.h)
target_include_directories(socket_client PUBLIC include)

add_library(args INTERFACE)
target_include_directories(args INTERFACE include/args)

# If you have a program that's just a .c file and it has a main method, define
# an executable. 
add_executable(client client/file_watcher.c)
add_executable(server server/server.c)

# Link libraries to their required libraries
target_link_libraries(socket_client
    PUBLIC args
    PRIVATE rfs_file
)

# Link executables to their required libraries
target_link_libraries(client
    PRIVATE rfs_file
    PRIVATE socket_client
)