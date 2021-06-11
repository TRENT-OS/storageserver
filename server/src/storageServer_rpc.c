/*
 * Copyright (C) 2020, HENSOLDT Cyber GmbH
 */

#include "OS_Error.h"
#include "OS_Dataport.h"

#include "lib_debug/Debug.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>

#include <camkes.h>


#define ARRAY_ELEMENTS(a)   ( sizeof(a) / sizeof(a[0]) )

// Client (badge) ID start with this value.
#define CID_BASE    101

// Not generated yet by camkes
seL4_Word storageServer_rpc_get_sender_id(void);


typedef struct {
    OS_Dataport_t                           inPort;
    const struct StorageServer_ClientConfig *cfg;
} client_context_t;


#define CLIENT_CTX_DATAPORT(n) \
    { \
        .inPort = OS_DATAPORT_ASSIGN(storageServer ## n ## _port), \
        .cfg = (((n)-1) < ARRAY_ELEMENTS(storageServer_config.clients)) ? \
                &storageServer_config.clients[(n)-1] : NULL \
    }

// The number of clients is fixed, but this is basically an arbitrary number
// that must match the partition configuration. Note that the dataport index
// starts counting with 1 and not with zero, that is why CLIENT_CTX_DATAPORT()
// does not get passed a zero.
static const client_context_t all_clients[8] = {
    CLIENT_CTX_DATAPORT(1),
    CLIENT_CTX_DATAPORT(2),
    CLIENT_CTX_DATAPORT(3),
    CLIENT_CTX_DATAPORT(4),
    CLIENT_CTX_DATAPORT(5),
    CLIENT_CTX_DATAPORT(6),
    CLIENT_CTX_DATAPORT(7),
    CLIENT_CTX_DATAPORT(8),
};

// dataport shared with lower layer
static const OS_Dataport_t outPort = OS_DATAPORT_ASSIGN(storage_port);

// The global marker stating wether the initialization was successful must be
// flagged volatile when strictly following the C rules. A hypothetical highly
// advanced optimizer could turn global variable accesses into constants, if it
// concludes the global state is always well known. Also, there is no rule in C
// that global variables must be synced with memory content on function entry
// and exit - it is just something that happen due to practical reasons. There
// is not even a rule that functions must be preserved and can't be inlined,
// which would eventually allow caching global variables easily. Furthermore, C
// also does not know threads nor concurrent execution of functions, but both
// have a string impact on global variables.
// Using volatile here guarantees at least, that accesses to global variables
// are accessing the actual memory in the given order stated in the program and
// there is no caching or constant folding that removes memory accesses. That is
// the best we can do to avoid surprises at higher optimization levels.
volatile bool init_ok = false;

// Private Functions -----------------------------------------------------------

//------------------------------------------------------------------------------
static
OS_Error_t
check_server_and_client_state(
    seL4_Word cid,
    const client_context_t **client_ctx)
{
    if (client_ctx) // caller might not be interested in the actual client_ctx
    {
        *client_ctx = NULL; // set default value
    }
    // Check if server is fully up and running.
    if (!init_ok)
    {
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] server initialization failed, fail call",
            cid);
        return OS_ERROR_INVALID_STATE;
    }

    // Check that the caller's ID is in the supported range.
    if ((cid < CID_BASE) || (cid > CID_BASE + ARRAY_ELEMENTS(all_clients)))
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] client ID out of range", cid);
        return OS_ERROR_INVALID_PARAMETER;
    }

    const client_context_t *my_client_ctx = &all_clients[cid - CID_BASE];

    if (client_ctx) // caller might not be interested in the actual client_ctx
    {
        *client_ctx = my_client_ctx;
    }

    // Check if the client has a partition.
    if (!my_client_ctx->cfg)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] no partition assigned", cid);
        return OS_ERROR_DEVICE_INVALID;
    }

    return OS_SUCCESS;
}


