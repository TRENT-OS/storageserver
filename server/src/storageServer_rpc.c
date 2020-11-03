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
#include <inttypes.h>

#include <camkes.h>

// Not generated yet by camkes
seL4_Word storageServer_rpc_get_sender_id(void);

// Currently we support up to this amount of clients; in principle this can be
// increased arbitrarily, but it has to go along with the same amount of data
// ports and also the macros for instantiating the server need to be adjusted.
#define STORAGESERVER_MAX_CLIENTS 8

// Our dataports for reading from the top and writing down to the actual
// storage layer; please note that each client has its own dataport
static OS_Dataport_t outPort = OS_DATAPORT_ASSIGN(storage_port);
static OS_Dataport_t inPorts[STORAGESERVER_MAX_CLIENTS] =
{
    OS_DATAPORT_ASSIGN(storageServer1_port),
    OS_DATAPORT_ASSIGN(storageServer2_port),
    OS_DATAPORT_ASSIGN(storageServer3_port),
    OS_DATAPORT_ASSIGN(storageServer4_port),
    OS_DATAPORT_ASSIGN(storageServer5_port),
    OS_DATAPORT_ASSIGN(storageServer6_port),
    OS_DATAPORT_ASSIGN(storageServer7_port),
    OS_DATAPORT_ASSIGN(storageServer8_port),
};

// Clients we have based on the amount of config data
static const size_t clients = sizeof(storageServer_config) /
                              sizeof(struct StorageServer_ClientConfig);

bool init_ok = false;

// Private Functions -----------------------------------------------------------
static OS_Dataport_t*
get_client_port(
    const unsigned int cid)
{
    if (cid < 1 || cid > clients)
    {
        Debug_LOG_ERROR("client ID %u invalid", cid);
        return NULL;
    }

    // Due to the way the macros work, the first client in the connection macro
    // (CONNECT_INSTANCE_StorageServer) always has the highest ID; so we need
    // to invert the mapping here
    return &inPorts[cid - 1];
}

//------------------------------------------------------------------------------
static const struct StorageServer_ClientConfig*
get_client_partition_config(
    const unsigned int cid)
{
    if (cid < 1 || cid > clients)
    {
        Debug_LOG_ERROR("client ID %u invalid", cid);
        return NULL;
    }

    // map the badge id into the config struct
    return &storageServer_config.clients[cid - 1];
}

//------------------------------------------------------------------------------
static bool
get_absolute_offset(
    const unsigned int cid,
    const off_t offset,
    const size_t size,
    off_t* abs_offset)
{
    // set default value
    *abs_offset = 0;

    const struct StorageServer_ClientConfig* p = get_client_partition_config(cid);
    if (NULL == p)
    {
        Debug_LOG_ERROR("no configuration for client ID %u", cid);
        return false;
    }

    off_t const abs_start_offset = p->offset + offset;
    // check overflow
    if (abs_start_offset < p->offset)
    {
        Debug_LOG_ERROR("invalid offset %" PRIiMAX, offset);
        return false;
    }

    off_t const end = abs_start_offset + size;
    // check overflow
    if (end < abs_start_offset)
    {
        Debug_LOG_ERROR("invalid size %d", size);
        return false;
    }

    if (size > p->size)
    {
        Debug_LOG_ERROR("size %d exceeds partition size", size);
        return false;
    }

    *abs_offset = abs_start_offset;
    return true;
}

// All arguments are of type `off_t` on purpose so that the arbitrary large
// storage can be verified.
static
bool
isValidStorageArea(
    off_t const offset,
    off_t const size,
    off_t const storageSize)
{
    // Casting to the biggest possible integer for overflow detection purposes.
    uintmax_t const end = (uintmax_t)offset + (uintmax_t)size;

    // Checking integer overflow first. The end index is not part of the area,
    // but we allow offset = end with size = 0 here.
    //
    // We also do not accept negative offset and sizes (off_t is signed).
    return ((offset >= 0)
            && (size >= 0)
            && (end >= offset)
            && (end <= storageSize));
}

// Public Functions ------------------------------------------------------------

