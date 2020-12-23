#
# StorageServer
#
# Copyright (C) 2020, Hensoldt Cyber GmbH
#

cmake_minimum_required(VERSION 3.17)


#-------------------------------------------------------------------------------
#
# Declare StorageServer CAmkES Component

function(StorageServer_DeclareCAmkESComponent name)

    DeclareCAmkESComponent(
        ${name}
        SOURCES
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/server/src/storageServer_rpc.c
        C_FLAGS
            -Wall
            -Werror
        LIBS
            os_core_api
            lib_debug
            lib_macros
    )

endfunction()
