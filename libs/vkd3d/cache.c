/*
 * Copyright 2020 Philip Rebohle for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#include "vkd3d_private.h"
#include "vkd3d_shader.h"

static size_t vkd3d_compute_size_varint(const uint32_t *words, size_t word_count)
{
    size_t size = 0;
    uint32_t w;
    size_t i;

    for (i = 0; i < word_count; i++)
    {
        w = words[i];
        if (w < (1u << 7))
            size += 1;
        else if (w < (1u << 14))
            size += 2;
        else if (w < (1u << 21))
            size += 3;
        else if (w < (1u << 28))
            size += 4;
        else
            size += 5;
    }
    return size;
}

static uint8_t *vkd3d_encode_varint(uint8_t *buffer, const uint32_t *words, size_t word_count)
{
    uint32_t w;
    size_t i;
    for (i = 0; i < word_count; i++)
    {
        w = words[i];
        if (w < (1u << 7))
            *buffer++ = w;
        else if (w < (1u << 14))
        {
            *buffer++ = 0x80u | ((w >> 0) & 0x7f);
            *buffer++ = (w >> 7) & 0x7f;
        }
        else if (w < (1u << 21))
        {
            *buffer++ = 0x80u | ((w >> 0) & 0x7f);
            *buffer++ = 0x80u | ((w >> 7) & 0x7f);
            *buffer++ = (w >> 14) & 0x7f;
        }
        else if (w < (1u << 28))
        {
            *buffer++ = 0x80u | ((w >> 0) & 0x7f);
            *buffer++ = 0x80u | ((w >> 7) & 0x7f);
            *buffer++ = 0x80u | ((w >> 14) & 0x7f);
            *buffer++ = (w >> 21) & 0x7f;
        }
        else
        {
            *buffer++ = 0x80u | ((w >> 0) & 0x7f);
            *buffer++ = 0x80u | ((w >> 7) & 0x7f);
            *buffer++ = 0x80u | ((w >> 14) & 0x7f);
            *buffer++ = 0x80u | ((w >> 21) & 0x7f);
            *buffer++ = (w >> 28) & 0x7f;
        }
    }

    return buffer;
}

static bool vkd3d_decode_varint(uint32_t *words, size_t words_size, const uint8_t *buffer, size_t buffer_size)
{
    size_t offset = 0;
    uint32_t shift;
    uint32_t *w;
    size_t i;

    for (i = 0; i < words_size; i++)
    {
        w = &words[i];
        *w = 0;

        shift = 0;
        do
        {
            if (offset >= buffer_size || shift >= 32u)
                return false;

            *w |= (buffer[offset] & 0x7f) << shift;
            shift += 7;
        } while (buffer[offset++] & 0x80);
    }

    return buffer_size == offset;
}

VkResult vkd3d_create_pipeline_cache(struct d3d12_device *device,
        size_t size, const void *data, VkPipelineCache *cache)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    VkPipelineCacheCreateInfo info;
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.pNext = NULL;
    info.flags = 0;
    info.initialDataSize = size;
    info.pInitialData = data;

    return VK_CALL(vkCreatePipelineCache(device->vk_device, &info, NULL, cache));
}

#define VKD3D_CACHE_BLOB_VERSION MAKE_MAGIC('V','K','B',3)

enum vkd3d_pipeline_blob_chunk_type
{
    /* VkPipelineCache blob data. */
    VKD3D_PIPELINE_BLOB_CHUNK_TYPE_PIPELINE_CACHE = 0,
    /* VkShaderStage is stored in upper 16 bits. */
    VKD3D_PIPELINE_BLOB_CHUNK_TYPE_VARINT_SPIRV = 1,
    /* For when a blob is stored inside a pipeline library, we can reference blobs by hash instead
     * to achieve de-dupe. We'll need to maintain the older path as well however since we need to support GetCachedBlob()
     * as a standalone thing as well. */
    VKD3D_PIPELINE_BLOB_CHUNK_TYPE_PIPELINE_CACHE_LINK = 2,
    /* VkShaderStage is stored in upper 16 bits. */
    VKD3D_PIPELINE_BLOB_CHUNK_TYPE_VARINT_SPIRV_LINK = 3,
    /* VkShaderStage is stored in upper 16 bits. */
    VKD3D_PIPELINE_BLOB_CHUNK_TYPE_SHADER_META = 4,
    VKD3D_PIPELINE_BLOB_CHUNK_TYPE_PSO_COMPAT = 5,
    VKD3D_PIPELINE_BLOB_CHUNK_TYPE_MASK = 0xffff,
    VKD3D_PIPELINE_BLOB_CHUNK_INDEX_SHIFT = 16,
};

#define VKD3D_PIPELINE_BLOB_CHUNK_ALIGN 8
struct vkd3d_pipeline_blob_chunk
{
    uint32_t type; /* vkd3d_pipeline_blob_chunk_type with extra data in upper bits. */
    uint32_t size; /* size of data. Does not include size of header. */
    uint8_t data[]; /* struct vkd3d_pipeline_blob_chunk_*. */
};

struct vkd3d_pipeline_blob_chunk_spirv
{
    uint32_t decompressed_spirv_size;
    uint32_t compressed_spirv_size;
    uint8_t data[];
};

struct vkd3d_pipeline_blob_chunk_link
{
    uint64_t hash;
};

struct vkd3d_pipeline_blob_chunk_shader_meta
{
    struct vkd3d_shader_meta meta;
};

struct vkd3d_pipeline_blob_chunk_pso_compat
{
    uint64_t root_signature_compat_hash;
};

STATIC_ASSERT(sizeof(struct vkd3d_pipeline_blob_chunk) == 8);
STATIC_ASSERT(offsetof(struct vkd3d_pipeline_blob_chunk, data) == 8);
STATIC_ASSERT(sizeof(struct vkd3d_pipeline_blob_chunk_spirv) == 8);
STATIC_ASSERT(sizeof(struct vkd3d_pipeline_blob_chunk_spirv) == offsetof(struct vkd3d_pipeline_blob_chunk_spirv, data));

#define VKD3D_PIPELINE_BLOB_ALIGN 8
struct vkd3d_pipeline_blob
{
    uint32_t version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t checksum; /* Simple checksum for data[] as a sanity check. uint32_t because it conveniently packs here. */
    uint64_t vkd3d_build;
    uint64_t vkd3d_shader_interface_key;
    uint8_t cache_uuid[VK_UUID_SIZE];
    uint8_t data[]; /* vkd3d_pipeline_blob_chunks laid out one after the other with u32 alignment. */
};

/* Used for de-duplicated pipeline cache and SPIR-V hashmaps. */
struct vkd3d_pipeline_blob_internal
{
    uint32_t checksum; /* Simple checksum for data[] as a sanity check. */
    uint8_t data[]; /* Either raw uint8_t for pipeline cache, or vkd3d_pipeline_blob_chunk_spirv. */
};

