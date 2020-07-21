/*
 * Copyright (C) 2020, HENSOLDT Cyber GmbH
 */

#include "OS_Error.h"
#include "OS_Dataport.h"

#include "LibDebug/Debug.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
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
                              sizeof(struct StorageServer_ClientConfig);

bool init_ok = false;

// Private Functions -----------------------------------------------------------


//------------------------------------------------------------------------------
static const struct StorageServer_ClientConfig*
get_client_partition_config(
    const unsigned int cid
) {
    if (cid > clients)
    {
        Debug_LOG_ERROR("client ID %u invalid", cid);
        return NULL;
    }

    // map the badge id into the config struct
    return &storageServer_config.clients[cid - 1];
}


static bool
get_absolute_offset(
    const unsigned int cid,
    const size_t offset,
    const size_t size,
    size_t* abs_offset)
{
    // set default value
    *abs_offset = 0;

    const struct StorageServer_ClientConfig* p = get_client_partition_config(cid);
    if (NULL == p)
    {
        Debug_LOG_ERROR("no configuration for client ID %u", cid);
        return false;
    }

    size_t const abs_start_offet = p->offset + offset;
    // check overflow
    if (abs_start_offet < p->offset)
    {
        Debug_LOG_ERROR("invalid offset %d", offset);
        return false;
    }

    size_t const end = abs_start_offet + size;
    // check overflow
    if (end < abs_start_offet)
    {
        Debug_LOG_ERROR("invalid size %d", size);
        return false;
    }

    if (size > p->size)
    {
        Debug_LOG_ERROR("size %d exceeds partition size", size);
        return false;
    }

    *abs_offset = abs_start_offet;
    return true;
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
    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    if (size > OS_Dataport_getSize(inPort))
    {
        // the client did a bogus request, it knows the data port size and
        // never ask for more data
        return OS_ERROR_INVALID_PARAMETER;
    }

    if (size > OS_Dataport_getSize(outPort))
    {
        // our lower data port is not big enough for this request, but the
        // client can know this. We could write the data in chunks here instead
        // of failing the request
        return OS_ERROR_BUFFER_TOO_SMALL;
    }

    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();

    size_t off;
    if (!get_absolute_offset(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    Debug_LOG_DEBUG(
        "write from client %u, offet=%zu (-> %zu), len %zu",
        cid, offset, off, size);


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
    // set default value
    *read = 0;

    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    if (size > OS_Dataport_getSize(inPort))
    {
        // the client did a bogus request, it knows the data port size and
        // never ask for more data
        return OS_ERROR_INVALID_PARAMETER;
    }

    if (size > OS_Dataport_getSize(outPort))
    {
        // our lower data port is not big enough for this request, but the
        // client can know this. We could read the data in chunks here instead
        // of failing the request
        return OS_ERROR_BUFFER_TOO_SMALL;
    }

    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();

    size_t off;

    if (!get_absolute_offset(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    Debug_LOG_DEBUG(
        "read from client %u, offet=%zu (-> %zu), len %zu",
        cid, offset, off, size);

    size_t lower_read = 0;
    OS_Error_t ret = storage_rpc_read(off, size, &lower_read);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR(
            "lower read failed, lower_read=%zu, ret=%d",
            lower_read, ret);
        // ToDo: handle the case where we lower storage driver returned an
        //       error, but also set lower_read > 0. Should we copy the data?
        return ret;
    }

    // do a sanity check for lower_read < size, we can't really trust the lower
    // layer and bogus data should not fool us
    if (lower_read > size)
    {
        Debug_LOG_ERROR("invalid lower_read %zu", lower_read);
        return OS_ERROR_INVALID_STATE;
    }

    memcpy(OS_Dataport_getBuf(inPort), OS_Dataport_getBuf(outPort), lower_read);

    *read = lower_read;
    return OS_SUCCESS;
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
    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();

    size_t off;

    if (!get_absolute_offset(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    Debug_LOG_DEBUG(
        "erase from client %u, offet=%zu (-> %zu), len %zu",
        cid, offset, off, size);

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
    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();

    const struct StorageServer_ClientConfig* p = get_client_partition_config(cid);
    if (NULL == p)
    {
        Debug_LOG_ERROR("no configuration for client ID %u", cid);
        return false;
    }

    *size = p->size;

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
    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    return storage_rpc_getState(flags);
}


//------------------------------------------------------------------------------
void
post_init(
    void)
{
    OS_Error_t err;

    // Make sure both dataports have the same size; this is for simplicity, we
    // we can deal with this later if it should be necessary..
    size_t dp_in_size = OS_Dataport_getSize(inPort);
    size_t dp_out_size = OS_Dataport_getSize(outPort);
    if (dp_in_size != dp_out_size)
    {
        Debug_LOG_ERROR(
            "Dataports in (%zu bytes) and out (%zu bytes) differ",
            dp_in_size, dp_out_size);
        return;
    }

    // Check the amount of bytes we have available on the lower device
    size_t sz = 0;
    if ((err = storage_rpc_getSize(&sz)) != OS_SUCCESS)
    {
        Debug_LOG_ERROR("storage_rpc_getSize() failed with %d", err);
        return;
    }

    Debug_LOG_INFO("storage medium size: %d bytes", sz);

    // Make sure we can fit all the clients with their sizes and offsets in
    // this underlying storage.
    size_t range = 0;
    for (unsigned int i = 0; i < clients; i++)
    {
        const struct StorageServer_ClientConfig* cli_part = &storageServer_config.clients[i];

        Debug_LOG_INFO(
                "client %i: offset=%zu, size=%zu",
                i + 1, cli_part->offset, cli_part->size);

        size_t part_end = cli_part->offset + cli_part->size;
        if (part_end < cli_part->offset)
        {
            Debug_LOG_ERROR(
                "client %i configuration invalid, offset=%zu, size=%zu",
                i, cli_part->offset, cli_part->size);
        }

        if (cli_part->offset < range)
        {
            Debug_LOG_ERROR(
                "client %i configuration invalid, offset %zu behind used space %zu",
                i, range, cli_part->offset);
        }

        range = part_end;
    }

    init_ok = true;
}