//------------------------------------------------------------------------------
static
OS_Error_t
NONNULL_ALL
check_request_area(
    const client_context_t* client_ctx,
    const off_t offset,
    const size_t size,
    off_t* p_abs_offset)
{
    assert(client_ctx->cfg);

    *p_abs_offset = 0; // set default value

    int64_t partition_size = client_ctx->cfg->size;
    assert(partition_size >= 0); // config must be sane

    if (size > partition_size)
    {
        Debug_LOG_ERROR(
            "size %zu exceeds the partition size %"PRId64,
            size, partition_size);
        return OS_ERROR_OUT_OF_BOUNDS;
    }

    // check if area is sane and there are no integer overflow. Since off_t is
    // signed, we have to reject negative values.
    if ( (offset < 0) || (offset >= partition_size)
         || (size < 0) || (size > partition_size - offset))
    {
        Debug_LOG_ERROR(
            "invalid area, offset %" PRIiMAX ", size %zu, partition size %"PRIiMAX,
            offset, size, partition_size);
        return OS_ERROR_OUT_OF_BOUNDS;
    }

    int64_t partition_offset = client_ctx->cfg->offset;
    assert(partition_offset >= 0); // config must be sane
    const off_t abs_start_offset = partition_offset + offset;
    // check overflow for absolute offset
    if (abs_start_offset < partition_offset)
    {
        Debug_LOG_ERROR("invalid absolute offset %" PRIiMAX, offset);
        return OS_ERROR_OUT_OF_BOUNDS;
    }

    const off_t end = abs_start_offset + size;
    // check size overflow with absolute offset
    if (end < abs_start_offset)
    {
        Debug_LOG_ERROR("invalid size %zu", size);
        return OS_ERROR_OUT_OF_BOUNDS;
    }

    *p_abs_offset = abs_start_offset;
    return OS_SUCCESS;
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
    size_t* const p_written)
{
    OS_Error_t ret;

    *p_written = 0; // set default return value

    // Check if we can handle this client's request.
    seL4_Word cid = storageServer_rpc_get_sender_id();
    const  client_context_t *client_ctx = NULL;
    ret = check_server_and_client_state(cid, &client_ctx);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] reject request", cid);
        return ret;
    }

    assert(client_ctx->cfg); // if we are here, client must have a partition

    size_t portSizeUpper = OS_Dataport_getSize(client_ctx->inPort);
    size_t portSizeLower = OS_Dataport_getSize(outPort);
    if ((size > portSizeUpper) || (size > portSizeLower))
    {
        // The client should know the maximum size from the system
        // configuration. We could process the data in chunks here instead of
        // failing the request, but that is currently not implemented.
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] size %zu exceeds max supported dataport "
            "sizes (upper %zu, lower %zu)",
            cid, size, portSizeUpper, portSizeLower);
        return OS_ERROR_BUFFER_TOO_SMALL;
    }

    off_t abs_off = 0;
    ret = check_request_area(client_ctx, offset, size, &abs_off);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] write area invalid", cid);
        return ret;
    }

    Debug_LOG_DEBUG(
        "[CID %"SEL4_PRI_word"] write offset=%" PRIiMAX " (-> %" PRIiMAX "), "
        "len %zu",
        cid,
        offset,
        abs_off,
        size);

    memcpy(
        OS_Dataport_getBuf(outPort),
        OS_Dataport_getBuf(client_ctx->inPort),
        size);

    return storage_rpc_write(abs_off, size, p_written);
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "read"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_read(
    off_t   const offset,
    size_t  const size,
    size_t* const p_read)
{
    OS_Error_t ret;

    *p_read = 0; // set default return value

    // Check if we can handle this client's request.
    seL4_Word cid = storageServer_rpc_get_sender_id();
    const client_context_t *client_ctx = NULL;
    ret = check_server_and_client_state(cid, &client_ctx);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] reject request", cid);
        return ret;
    }

    assert(client_ctx->cfg); // if we are here, client must have a partition

    size_t portSizeUpper = OS_Dataport_getSize(client_ctx->inPort);
    size_t portSizeLower = OS_Dataport_getSize(outPort);
    if ((size > portSizeUpper) || (size > portSizeLower))
    {
        // The client should know the maximum size from the system
        // configuration. We could process the data in chunks here instead of
        // failing the request, but that is currently not implemented.
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] size %zu exceeds max supported dataport "
            "sizes (upper %zu, lower %zu)",
            cid, size, portSizeUpper, portSizeLower);
        return OS_ERROR_BUFFER_TOO_SMALL;
    }

    off_t abs_off = 0;
    ret = check_request_area(client_ctx, offset, size, &abs_off);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] read area invalid", cid);
        return ret;
    }

    Debug_LOG_DEBUG(
        "[CID %"SEL4_PRI_word"] read at offset=%" PRIiMAX " (-> %" PRIiMAX
        "), len %zu",
        cid,
        offset,
        abs_off,
        size);

    size_t lower_read = 0;
    ret = storage_rpc_read(abs_off, size, &lower_read);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] lower read failed, lower_read=%zu, ret=%d",
            cid, lower_read, ret);
        // ToDo: handle the case where we lower storage driver returned an
        //       error, but also set lower_read > 0. Should we copy the data?
        return ret;
    }

    // do a sanity check for lower_read < size, we can't really trust the lower
    // layer and bogus data should not fool us
    if (lower_read > size)
    {
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] invalid lower_read %zu", cid, lower_read);
        return OS_ERROR_INVALID_STATE;
    }

    memcpy(
        OS_Dataport_getBuf(client_ctx->inPort),
        OS_Dataport_getBuf(outPort),
        lower_read);

    *p_read = lower_read;
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
    off_t* const p_erased)
{
    OS_Error_t ret;

    *p_erased = 0; // set default return value

    // Check if we can handle this client's request.
    seL4_Word cid = storageServer_rpc_get_sender_id();
    const client_context_t *client_ctx = NULL;
    ret = check_server_and_client_state(cid, &client_ctx);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] reject request", cid);
        return ret;
    }

    assert(client_ctx->cfg); // if we are here, client must have a partition

    off_t abs_off = 0;
    ret = check_request_area(client_ctx, offset, size, &abs_off);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] erase area invalid", cid);
        return ret;
    }

    Debug_LOG_DEBUG(
        "[CID %"SEL4_PRI_word"] erase at offset=%" PRIiMAX " (-> %" PRIiMAX
        "), len %" PRIiMAX,
        cid,
        offset,
        abs_off,
        size);

    ret = storage_rpc_erase(abs_off, size, p_erased);
    if (ret != OS_SUCCESS)
    {
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] storage_rpc_erase() failed, code: %d",
            cid, ret);

        return ret;
    }

    return OS_SUCCESS;
}