STATIC_ASSERT(offsetof(struct vkd3d_pipeline_blob, data) == (32 + VK_UUID_SIZE));
STATIC_ASSERT(offsetof(struct vkd3d_pipeline_blob, data) == sizeof(struct vkd3d_pipeline_blob));

static uint32_t vkd3d_pipeline_blob_compute_data_checksum(const uint8_t *data, size_t size)
{
    uint64_t h = hash_fnv1_init();
    size_t i;

    for (i = 0; i < size; i++)
        h = hash_fnv1_iterate_u8(h, data[i]);
    return hash_uint64(h);
}

static const struct vkd3d_pipeline_blob_chunk *find_blob_chunk(const struct vkd3d_pipeline_blob_chunk *chunk,
        size_t size, uint32_t type)
{
    uint32_t aligned_chunk_size;

    while (size >= sizeof(struct vkd3d_pipeline_blob_chunk))
    {
        aligned_chunk_size = align(chunk->size + sizeof(struct vkd3d_pipeline_blob_chunk),
                VKD3D_PIPELINE_BLOB_CHUNK_ALIGN);
        if (aligned_chunk_size > size)
            return NULL;
        if (chunk->type == type)
            return chunk;

        chunk = (const struct vkd3d_pipeline_blob_chunk *)&chunk->data[align(chunk->size, VKD3D_PIPELINE_BLOB_CHUNK_ALIGN)];
        size -= aligned_chunk_size;
    }

    return NULL;
}

HRESULT d3d12_cached_pipeline_state_validate(struct d3d12_device *device,
        const struct d3d12_cached_pipeline_state *state,
        vkd3d_shader_hash_t root_signature_compat_hash)
{
    const VkPhysicalDeviceProperties *device_properties = &device->device_info.properties2.properties;
    const struct vkd3d_pipeline_blob *blob = state->blob.pCachedBlob;
    const struct vkd3d_pipeline_blob_chunk *chunk;
    uint32_t checksum;

    /* Avoid E_INVALIDARG with an invalid header size, since that may confuse some games */
    if (state->blob.CachedBlobSizeInBytes < sizeof(*blob) || blob->version != VKD3D_CACHE_BLOB_VERSION)
        return D3D12_ERROR_DRIVER_VERSION_MISMATCH;

    /* Indicate that the cached data is not useful if we're running on a different device or driver */
    if (blob->vendor_id != device_properties->vendorID || blob->device_id != device_properties->deviceID)
        return D3D12_ERROR_ADAPTER_NOT_FOUND;

    /* Check the vkd3d-proton build since the shader compiler itself may change,
     * and the driver since that will affect the generated pipeline cache.
     * Based on global configuration flags, which extensions are available, etc,
     * the generated shaders may also change, so key on that as well. */
    if (blob->vkd3d_build != vkd3d_build ||
            blob->vkd3d_shader_interface_key != device->shader_interface_key ||
            memcmp(blob->cache_uuid, device_properties->pipelineCacheUUID, VK_UUID_SIZE) != 0)
        return D3D12_ERROR_DRIVER_VERSION_MISMATCH;

    checksum = vkd3d_pipeline_blob_compute_data_checksum(blob->data,
            state->blob.CachedBlobSizeInBytes - offsetof(struct vkd3d_pipeline_blob, data));

    if (checksum != blob->checksum)
    {
        ERR("Corrupt PSO cache blob entry found!\n");
        /* Same rationale as above, avoid E_INVALIDARG, since that may confuse some games */
        return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
    }

    /* Fetch compat info. */
    chunk = find_blob_chunk((const struct vkd3d_pipeline_blob_chunk *)blob->data,
            state->blob.CachedBlobSizeInBytes - offsetof(struct vkd3d_pipeline_blob, data),
            VKD3D_PIPELINE_BLOB_CHUNK_TYPE_PSO_COMPAT);
    if (!chunk || chunk->size != sizeof(root_signature_compat_hash))
        return E_FAIL;

    /* Verify the expected root signature that was used to generate the SPIR-V. */
    if (memcmp(&root_signature_compat_hash, chunk->data, chunk->size) != 0)
    {
        WARN("Root signature compatibility hash mismatch.\n");
        return E_INVALIDARG;
    }

    return S_OK;
}

static struct vkd3d_pipeline_blob_chunk *finish_and_iterate_blob_chunk(struct vkd3d_pipeline_blob_chunk *chunk)
{
    uint32_t aligned_size = align(chunk->size, VKD3D_PIPELINE_BLOB_CHUNK_ALIGN);
    /* Ensure we get stable hashes if we need to pad. */
    memset(&chunk->data[chunk->size], 0, aligned_size - chunk->size);
    return (struct vkd3d_pipeline_blob_chunk *)&chunk->data[aligned_size];
}

HRESULT vkd3d_create_pipeline_cache_from_d3d12_desc(struct d3d12_device *device,
        const struct d3d12_cached_pipeline_state *state, VkPipelineCache *cache)
{
    const struct vkd3d_pipeline_blob *blob = state->blob.pCachedBlob;
    const struct vkd3d_pipeline_blob_chunk *chunk;
    VkResult vr;

    if (!state->blob.CachedBlobSizeInBytes)
    {
        vr = vkd3d_create_pipeline_cache(device, 0, NULL, cache);
        return hresult_from_vk_result(vr);
    }

    chunk = find_blob_chunk((const struct vkd3d_pipeline_blob_chunk *)blob->data,
            state->blob.CachedBlobSizeInBytes - offsetof(struct vkd3d_pipeline_blob, data),
            VKD3D_PIPELINE_BLOB_CHUNK_TYPE_PIPELINE_CACHE);

    if (!chunk)
    {
        vr = vkd3d_create_pipeline_cache(device, 0, NULL, cache);
        return hresult_from_vk_result(vr);
    }

    vr = vkd3d_create_pipeline_cache(device, chunk->size, chunk->data, cache);
    return hresult_from_vk_result(vr);
}

HRESULT vkd3d_get_cached_spirv_code_from_d3d12_desc(
        const D3D12_SHADER_BYTECODE *code, const struct d3d12_cached_pipeline_state *state,
        VkShaderStageFlagBits stage,
        struct vkd3d_shader_code *spirv_code)
{
    const struct vkd3d_shader_code dxbc = { code->pShaderBytecode, code->BytecodeLength };
    const struct vkd3d_pipeline_blob *blob = state->blob.pCachedBlob;
    const struct vkd3d_pipeline_blob_chunk_shader_meta *meta;
    const struct vkd3d_pipeline_blob_chunk_spirv *spirv;
    const struct vkd3d_pipeline_blob_chunk *chunk;
    vkd3d_shader_hash_t dxbc_hash;
    void *duped_code;

    if (!state->blob.CachedBlobSizeInBytes)
        return E_FAIL;

