# cmake -DSRC=<file> -DDST=<file> -P copy_if_missing.cmake
# Copies SRC to DST unless DST exists (keeps the player's edited tod.ini).
if(NOT EXISTS "${DST}")
    configure_file("${SRC}" "${DST}" COPYONLY)
endif()