//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "size"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_getSize(
    off_t* const p_size)
{
    OS_Error_t ret;

    *p_size = 0; // set default return value

    // Check if we can handle this client's request.
    seL4_Word cid = storageServer_rpc_get_sender_id();
    const client_context_t *client_ctx = NULL;
    ret = check_server_and_client_state(cid, &client_ctx);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] reject request", cid);
        return ret;
    }

    assert(client_ctx->cfg); // if we are here, client must have a partition

    *p_size = client_ctx->cfg->size;

    // We need to forward the error code of the underlying storage e.g. in case
    // the medium is not present, so that StorageServer is transparent.
    //
    // Additionally we can verify the config server configuration and raise an
    // error.
    off_t storageSize = 0;
    ret = storage_rpc_getSize(&storageSize);
    if (ret != OS_SUCCESS)
    {
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] storage_rpc_getSize() failed, code: %d",
            cid, ret);

        return ret;
    }

    // We don't care about the `size + offset` overflow, because there is no
    // true impact beside sending a warning message, and the overflow is
    // detected in the `post_init` function.
    if(storageSize < (client_ctx->cfg->size + client_ctx->cfg->offset))
    {
        Debug_LOG_WARNING(
            "[CID %"SEL4_PRI_word"] underlying storage too small. Check "
            " StorageServer config: storageSize = %" PRIiMAX
            " clientStorageSize = %" PRIiMAX
            " clientStorageOffset = %" PRIiMAX,
            cid,
            storageSize,
            client_ctx->cfg->size,
            client_ctx->cfg->offset);
    }

    return OS_SUCCESS;
}