    /* Fetch shader meta. */
    chunk = find_blob_chunk((const struct vkd3d_pipeline_blob_chunk *)blob->data,
            state->blob.CachedBlobSizeInBytes - offsetof(struct vkd3d_pipeline_blob, data),
            VKD3D_PIPELINE_BLOB_CHUNK_TYPE_SHADER_META | (stage << VKD3D_PIPELINE_BLOB_CHUNK_INDEX_SHIFT));
    if (!chunk || chunk->size != sizeof(*meta))
        return E_FAIL;
    meta = (const struct vkd3d_pipeline_blob_chunk_shader_meta*)chunk->data;
    memcpy(&spirv_code->meta, &meta->meta, sizeof(meta->meta));

    /* Verify that DXBC blob hash matches with what we expect. */
    dxbc_hash = vkd3d_shader_hash(&dxbc);
    if (dxbc_hash != spirv_code->meta.hash)
    {
        WARN("DXBC blob hash in CreatePSO state (%016"PRIx64") does not match expected hash (%016"PRIx64".\n",
                dxbc_hash, spirv_code->meta.hash);
        return E_INVALIDARG;
    }

    /* Aim to pull SPIR-V either from inlined chunk, or a link. */
    chunk = find_blob_chunk((const struct vkd3d_pipeline_blob_chunk *)blob->data,
            state->blob.CachedBlobSizeInBytes - offsetof(struct vkd3d_pipeline_blob, data),
            VKD3D_PIPELINE_BLOB_CHUNK_TYPE_VARINT_SPIRV | (stage << VKD3D_PIPELINE_BLOB_CHUNK_INDEX_SHIFT));

    if (!chunk)
        return E_FAIL;

    spirv = (const struct vkd3d_pipeline_blob_chunk_spirv *)chunk->data;

    duped_code = vkd3d_malloc(spirv->decompressed_spirv_size);
    if (!duped_code)
        return E_OUTOFMEMORY;

    if (!vkd3d_decode_varint(duped_code,
            spirv->decompressed_spirv_size / sizeof(uint32_t),
            spirv->data, spirv->compressed_spirv_size))
    {
        FIXME("Failed to decode VARINT.\n");
        vkd3d_free(duped_code);
        return E_INVALIDARG;
    }

    spirv_code->code = duped_code;
    spirv_code->size = spirv->decompressed_spirv_size;

    return S_OK;
}

static size_t vkd3d_shader_code_compute_serialized_size(const struct vkd3d_shader_code *code,
        size_t *out_varint_size, bool inline_spirv)
{
    size_t blob_size = 0;
    size_t varint_size = 0;

    if (code->size && !(code->meta.flags & VKD3D_SHADER_META_FLAG_REPLACED))
    {
        if (out_varint_size || inline_spirv)
            varint_size = vkd3d_compute_size_varint(code->code, code->size / sizeof(uint32_t));

        /* If we have a pipeline library, we will store a reference to the SPIR-V instead. */
        if (inline_spirv)
        {
            blob_size += align(sizeof(struct vkd3d_pipeline_blob_chunk) +
                    sizeof(struct vkd3d_pipeline_blob_chunk_spirv) + varint_size,
                    VKD3D_PIPELINE_BLOB_CHUNK_ALIGN);
        }
        else
        {
            blob_size += align(sizeof(struct vkd3d_pipeline_blob_chunk) +
                    sizeof(struct vkd3d_pipeline_blob_chunk_link),
                    VKD3D_PIPELINE_BLOB_CHUNK_ALIGN);
        }

        blob_size += align(sizeof(struct vkd3d_pipeline_blob_chunk) +
                sizeof(struct vkd3d_pipeline_blob_chunk_shader_meta),
                VKD3D_PIPELINE_BLOB_CHUNK_ALIGN);
    }

    if (out_varint_size)
        *out_varint_size = varint_size;
    return blob_size;
}