//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "written"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_write(
    off_t   const offset,
    size_t  const size,
    size_t* const written)
{
    *written = 0U;
    seL4_Word cid = storageServer_rpc_get_sender_id();
    OS_Dataport_t* inPort;

    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    inPort = get_client_port(cid);
    if (NULL == inPort || size > OS_Dataport_getSize(*inPort))
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

    if(!isValidStorageArea(
            offset,
            size,
            get_client_partition_config(cid)->size))
    {
        Debug_LOG_ERROR(
            "Offset out of bounds for this client. "
            "offset = %" PRIiMAX ", size = %zu, partitionSize = %" PRId64,
            offset,
            size,
            get_client_partition_config(cid)->size);

        return OS_ERROR_OUT_OF_BOUNDS;
    }

    off_t off;
    if (!get_absolute_offset(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    Debug_LOG_DEBUG(
        "write from client %u, offset=%" PRIiMAX " (-> %" PRIiMAX "), "
        "len %zu",
        cid,
        offset,
        off,
        size);

    memcpy(OS_Dataport_getBuf(outPort), OS_Dataport_getBuf(*inPort), size);

    return storage_rpc_write(off, size, written);
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "read"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_read(
    off_t   const offset,
    size_t  const size,
    size_t* const read)
{
    *read = 0U;
    seL4_Word cid = storageServer_rpc_get_sender_id();
    OS_Dataport_t* inPort;

    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    inPort = get_client_port(cid);
    if (NULL == inPort || size > OS_Dataport_getSize(*inPort))
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

    if(!isValidStorageArea(
            offset,
            size,
            get_client_partition_config(cid)->size))
    {
        Debug_LOG_ERROR(
            "Offset out of bounds for this client. "
            "offset = %" PRIiMAX ", size = %zu, partitionSize = %" PRId64,
            offset,
            size,
            get_client_partition_config(cid)->size);

        return OS_ERROR_OUT_OF_BOUNDS;
    }
    off_t off;
    if (!get_absolute_offset(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    Debug_LOG_DEBUG(
        "read from client %u, offset=%" PRIiMAX " (-> %" PRIiMAX "), len %zu",
        cid,
        offset,
        off,
        size);

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

    memcpy(OS_Dataport_getBuf(*inPort), OS_Dataport_getBuf(outPort), lower_read);

    *read = lower_read;
    return OS_SUCCESS;
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "erased"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_erase(
    off_t  const offset,
    off_t  const size,
    off_t* const erased)
{
    *erased = 0;

    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    // get the calling client's ID
    seL4_Word cid = storageServer_rpc_get_sender_id();

    if(!isValidStorageArea(
            offset,
            size,
            get_client_partition_config(cid)->size))
    {
        Debug_LOG_ERROR(
            "Offset out of bounds for this client. "
            "offset = %" PRIiMAX ", size = %" PRIiMAX ", partitionSize = %" PRId64,
            offset,
            size,
            get_client_partition_config(cid)->size);

        return OS_ERROR_OUT_OF_BOUNDS;
    }

    off_t off;

    if (!get_absolute_offset(cid, offset, size, &off))
    {
        return OS_ERROR_INSUFFICIENT_SPACE;
    }

    Debug_LOG_DEBUG(
        "erase from client %u, offset=%" PRIiMAX " (-> %" PRIiMAX
        "), len %" PRIiMAX,
        cid,
        offset,
        off,
        size);

    return storage_rpc_erase(off, size, erased);
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "size"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_getSize(
    off_t* const size)
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
        return OS_ERROR_INVALID_STATE;
    }

    *size = p->size;

    return OS_SUCCESS;
}

//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "blockSize"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_getBlockSize(
    size_t* const blockSize)
{
    if (!init_ok)
    {
        Debug_LOG_ERROR("fail call since initialization failed");
        return OS_ERROR_INVALID_STATE;
    }

    return storage_rpc_getBlockSize(blockSize);
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
    size_t dp_in_size, dp_out_size = OS_Dataport_getSize(outPort);
    uint8_t i;

    // Check we don't have too many clients
    if (clients > STORAGESERVER_MAX_CLIENTS)
    {
        Debug_LOG_ERROR(
            "Config contains too many clients (%i), currently we support "
            "only %i clients", clients, STORAGESERVER_MAX_CLIENTS);
        return;
    }

    // Make sure both dataports have the same size; this is for simplicity, we
    // we can deal with this later if it should be necessary..
    for (i = 0; i < clients; i++)
    {
        if (OS_Dataport_isUnset(inPorts[clients - i - 1]))
        {
            Debug_LOG_ERROR("Dataport %i is unset, it should be connected "
                            "to the respective client", i);
            return;
        }
        dp_in_size = OS_Dataport_getSize(inPorts[clients - i - 1]);
        if (dp_in_size != dp_out_size)
        {
            Debug_LOG_ERROR(
                "Dataports in (client %i, %zu bytes) and out (%zu bytes) differ",
                i, dp_in_size, dp_out_size);
            return;
        }
    }

    // Check the amount of bytes we have available on the lower device
    off_t sz = 0;
    if ((err = storage_rpc_getSize(&sz)) != OS_SUCCESS)
    {
        Debug_LOG_ERROR("storage_rpc_getSize() failed with %d", err);
        return;
    }

    Debug_LOG_INFO("storage medium size: %" PRIiMAX " bytes", sz);

    // Make sure we can fit all the clients with their sizes and offsets in
    // this underlying storage.
    off_t range = 0;
    for (unsigned int i = 0; i < clients; i++)
    {
        const struct StorageServer_ClientConfig* cli_part =
                &storageServer_config.clients[i];

        Debug_LOG_INFO(
            "client %i: offset=%" PRId64", size=%" PRId64,
            i + 1, cli_part->offset, cli_part->size);

        off_t part_end = cli_part->offset + cli_part->size;
        if (part_end < cli_part->offset)
        {
            Debug_LOG_ERROR(
                "client %i configuration invalid, offset=%" PRId64", size=%" PRId64,
                i,
                cli_part->offset,
                cli_part->size);
        }

        if (cli_part->offset < range)
        {
            Debug_LOG_ERROR(
                "client %i configuration invalid, offset %" PRIiMAX
                " behind used space %" PRId64,
                i,
                range,
                cli_part->offset);
        }

        range = part_end;
    }

    init_ok = true;
}