//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "blockSize"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_getBlockSize(
    size_t* const p_blockSize)
{
    OS_Error_t ret;

    *p_blockSize = 0; // set default return value

    // Check if we can handle this client's request.
    seL4_Word cid = storageServer_rpc_get_sender_id();
    ret = check_server_and_client_state(cid, NULL);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] reject request", cid);
        return ret;
    }

    ret = storage_rpc_getBlockSize(p_blockSize);
    if (ret != OS_SUCCESS)
    {
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] storage_rpc_getBlockSize() failed, code: %d",
            cid, ret);

        return ret;
    }

    return OS_SUCCESS;
}

//------------------------------------------------------------------------------
// This is a CAmkES RPC interface handler. It's guaranteed that "flags"
// never points to NULL.
OS_Error_t
NONNULL_ALL
storageServer_rpc_getState(
    uint32_t* p_flags)
{
    OS_Error_t ret;

    *p_flags = 0; // set default return value

    // Check if we can handle this client's request.
    seL4_Word cid = storageServer_rpc_get_sender_id();
    ret = check_server_and_client_state(cid, NULL);
    if (OS_SUCCESS != ret)
    {
        Debug_LOG_ERROR("[CID %"SEL4_PRI_word"] reject request", cid);
        return ret;
    }

    ret = storage_rpc_getState(p_flags);
    if (ret != OS_SUCCESS)
    {
        Debug_LOG_ERROR(
            "[CID %"SEL4_PRI_word"] storage_rpc_getState() failed, code: %d",
            cid, ret);

        return ret;
    }

    return OS_SUCCESS;
}


//------------------------------------------------------------------------------
void
post_init(void)
{
    OS_Error_t ret;
    const size_t dp_out_size = OS_Dataport_getSize(outPort);

    // Check the amount of bytes we have available on the lower device
    off_t sz = 0;
    ret = storage_rpc_getSize(&sz);
    if (OS_SUCCESS == ret)
    {
        Debug_LOG_WARNING("storage_rpc_getSize() failed with %d", ret);
    }
    else
    {
        Debug_LOG_INFO("storage medium size: %" PRIiMAX " bytes", sz);
    }

    // Check that the storage partition configuration is valid.
    off_t range = 0;
    for (unsigned int i = 0; i < ARRAY_ELEMENTS(storageServer_config.clients); i++)
    {
        const struct StorageServer_ClientConfig* cli_part =
                &storageServer_config.clients[i];

        Debug_LOG_INFO(
            "partition %u: offset=%" PRId64", size=%" PRId64,
            i, cli_part->offset, cli_part->size);

        off_t part_end = cli_part->offset + cli_part->size;
        if (part_end < cli_part->offset)
        {
            Debug_LOG_ERROR(
                "partition %u configuration invalid, offset=%" PRId64", size=%" PRId64,
                i,
                cli_part->offset,
                cli_part->size);
        }

        if (cli_part->offset < range)
        {
            Debug_LOG_ERROR(
                "partition %u configuration invalid, offset %" PRIiMAX
                " behind used space %" PRId64,
                i,
                range,
                cli_part->offset);
        }

        range = part_end;
    }

    // Setup the context for each client.
    for (unsigned int i = 0; i < ARRAY_ELEMENTS(all_clients); i++)
    {
        const client_context_t *client_ctx = &all_clients[i];

        // The dataport can be unset if there is no client connected.
        if (OS_Dataport_isUnset(client_ctx->inPort))
        {
            Debug_LOG_WARNING(
                "client %u dataport (storageServer_%d_port) not connected",
                i, i+1);
        }

        // Make sure that the client data port size matches the size of our
        // lower dataports to the actual storage driver.
        size_t dp_in_size = OS_Dataport_getSize(client_ctx->inPort);
        if (dp_in_size > dp_out_size)
        {
            Debug_LOG_ERROR(
                "client %u: upper (%zu) and lower (%zu) dataports sizes differ",
                i, dp_in_size, dp_out_size);
            return;
        }

        if (!client_ctx->cfg)
        {
            Debug_LOG_WARNING("client %u has no partition assigned", i);
        }
    }

    Debug_LOG_INFO("Storage Server running");

    init_ok = true;
}