VkResult vkd3d_serialize_pipeline_state(struct d3d12_pipeline_library *pipeline_library,
        const struct d3d12_pipeline_state *state, size_t *size, void *data)
{
    const VkPhysicalDeviceProperties *device_properties = &state->device->device_info.properties2.properties;
    const struct vkd3d_vk_device_procs *vk_procs = &state->device->vk_procs;
    struct vkd3d_pipeline_blob_chunk_pso_compat *pso_compat;
    struct vkd3d_pipeline_blob_chunk_shader_meta *meta;
    struct vkd3d_pipeline_blob_chunk_spirv *spirv;
    size_t varint_size[VKD3D_MAX_SHADER_STAGES];
    struct vkd3d_pipeline_blob *blob = data;
    struct vkd3d_pipeline_blob_chunk *chunk;
    size_t vk_blob_size_pipeline_cache = 0;
    size_t total_size = sizeof(*blob);
    size_t vk_blob_size = 0;
    bool need_blob_sizes;
    unsigned int i;
    VkResult vr;

    need_blob_sizes = !pipeline_library || data;

    /* PSO compatibility information is global to a PSO. */
    vk_blob_size += align(sizeof(struct vkd3d_pipeline_blob_chunk) +
            sizeof(struct vkd3d_pipeline_blob_chunk_pso_compat),
            VKD3D_PIPELINE_BLOB_CHUNK_ALIGN);

    if (state->vk_pso_cache)
    {
        if (need_blob_sizes)
        {
            if ((vr = VK_CALL(vkGetPipelineCacheData(state->device->vk_device, state->vk_pso_cache, &vk_blob_size_pipeline_cache, NULL))))
            {
                ERR("Failed to retrieve pipeline cache size, vr %d.\n", vr);
                return vr;
            }
        }

        if (pipeline_library)
        {
            vk_blob_size += align(sizeof(struct vkd3d_pipeline_blob_chunk) +
                    sizeof(struct vkd3d_pipeline_blob_chunk_link),
                    VKD3D_PIPELINE_BLOB_CHUNK_ALIGN);
        }
        else
        {
            vk_blob_size += align(vk_blob_size_pipeline_cache +
                    sizeof(struct vkd3d_pipeline_blob_chunk),
                    VKD3D_PIPELINE_BLOB_CHUNK_ALIGN);
        }
    }

    if (d3d12_pipeline_state_is_graphics(state))
    {
        for (i = 0; i < state->graphics.stage_count; i++)
        {
            vk_blob_size += vkd3d_shader_code_compute_serialized_size(&state->graphics.code[i],
                    need_blob_sizes ? &varint_size[i] : NULL, !pipeline_library);
        }
    }
    else if (d3d12_pipeline_state_is_compute(state))
    {
        vk_blob_size += vkd3d_shader_code_compute_serialized_size(&state->compute.code,
                need_blob_sizes ? &varint_size[0] : NULL, !pipeline_library);
    }

    total_size += vk_blob_size;

    if (blob && *size < total_size)
        return VK_INCOMPLETE;

    if (blob)
    {
        blob->version = VKD3D_CACHE_BLOB_VERSION;
        blob->vendor_id = device_properties->vendorID;
        blob->device_id = device_properties->deviceID;
        blob->vkd3d_shader_interface_key = state->device->shader_interface_key;
        blob->vkd3d_build = vkd3d_build;
        memcpy(blob->cache_uuid, device_properties->pipelineCacheUUID, VK_UUID_SIZE);
        chunk = (struct vkd3d_pipeline_blob_chunk *)blob->data;

        chunk->type = VKD3D_PIPELINE_BLOB_CHUNK_TYPE_PSO_COMPAT;
        chunk->size = sizeof(struct vkd3d_pipeline_blob_chunk_pso_compat);
        pso_compat = (struct vkd3d_pipeline_blob_chunk_pso_compat*)chunk->data;
        pso_compat->root_signature_compat_hash = state->root_signature_compat_hash;
        chunk = finish_and_iterate_blob_chunk(chunk);

        if (state->vk_pso_cache)
        {
            /* Store PSO cache, or link to it if using pipeline cache. */
            chunk->type = VKD3D_PIPELINE_BLOB_CHUNK_TYPE_PIPELINE_CACHE;
            chunk->size = vk_blob_size_pipeline_cache;
            if ((vr = VK_CALL(vkGetPipelineCacheData(state->device->vk_device, state->vk_pso_cache, &vk_blob_size_pipeline_cache, chunk->data))))
                return vr;
            chunk = finish_and_iterate_blob_chunk(chunk);
        }

        if (d3d12_pipeline_state_is_graphics(state))
        {
            for (i = 0; i < state->graphics.stage_count; i++)
            {
                if (state->graphics.code[i].size &&
                        !(state->graphics.code[i].meta.flags & VKD3D_SHADER_META_FLAG_REPLACED))
                {
                    /* Store inline SPIR-V, or (TODO) a link to SPIR-V in pipeline library. */
                    chunk->type = VKD3D_PIPELINE_BLOB_CHUNK_TYPE_VARINT_SPIRV |
                            (state->graphics.stages[i].stage << VKD3D_PIPELINE_BLOB_CHUNK_INDEX_SHIFT);
                    chunk->size = sizeof(struct vkd3d_pipeline_blob_chunk_spirv) + varint_size[i];

                    spirv = (struct vkd3d_pipeline_blob_chunk_spirv *)chunk->data;
                    spirv->compressed_spirv_size = varint_size[i];
                    spirv->decompressed_spirv_size = state->graphics.code[i].size;

                    vkd3d_encode_varint(spirv->data, state->graphics.code[i].code,
                            state->graphics.code[i].size / sizeof(uint32_t));
                    chunk = finish_and_iterate_blob_chunk(chunk);

                    /* Store meta information for SPIR-V. */
                    chunk->type = VKD3D_PIPELINE_BLOB_CHUNK_TYPE_SHADER_META |
                            (state->graphics.stages[i].stage << VKD3D_PIPELINE_BLOB_CHUNK_INDEX_SHIFT);
                    chunk->size = sizeof(struct vkd3d_pipeline_blob_chunk_shader_meta);
                    meta = (struct vkd3d_pipeline_blob_chunk_shader_meta*)chunk->data;
                    meta->meta = state->graphics.code[i].meta;
                    chunk = finish_and_iterate_blob_chunk(chunk);
                }
            }
        }
        else if (d3d12_pipeline_state_is_compute(state))
        {
            if (state->compute.code.size &&
                    !(state->compute.code.meta.flags & VKD3D_SHADER_META_FLAG_REPLACED))
            {
                /* Store inline SPIR-V, or (TODO) a link to SPIR-V in pipeline library. */
                chunk->type = VKD3D_PIPELINE_BLOB_CHUNK_TYPE_VARINT_SPIRV |
                        (VK_SHADER_STAGE_COMPUTE_BIT << VKD3D_PIPELINE_BLOB_CHUNK_INDEX_SHIFT);
                chunk->size = sizeof(struct vkd3d_pipeline_blob_chunk_spirv) + varint_size[0];

                spirv = (struct vkd3d_pipeline_blob_chunk_spirv *)chunk->data;
                spirv->compressed_spirv_size = varint_size[0];
                spirv->decompressed_spirv_size = state->compute.code.size;

                vkd3d_encode_varint(spirv->data, state->compute.code.code,
                        state->compute.code.size / sizeof(uint32_t));
                chunk = finish_and_iterate_blob_chunk(chunk);

                /* Store meta information for SPIR-V. */
                chunk->type = VKD3D_PIPELINE_BLOB_CHUNK_TYPE_SHADER_META |
                        (VK_SHADER_STAGE_COMPUTE_BIT << VKD3D_PIPELINE_BLOB_CHUNK_INDEX_SHIFT);
                chunk->size = sizeof(struct vkd3d_pipeline_blob_chunk_shader_meta);
                meta = (struct vkd3d_pipeline_blob_chunk_shader_meta*)chunk->data;
                meta->meta = state->compute.code.meta;
                chunk = finish_and_iterate_blob_chunk(chunk);
            }
        }

        blob->checksum = vkd3d_pipeline_blob_compute_data_checksum(blob->data, vk_blob_size);
    }

    *size = total_size;
    return VK_SUCCESS;
}

struct vkd3d_cached_pipeline_key
{
    size_t name_length;
    const void *name;
    uint64_t internal_key_hash; /* Used for internal keys which are just hashes. Used if name_length is 0. */
};

struct vkd3d_cached_pipeline_data
{
    size_t blob_length;
    const void *blob;
    bool is_new;
};

struct vkd3d_cached_pipeline_entry
{
    struct hash_map_entry entry;
    struct vkd3d_cached_pipeline_key key;
    struct vkd3d_cached_pipeline_data data;
};

static uint32_t vkd3d_cached_pipeline_hash_name(const void *key)
{
    const struct vkd3d_cached_pipeline_key *k = key;
    uint32_t hash = 0;
    size_t i;

    for (i = 0; i < k->name_length; i += 4)
    {
        uint32_t accum = 0;
        memcpy(&accum, (const char*)k->name + i,
                min(k->name_length - i, sizeof(accum)));
        hash = hash_combine(hash, accum);
    }

    return hash;
}

static uint32_t vkd3d_cached_pipeline_hash_internal(const void *key)
{
    const struct vkd3d_cached_pipeline_key *k = key;
    return hash_uint64(k->internal_key_hash);
}

static bool vkd3d_cached_pipeline_compare_name(const void *key, const struct hash_map_entry *entry)
{
    const struct vkd3d_cached_pipeline_entry *e = (const struct vkd3d_cached_pipeline_entry*)entry;
    const struct vkd3d_cached_pipeline_key *k = key;

    return k->name_length == e->key.name_length &&
            !memcmp(k->name, e->key.name, k->name_length);
}

