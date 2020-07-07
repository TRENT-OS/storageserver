/*
 * Copyright (C) 2020, HENSOLDT Cyber GmbH
 */

#include "OS_Error.h"
#include "OS_Dataport.h"

#include "LibDebug/Debug.h"

#include <stddef.h>
#include <string.h>

#include <camkes.h>

// Not generated yet by camkes
seL4_Word storageServer_rpc_get_sender_id(void);

// Our dataports for reading from the top and writing down to the actual
// storage layer
static OS_Dataport_t outPort = OS_DATAPORT_ASSIGN(storage_dp);
static OS_Dataport_t inPort = OS_DATAPORT_ASSIGN(storageServer_dp);

// Clients we have based on the amount of config data
static const size_t clients = sizeof(storageServer_config) /
                              sizeof(struct ClientConfig);

// Private Functions -----------------------------------------------------------

//------------------------------------------------------------------------------
static bool
mapToStorage(
    const unsigned int cid,
    const size_t offset,
    const size_t size,
    size_t* newOff)
{
    // Make we can map the badge id into the config struct
    Debug_ASSERT_PRINTFLN(cid > 0 && cid <= clients,
                          "Client ID (%i) exceeds excpedet range", cid);

    size_t sz = storageServer_config.clients[cid - 1].size;
    size_t off = storageServer_config.clients[cid - 1].offset;

    *newOff = off + offset;

    return (offset + size < sz);
}

// Public Functions ------------------------------------------------------------

//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "written"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_write(
    size_t  const offset,
    size_t  const size,
    size_t* const written)
{
    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();

    size_t off;

    if (!mapToStorage(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    if (size > OS_Dataport_getSize(outPort))
    {
        return OS_ERROR_BUFFER_TOO_SMALL;
    }

    memcpy(OS_Dataport_getBuf(outPort), OS_Dataport_getBuf(inPort), size);

    return storage_rpc_write(off, size, written);
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "read"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_read(
    size_t  const offset,
    size_t  const size,
    size_t* const read)
{
    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();

    OS_Error_t err;
    size_t off;

    if (!mapToStorage(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    if ((err = storage_rpc_read(off, size, read)) == OS_SUCCESS)
    {
        if (*read > OS_Dataport_getSize(inPort))
        {
            return OS_ERROR_BUFFER_TOO_SMALL;
        }

        memcpy(OS_Dataport_getBuf(inPort), OS_Dataport_getBuf(outPort), *read);
    }

    return err;
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "erased"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_erase(
    size_t  const offset,
    size_t  const size,
    size_t* const erased)
{
    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();

    size_t off;

    if (!mapToStorage(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    return storage_rpc_erase(off, size, erased);
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "size"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_getSize(
    size_t* const size)
{
    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();
    *size = storageServer_config.clients[cid - 1].size;

    return OS_SUCCESS;
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "flags"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_getState(
    uint32_t* flags)
{
    return storage_rpc_getState(flags);
}


//------------------------------------------------------------------------------
void
post_init(
    void)
{
    OS_Error_t err;
    size_t i, sz, range;

    // Check the amount of bytes we have available on the lower device
    if ((err = storage_rpc_getSize(&sz)) != OS_SUCCESS)
    {
        Debug_LOG_ERROR("storage_rpc_getSize() failed with %d", err);
        return;
    }

    // Make sure we can fit all the clients with their sizes and offsets in
    // this range; here we see how the individual offsets + sizes are simply
    // added up ..
    range = 0;
    for (i = 0; i < clients; i++)
    {
        range += storageServer_config.clients[i].offset +
                 storageServer_config.clients[i].size;
    }
    Debug_ASSERT_PRINTFLN(range <= sz,
                          "Client configuration (%zu bytes) exceeds "
                          "underlying storage size (%zu bytes)",
                          range, sz);

    // Make sure both dataports have the same size; this is for simplicity, we
    // we can deal with this later if it should be necessary..
    Debug_ASSERT_PRINTFLN(OS_Dataport_getSize(inPort) ==
                          OS_Dataport_getSize(outPort),
                          "Dataports have different sizes");
}