static bool vkd3d_cached_pipeline_compare_internal(const void *key, const struct hash_map_entry *entry)
{
    const struct vkd3d_cached_pipeline_entry *e = (const struct vkd3d_cached_pipeline_entry*)entry;
    const struct vkd3d_cached_pipeline_key *k = key;
    return k->internal_key_hash == e->key.internal_key_hash;
}

struct vkd3d_serialized_pipeline_toc_entry
{
    uint64_t blob_offset;
    uint32_t name_length;
    uint32_t blob_length;
};
STATIC_ASSERT(sizeof(struct vkd3d_serialized_pipeline_toc_entry) == 16);

#define VKD3D_PIPELINE_LIBRARY_VERSION MAKE_MAGIC('V','K','L',3)

struct vkd3d_serialized_pipeline_library
{
    uint32_t version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t spirv_count;
    uint32_t driver_cache_count;
    uint32_t pipeline_count;
    uint64_t vkd3d_build;
    uint64_t vkd3d_shader_interface_key;
    uint8_t cache_uuid[VK_UUID_SIZE];
    struct vkd3d_serialized_pipeline_toc_entry entries[];
};
/* After entries, name buffers are encoded tightly packed one after the other.
 * For blob data, these are referenced by blob_offset / blob_length.
 * blob_offset is aligned. */

/* Rationale for this split format is:
 * - It is implied that the pipeline library can be used directly from an mmap-ed on-disk file,
 *   since users cannot free the pointer to library once created.
 *   In this situation, we should scan through just the TOC to begin with to avoid page faulting on potentially 100s of MBs.
 *   It is also more cache friendly this way.
 * - Having a more split TOC structure like this will make it easier to add SPIR-V deduplication down the line.
 */

STATIC_ASSERT(sizeof(struct vkd3d_serialized_pipeline_library) == offsetof(struct vkd3d_serialized_pipeline_library, entries));
STATIC_ASSERT(sizeof(struct vkd3d_serialized_pipeline_library) == 40 + VK_UUID_SIZE);

/* ID3D12PipelineLibrary */
static inline struct d3d12_pipeline_library *impl_from_ID3D12PipelineLibrary(d3d12_pipeline_library_iface *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_pipeline_library, ID3D12PipelineLibrary_iface);
}

static void d3d12_pipeline_library_serialize_entry(
        const struct vkd3d_cached_pipeline_entry *entry,
        struct vkd3d_serialized_pipeline_toc_entry *header,
        uint8_t *data, size_t name_offset, size_t blob_offset)
{
    header->blob_offset = blob_offset;
    header->name_length = entry->key.name_length;
    header->blob_length = entry->data.blob_length;

    if (entry->key.name_length)
        memcpy(data + name_offset, entry->key.name, entry->key.name_length);
    else
        memcpy(data + name_offset, &entry->key.internal_key_hash, sizeof(entry->key.internal_key_hash));

    memcpy(data + blob_offset, entry->data.blob, entry->data.blob_length);
}

static void d3d12_pipeline_library_cleanup_map(struct hash_map *map)
{
    size_t i;

    for (i = 0; i < map->entry_count; i++)
    {
        struct vkd3d_cached_pipeline_entry *e = (struct vkd3d_cached_pipeline_entry*)hash_map_get_entry(map, i);

        if ((e->entry.flags & HASH_MAP_ENTRY_OCCUPIED) && e->data.is_new)
        {
            vkd3d_free((void*)e->key.name);
            vkd3d_free((void*)e->data.blob);
        }
    }

    hash_map_clear(map);
}

static void d3d12_pipeline_library_cleanup(struct d3d12_pipeline_library *pipeline_library, struct d3d12_device *device)
{
    d3d12_pipeline_library_cleanup_map(&pipeline_library->pso_map);
    d3d12_pipeline_library_cleanup_map(&pipeline_library->driver_cache_map);
    d3d12_pipeline_library_cleanup_map(&pipeline_library->spirv_cache_map);

    vkd3d_private_store_destroy(&pipeline_library->private_store);
    rwlock_destroy(&pipeline_library->mutex);
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_QueryInterface(d3d12_pipeline_library_iface *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12PipelineLibrary)
            || IsEqualGUID(riid, &IID_ID3D12PipelineLibrary1)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12PipelineLibrary_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_pipeline_library_AddRef(d3d12_pipeline_library_iface *iface)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);
    ULONG refcount = InterlockedIncrement(&pipeline_library->refcount);

    TRACE("%p increasing refcount to %u.\n", pipeline_library, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_pipeline_library_Release(d3d12_pipeline_library_iface *iface)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);
    ULONG refcount = InterlockedDecrement(&pipeline_library->refcount);

    TRACE("%p decreasing refcount to %u.\n", pipeline_library, refcount);

    if (!refcount)
    {
        d3d12_pipeline_library_cleanup(pipeline_library, pipeline_library->device);
        d3d12_device_release(pipeline_library->device);
        vkd3d_free(pipeline_library);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_GetPrivateData(d3d12_pipeline_library_iface *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&pipeline_library->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_SetPrivateData(d3d12_pipeline_library_iface *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&pipeline_library->private_store, guid, data_size, data,
            NULL, NULL);
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_SetPrivateDataInterface(d3d12_pipeline_library_iface *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&pipeline_library->private_store, guid, data,
            NULL, NULL);
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_GetDevice(d3d12_pipeline_library_iface *iface,
        REFIID iid, void **device)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return d3d12_device_query_interface(pipeline_library->device, iid, device);
}

static uint32_t d3d12_cached_pipeline_entry_name_table_size(const struct vkd3d_cached_pipeline_entry *entry)
{
    if (entry->key.name_length)
        return entry->key.name_length;
    else
        return sizeof(entry->key.internal_key_hash);
}

static bool d3d12_pipeline_library_insert_hash_map_blob(struct d3d12_pipeline_library *pipeline_library,
        struct hash_map *map, const struct vkd3d_cached_pipeline_entry *entry)
{
    const struct vkd3d_cached_pipeline_entry *new_entry;
    if ((new_entry = (const struct vkd3d_cached_pipeline_entry*)hash_map_insert(map, &entry->key, &entry->entry)) &&
            memcmp(&new_entry->data, &entry->data, sizeof(entry->data)) == 0)
    {
        pipeline_library->total_name_table_size += d3d12_cached_pipeline_entry_name_table_size(entry);
        pipeline_library->total_blob_size += align(entry->data.blob_length, VKD3D_PIPELINE_BLOB_ALIGN);
        return true;
    }
    else
        return false;
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_StorePipeline(d3d12_pipeline_library_iface *iface,
        LPCWSTR name, ID3D12PipelineState *pipeline)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);
    struct d3d12_pipeline_state *pipeline_state = impl_from_ID3D12PipelineState(pipeline);
    struct vkd3d_cached_pipeline_entry entry;
    void *new_name, *new_blob;
    VkResult vr;
    int rc;

    TRACE("iface %p, name %s, pipeline %p.\n", iface, debugstr_w(name), pipeline);

    if ((rc = rwlock_lock_write(&pipeline_library->mutex)))
    {
        ERR("Failed to lock mutex, rc %d.\n", rc);
        return hresult_from_errno(rc);
    }

    entry.key.name_length = vkd3d_wcslen(name) * sizeof(WCHAR);
    entry.key.name = name;
    entry.key.internal_key_hash = 0;

    if (hash_map_find(&pipeline_library->pso_map, &entry.key))
    {
        WARN("Pipeline %s already exists.\n", debugstr_w(name));
        rwlock_unlock_write(&pipeline_library->mutex);
        return E_INVALIDARG;
    }

    /* We need to allocate persistent storage for the name */
    if (!(new_name = malloc(entry.key.name_length)))
    {
        rwlock_unlock_write(&pipeline_library->mutex);
        return E_OUTOFMEMORY;
    }

    memcpy(new_name, name, entry.key.name_length);
    entry.key.name = new_name;

    if (FAILED(vr = vkd3d_serialize_pipeline_state(NULL /* TODO */, pipeline_state, &entry.data.blob_length, NULL)))
    {
        vkd3d_free(new_name);
        rwlock_unlock_write(&pipeline_library->mutex);
        return hresult_from_vk_result(vr);
    }

    if (!(new_blob = malloc(entry.data.blob_length)))
    {
        vkd3d_free(new_name);
        rwlock_unlock_write(&pipeline_library->mutex);
        return E_OUTOFMEMORY;
    }

    if (FAILED(vr = vkd3d_serialize_pipeline_state(NULL /* TODO */, pipeline_state, &entry.data.blob_length, new_blob)))
    {
        vkd3d_free(new_name);
        vkd3d_free(new_blob);
        rwlock_unlock_write(&pipeline_library->mutex);
        return hresult_from_vk_result(vr);
    }

    entry.data.blob = new_blob;
    entry.data.is_new = true;

    if (!d3d12_pipeline_library_insert_hash_map_blob(pipeline_library, &pipeline_library->pso_map, &entry))
    {
        vkd3d_free(new_name);
        vkd3d_free(new_blob);
        rwlock_unlock_write(&pipeline_library->mutex);
        return E_OUTOFMEMORY;
    }

    rwlock_unlock_write(&pipeline_library->mutex);
    return S_OK;
}

static HRESULT d3d12_pipeline_library_load_pipeline(struct d3d12_pipeline_library *pipeline_library, LPCWSTR name,
        VkPipelineBindPoint bind_point, struct d3d12_pipeline_state_desc *desc, struct d3d12_pipeline_state **state)
{
    const struct vkd3d_cached_pipeline_entry *e;
    struct vkd3d_cached_pipeline_key key;
    int rc;

    if ((rc = rwlock_lock_read(&pipeline_library->mutex)))
    {
        ERR("Failed to lock mutex, rc %d.\n", rc);
        return hresult_from_errno(rc);
    }

    key.name_length = vkd3d_wcslen(name) * sizeof(WCHAR);
    key.name = name;

    if (!(e = (const struct vkd3d_cached_pipeline_entry*)hash_map_find(&pipeline_library->pso_map, &key)))
    {
        WARN("Pipeline %s does not exist.\n", debugstr_w(name));
        rwlock_unlock_read(&pipeline_library->mutex);
        return E_INVALIDARG;
    }

    desc->cached_pso.blob.CachedBlobSizeInBytes = e->data.blob_length;
    desc->cached_pso.blob.pCachedBlob = e->data.blob;
    desc->cached_pso.library = pipeline_library;
    rwlock_unlock_read(&pipeline_library->mutex);

    return d3d12_pipeline_state_create(pipeline_library->device, bind_point, desc, state);
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_LoadGraphicsPipeline(d3d12_pipeline_library_iface *iface,
        LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID iid, void **pipeline_state)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);
    struct d3d12_pipeline_state_desc pipeline_desc;
    struct d3d12_pipeline_state *object;
    HRESULT hr;

    TRACE("iface %p, name %s, desc %p, iid %s, pipeline_state %p.\n", iface,
            debugstr_w(name), desc, debugstr_guid(iid), pipeline_state);

    if (FAILED(hr = vkd3d_pipeline_state_desc_from_d3d12_graphics_desc(&pipeline_desc, desc)))
        return hr;

    if (FAILED(hr = d3d12_pipeline_library_load_pipeline(pipeline_library,
            name, VK_PIPELINE_BIND_POINT_GRAPHICS, &pipeline_desc, &object)))
        return hr;

    return return_interface(&object->ID3D12PipelineState_iface,
            &IID_ID3D12PipelineState, iid, pipeline_state);
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_LoadComputePipeline(d3d12_pipeline_library_iface *iface,
        LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID iid, void **pipeline_state)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);
    struct d3d12_pipeline_state_desc pipeline_desc;
    struct d3d12_pipeline_state *object;
    HRESULT hr;

    TRACE("iface %p, name %s, desc %p, iid %s, pipeline_state %p.\n", iface,
            debugstr_w(name), desc, debugstr_guid(iid), pipeline_state);

    if (FAILED(hr = vkd3d_pipeline_state_desc_from_d3d12_compute_desc(&pipeline_desc, desc)))
        return hr;

    if (FAILED(hr = d3d12_pipeline_library_load_pipeline(pipeline_library,
            name, VK_PIPELINE_BIND_POINT_COMPUTE, &pipeline_desc, &object)))
        return hr;

    return return_interface(&object->ID3D12PipelineState_iface,
            &IID_ID3D12PipelineState, iid, pipeline_state);
}

static size_t d3d12_pipeline_library_get_aligned_name_table_size(struct d3d12_pipeline_library *pipeline_library)
{
    return align(pipeline_library->total_name_table_size, VKD3D_PIPELINE_BLOB_ALIGN);
}

static size_t d3d12_pipeline_library_get_serialized_size(struct d3d12_pipeline_library *pipeline_library)
{
    size_t total_size = 0;

    total_size += sizeof(struct vkd3d_serialized_pipeline_library);
    total_size += sizeof(struct vkd3d_serialized_pipeline_toc_entry) * pipeline_library->pso_map.used_count;
    total_size += sizeof(struct vkd3d_serialized_pipeline_toc_entry) * pipeline_library->spirv_cache_map.used_count;
    total_size += sizeof(struct vkd3d_serialized_pipeline_toc_entry) * pipeline_library->driver_cache_map.used_count;
    total_size += d3d12_pipeline_library_get_aligned_name_table_size(pipeline_library);
    total_size += pipeline_library->total_blob_size;

    return total_size;
}

static SIZE_T STDMETHODCALLTYPE d3d12_pipeline_library_GetSerializedSize(d3d12_pipeline_library_iface *iface)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);
    size_t total_size;
    int rc;

    TRACE("iface %p.\n", iface);

    if ((rc = rwlock_lock_read(&pipeline_library->mutex)))
    {
        ERR("Failed to lock mutex, rc %d.\n", rc);
        return 0;
    }

    total_size = d3d12_pipeline_library_get_serialized_size(pipeline_library);

    rwlock_unlock_read(&pipeline_library->mutex);
    return total_size;
}

static void d3d12_pipeline_library_serialize_hash_map(const struct hash_map *map,
        struct vkd3d_serialized_pipeline_toc_entry **inout_toc_entries, uint8_t *serialized_data,
        size_t *inout_name_offset, size_t *inout_blob_offset)
{
    struct vkd3d_serialized_pipeline_toc_entry *toc_entries = *inout_toc_entries;
    size_t name_offset = *inout_name_offset;
    size_t blob_offset = *inout_blob_offset;
    uint32_t i;

    for (i = 0; i < map->entry_count; i++)
    {
        struct vkd3d_cached_pipeline_entry *e = (struct vkd3d_cached_pipeline_entry*)hash_map_get_entry(map, i);

        if (e->entry.flags & HASH_MAP_ENTRY_OCCUPIED)
        {
            d3d12_pipeline_library_serialize_entry(e, toc_entries, serialized_data, name_offset, blob_offset);
            toc_entries++;
            name_offset += e->key.name_length ? e->key.name_length : sizeof(e->key.internal_key_hash);
            blob_offset += align(e->data.blob_length, VKD3D_PIPELINE_BLOB_ALIGN);
        }
    }

    *inout_toc_entries = toc_entries;
    *inout_name_offset = name_offset;
    *inout_blob_offset = blob_offset;
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_Serialize(d3d12_pipeline_library_iface *iface,
        void *data, SIZE_T data_size)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);
    const VkPhysicalDeviceProperties *device_properties = &pipeline_library->device->device_info.properties2.properties;
    struct vkd3d_serialized_pipeline_library *header = data;
    struct vkd3d_serialized_pipeline_toc_entry *toc_entries;
    uint8_t *serialized_data;
    size_t total_toc_entries;
    size_t required_size;
    size_t name_offset;
    size_t blob_offset;
    int rc;

    TRACE("iface %p.\n", iface);

    if ((rc = rwlock_lock_read(&pipeline_library->mutex)))
    {
        ERR("Failed to lock mutex, rc %d.\n", rc);
        return 0;
    }

    required_size = d3d12_pipeline_library_get_serialized_size(pipeline_library);
    if (data_size < required_size)
    {
        rwlock_unlock_read(&pipeline_library->mutex);
        return E_INVALIDARG;
    }

    header->version = VKD3D_PIPELINE_LIBRARY_VERSION;
    header->vendor_id = device_properties->vendorID;
    header->device_id = device_properties->deviceID;
    header->pipeline_count = pipeline_library->pso_map.used_count;
    header->spirv_count = pipeline_library->spirv_cache_map.used_count;
    header->driver_cache_count = pipeline_library->driver_cache_map.used_count;
    header->vkd3d_build = vkd3d_build;
    header->vkd3d_shader_interface_key = pipeline_library->device->shader_interface_key;
    memcpy(header->cache_uuid, device_properties->pipelineCacheUUID, VK_UUID_SIZE);

    total_toc_entries = header->pipeline_count + header->spirv_count + header->driver_cache_count;

    toc_entries = header->entries;
    serialized_data = (uint8_t *)&toc_entries[total_toc_entries];
    name_offset = 0;
    blob_offset = d3d12_pipeline_library_get_aligned_name_table_size(pipeline_library);

    d3d12_pipeline_library_serialize_hash_map(&pipeline_library->spirv_cache_map, &toc_entries,
            serialized_data, &name_offset, &blob_offset);
    d3d12_pipeline_library_serialize_hash_map(&pipeline_library->driver_cache_map, &toc_entries,
            serialized_data, &name_offset, &blob_offset);
    d3d12_pipeline_library_serialize_hash_map(&pipeline_library->pso_map, &toc_entries,
            serialized_data, &name_offset, &blob_offset);

    rwlock_unlock_read(&pipeline_library->mutex);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_pipeline_library_LoadPipeline(d3d12_pipeline_library_iface *iface,
        LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID iid, void **pipeline_state)
{
    struct d3d12_pipeline_library *pipeline_library = impl_from_ID3D12PipelineLibrary(iface);
    struct d3d12_pipeline_state_desc pipeline_desc;
    struct d3d12_pipeline_state *object;
    VkPipelineBindPoint pipeline_type;
    HRESULT hr;

    TRACE("iface %p, name %s, desc %p, iid %s, pipeline_state %p.\n", iface,
            debugstr_w(name), desc, debugstr_guid(iid), pipeline_state);

    if (FAILED(hr = vkd3d_pipeline_state_desc_from_d3d12_stream_desc(&pipeline_desc, desc, &pipeline_type)))
        return hr;

    if (FAILED(hr = d3d12_pipeline_library_load_pipeline(pipeline_library,
            name, pipeline_type, &pipeline_desc, &object)))
        return hr;

    return return_interface(&object->ID3D12PipelineState_iface,
            &IID_ID3D12PipelineState, iid, pipeline_state);
}

static CONST_VTBL struct ID3D12PipelineLibrary1Vtbl d3d12_pipeline_library_vtbl =
{
    /* IUnknown methods */
    d3d12_pipeline_library_QueryInterface,
    d3d12_pipeline_library_AddRef,
    d3d12_pipeline_library_Release,
    /* ID3D12Object methods */
    d3d12_pipeline_library_GetPrivateData,
    d3d12_pipeline_library_SetPrivateData,
    d3d12_pipeline_library_SetPrivateDataInterface,
    (void *)d3d12_object_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_pipeline_library_GetDevice,
    /* ID3D12PipelineLibrary methods */
    d3d12_pipeline_library_StorePipeline,
    d3d12_pipeline_library_LoadGraphicsPipeline,
    d3d12_pipeline_library_LoadComputePipeline,
    d3d12_pipeline_library_GetSerializedSize,
    d3d12_pipeline_library_Serialize,
    /* ID3D12PipelineLibrary1 methods */
    d3d12_pipeline_library_LoadPipeline,
};

static HRESULT d3d12_pipeline_library_unserialize_hash_map(
        struct d3d12_pipeline_library *pipeline_library,
        const struct vkd3d_serialized_pipeline_toc_entry *entries,
        size_t entries_count, struct hash_map *map,
        const uint8_t *serialized_data_base, size_t serialized_data_size,
        const uint8_t **inout_name_table)
{
    const uint8_t *name_table = *inout_name_table;
    uint32_t i;

    /* The application is not allowed to free the blob, so we
     * can safely use pointers without copying the data first. */
    for (i = 0; i < entries_count; i++)
    {
        const struct vkd3d_serialized_pipeline_toc_entry *toc_entry = &entries[i];
        struct vkd3d_cached_pipeline_entry entry;

        entry.key.name_length = toc_entry->name_length;

        if (entry.key.name_length)
        {
            entry.key.name = name_table;
            entry.key.internal_key_hash = 0;
            /* Verify that name table entry does not overflow. */
            if (name_table + entry.key.name_length > serialized_data_base + serialized_data_size)
                return E_INVALIDARG;
            name_table += entry.key.name_length;
        }
        else
        {
            entry.key.name = NULL;
            entry.key.internal_key_hash = 0;
            /* Verify that name table entry does not overflow. */
            if (name_table + sizeof(entry.key.internal_key_hash) > serialized_data_base + serialized_data_size)
                return E_INVALIDARG;
            memcpy(&entry.key.internal_key_hash, name_table, sizeof(entry.key.internal_key_hash));
            name_table += sizeof(entry.key.internal_key_hash);
        }

        /* Verify that blob entry does not overflow. */
        if (toc_entry->blob_offset + toc_entry->blob_length > serialized_data_size)
            return E_INVALIDARG;

        entry.data.blob_length = toc_entry->blob_length;
        entry.data.blob = serialized_data_base + toc_entry->blob_offset;
        entry.data.is_new = false;

        if (!d3d12_pipeline_library_insert_hash_map_blob(pipeline_library, map, &entry))
            return E_OUTOFMEMORY;
    }

    *inout_name_table = name_table;
    return S_OK;
}

static HRESULT d3d12_pipeline_library_read_blob(struct d3d12_pipeline_library *pipeline_library,
        struct d3d12_device *device, const void *blob, size_t blob_length)
{
    const VkPhysicalDeviceProperties *device_properties = &device->device_info.properties2.properties;
    const struct vkd3d_serialized_pipeline_library *header = blob;
    const uint8_t *serialized_data_base;
    size_t serialized_data_size;
    const uint8_t *name_table;
    size_t header_entry_size;
    size_t total_toc_entries;
    uint32_t i;
    HRESULT hr;

    /* Same logic as for pipeline blobs, indicate that the app needs
     * to rebuild the pipeline library in case vkd3d itself or the
     * underlying device/driver changed */
    if (blob_length < sizeof(*header) || header->version != VKD3D_PIPELINE_LIBRARY_VERSION)
        return D3D12_ERROR_DRIVER_VERSION_MISMATCH;

    if (header->device_id != device_properties->deviceID || header->vendor_id != device_properties->vendorID)
        return D3D12_ERROR_ADAPTER_NOT_FOUND;

    if (header->vkd3d_build != vkd3d_build ||
            header->vkd3d_shader_interface_key != device->shader_interface_key ||
            memcmp(header->cache_uuid, device_properties->pipelineCacheUUID, VK_UUID_SIZE) != 0)
        return D3D12_ERROR_DRIVER_VERSION_MISMATCH;

    total_toc_entries = header->pipeline_count + header->spirv_count + header->driver_cache_count;

    header_entry_size = offsetof(struct vkd3d_serialized_pipeline_library, entries) +
            total_toc_entries * sizeof(struct vkd3d_serialized_pipeline_toc_entry);

    if (blob_length < header_entry_size)
        return D3D12_ERROR_DRIVER_VERSION_MISMATCH;

    serialized_data_size = blob_length - header_entry_size;
    serialized_data_base = (const uint8_t *)&header->entries[total_toc_entries];
    name_table = serialized_data_base;

    i = 0;

    if (FAILED(hr = d3d12_pipeline_library_unserialize_hash_map(pipeline_library,
            &header->entries[i], header->spirv_count,
            &pipeline_library->spirv_cache_map, serialized_data_base, serialized_data_size,
            &name_table)))
        return hr;
    i += header->spirv_count;

    if (FAILED(hr = d3d12_pipeline_library_unserialize_hash_map(pipeline_library,
            &header->entries[i], header->driver_cache_count,
            &pipeline_library->driver_cache_map, serialized_data_base, serialized_data_size,
            &name_table)))
        return hr;
    i += header->driver_cache_count;

    if (FAILED(hr = d3d12_pipeline_library_unserialize_hash_map(pipeline_library,
            &header->entries[i], header->pipeline_count,
            &pipeline_library->pso_map, serialized_data_base, serialized_data_size,
            &name_table)))
        return hr;
    i += header->pipeline_count;

    return S_OK;
}

static HRESULT d3d12_pipeline_library_init(struct d3d12_pipeline_library *pipeline_library,
        struct d3d12_device *device, const void *blob, size_t blob_length)
{
    HRESULT hr;
    int rc;

    memset(pipeline_library, 0, sizeof(*pipeline_library));
    pipeline_library->ID3D12PipelineLibrary_iface.lpVtbl = &d3d12_pipeline_library_vtbl;
    pipeline_library->refcount = 1;

    if (!blob_length && blob)
        return E_INVALIDARG;

    if ((rc = rwlock_init(&pipeline_library->mutex)))
        return hresult_from_errno(rc);

    hash_map_init(&pipeline_library->spirv_cache_map, vkd3d_cached_pipeline_hash_internal,
            vkd3d_cached_pipeline_compare_internal, sizeof(struct vkd3d_cached_pipeline_entry));
    hash_map_init(&pipeline_library->driver_cache_map, vkd3d_cached_pipeline_hash_internal,
            vkd3d_cached_pipeline_compare_internal, sizeof(struct vkd3d_cached_pipeline_entry));
    hash_map_init(&pipeline_library->pso_map, vkd3d_cached_pipeline_hash_name,
            vkd3d_cached_pipeline_compare_name, sizeof(struct vkd3d_cached_pipeline_entry));

    if (blob_length)
    {
        if (FAILED(hr = d3d12_pipeline_library_read_blob(pipeline_library, device, blob, blob_length)))
            goto cleanup_hash_map;
    }

    if (FAILED(hr = vkd3d_private_store_init(&pipeline_library->private_store)))
        goto cleanup_mutex;

    d3d12_device_add_ref(pipeline_library->device = device);
    return hr;

cleanup_hash_map:
    hash_map_clear(&pipeline_library->pso_map);
    hash_map_clear(&pipeline_library->spirv_cache_map);
    hash_map_clear(&pipeline_library->driver_cache_map);
cleanup_mutex:
    rwlock_destroy(&pipeline_library->mutex);
    return hr;
}

HRESULT d3d12_pipeline_library_create(struct d3d12_device *device, const void *blob,
        size_t blob_length, struct d3d12_pipeline_library **pipeline_library)
{
    struct d3d12_pipeline_library *object;
    HRESULT hr;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d12_pipeline_library_init(object, device, blob, blob_length)))
    {
        vkd3d_free(object);
        return hr;
    }

    TRACE("Created pipeline library %p.\n", object);

    *pipeline_library = object;
    return S_OK;
}
