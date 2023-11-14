/*********************************************************************
    Blosc - Blocked Shuffling and Compression Library

    Copyright (c) 2021  The Blosc Development Team <blosc@blosc.org>
    https://blosc.org
    License: BSD 3-Clause (see LICENSE.txt)

    See LICENSE.txt for details about copyright and rights to use.
**********************************************************************/

#include <memory>

#include "grok.h"
#include "blosc2_grok.h"

int blosc2_grok_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk
) {
    int size = -1;

    // Read blosc2 metadata
    uint8_t *content;
    int32_t content_len;
    BLOSC_ERROR(blosc2_meta_get((blosc2_schunk*)cparams->schunk, "b2nd",
                                &content, &content_len));

    int8_t ndim;
    int64_t shape[3];
    int32_t chunkshape[3];
    int32_t blockshape[3];
    char *dtype;
    int8_t dtype_format;
    BLOSC_ERROR(
        b2nd_deserialize_meta(content, content_len, &ndim,
                              shape, chunkshape, blockshape, &dtype, &dtype_format)
    );
    free(content);
    free(dtype);

    const uint32_t numComps = blockshape[0];
    const uint32_t dimX = blockshape[1];
    const uint32_t dimY = blockshape[2];
    const uint32_t typesize = ((blosc2_schunk*)cparams->schunk)->typesize;
    const uint32_t precision = typesize * 8;

    // initialize compress parameters
    grk_codec* codec = nullptr;
    blosc2_grok_params *codec_params = (blosc2_grok_params *)cparams->codec_params;
    grk_cparameters *compressParams = &codec_params->compressParams;
    grk_stream_params *streamParams = &codec_params->streamParams;
    grk_set_default_stream_params(streamParams);
    //WriteStreamInfo sinfo(&streamParams);

    std::unique_ptr<uint8_t[]> data;
    size_t bufLen = (size_t)numComps * ((precision + 7) / 8) * dimX * dimY;
    data = std::make_unique<uint8_t[]>(bufLen);
    streamParams->buf = data.get();
    streamParams->buf_len = bufLen;

    // create blank image
    auto* components = new grk_image_comp[numComps];
    for(uint32_t i = 0; i < numComps; ++i) {
        auto c = components + i;
        c->w = dimX;
        c->h = dimY;
        c->dx = 1;
        c->dy = 1;
        c->prec = precision;
        c->sgnd = false;
    }
    grk_image* image = grk_image_new(
        numComps, components, GRK_CLRSPC_SRGB, true);

    // fill in component data
    // see grok.h header for full details of image structure

    auto *ptr = (uint8_t*)input;
    for (uint16_t compno = 0; compno < image->numcomps; ++compno) {
        auto comp = image->comps + compno;
        auto compWidth = comp->w;
        auto compHeight = comp->h;
        auto compData = comp->data;
        if(!compData) {
            fprintf(stderr, "Image has null data for component %d\n", compno);
            goto beach;
        }
        // fill in component data, taking component stride into account
        // in this example, we just zero out each component
        auto srcData = new int32_t[compWidth * compHeight];

        uint32_t len = compWidth * compHeight * typesize;
        memcpy(srcData, ptr, len);
        ptr += len;
        //memset(srcData, 0, compWidth * compHeight * sizeof(int32_t));

        auto srcPtr = srcData;
        for(uint32_t j = 0; j < compHeight; ++j) {
            memcpy(compData, srcPtr, compWidth * sizeof(int32_t));
            srcPtr += compWidth;
            compData += comp->stride;
        }
        delete[] srcData;
    }

    // initialize compressor
    codec = grk_compress_init(streamParams, compressParams, image);
    if (!codec) {
        fprintf(stderr, "Failed to initialize compressor\n");
        goto beach;
    }

    // compress
    size = (int)grk_compress(codec, nullptr);
    if (size == 0) {
        fprintf(stderr, "Failed to compress\n");
        goto beach;
    }
    if (size > output_len) {
        // Uncompressible data
        return 0;
    }
    memcpy(output, streamParams->buf, size);

    printf("Compression succeeded: %d bytes used.\n", size);

beach:
    // cleanup
    delete[] components;
    grk_object_unref(codec);
    grk_object_unref(&image->obj);
    grk_deinitialize();

    return size;
}

// Decompress a block
int blosc2_grok_decoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                        uint8_t meta, blosc2_dparams *dparams, const void *chunk) {
    int rc = EXIT_FAILURE;

    uint16_t numTiles = 0;

    // initialize decompress parameters
    grk_decompress_parameters decompressParams;
    grk_decompress_set_default_params(&decompressParams);
    decompressParams.compressionLevel = GRK_DECOMPRESS_COMPRESSION_LEVEL_DEFAULT;
    decompressParams.verbose_ = true;

    grk_image *image = nullptr;
    grk_codec *codec = nullptr;

    // initialize library
    grk_initialize(nullptr, 0, false);

    printf("Decompressing buffer\n");

    // initialize decompressor
    grk_stream_params streamParams;
    grk_set_default_stream_params(&streamParams);

    streamParams.stream_len = input_len;
    streamParams.buf = (uint8_t *)input;
    streamParams.buf_len = input_len;

    codec = grk_decompress_init(&streamParams, &decompressParams.core);
    if (!codec) {
        fprintf(stderr, "Failed to set up decompressor\n");
        goto beach;
    }

    // read j2k header
    grk_header_info headerInfo;
    memset(&headerInfo, 0, sizeof(headerInfo));
    if (!grk_decompress_read_header(codec, &headerInfo)) {
        fprintf(stderr, "Failed to read the header\n");
        goto beach;
    }

    // retrieve image that will store uncompressed image data
    image = grk_decompress_get_composited_image(codec);
    if (!image) {
        fprintf(stderr, "Failed to retrieve image \n");
        goto beach;
    }

    numTiles = (uint16_t)(headerInfo.t_grid_width * headerInfo.t_grid_height);
    printf("\nImage Info\n");
    printf("Width: %d\n", image->x1 - image->x0);
    printf("Height: %d\n", image->y1 - image->y0);
    printf("Number of components: %d\n", image->numcomps);
    for (uint16_t compno = 0; compno < image->numcomps; ++compno) {
        printf("Precision of component %d : %d\n", compno, image->comps[compno].prec);
    }
    printf("Number of tiles: %d\n", numTiles);
    if (numTiles > 1) {
        printf("Nominal tile dimensions: (%d,%d)\n", headerInfo.t_width, headerInfo.t_height);
    }

    // decompress all tiles
    if (!grk_decompress(codec, nullptr))
        goto beach;

    // see grok.h header for full details of image structure
    for (uint16_t compno = 0; compno < image->numcomps; ++compno) {
        auto comp = image->comps + compno;
        auto compWidth = comp->w;
        auto compHeight = comp->h;
        auto compData = comp->data;
        if (!compData) {
            fprintf(stderr, "Image has null data for component %d\n", compno);
            goto beach;
        }
        printf("Component %d : dimensions (%d,%d) at precision %d\n",
               compno, compWidth, compHeight, comp->prec);

        // copy data, taking component stride into account
        auto copiedData = new int32_t[compWidth * compHeight];
        auto copyPtr = copiedData;
        for (uint32_t j = 0; j < compHeight; ++j) {
            memcpy(copyPtr, compData, compWidth * sizeof(int32_t));
            copyPtr += compWidth;
            compData += comp->stride;
        }
        delete[] copiedData;
    }

    rc = EXIT_SUCCESS;
beach:
    // cleanup
    grk_object_unref(codec);
    grk_deinitialize();

    return rc;
}

void blosc2_grok_init(uint32_t nthreads, bool verbose) {
    // initialize library
    grk_initialize(nullptr, nthreads, verbose);
}

void blosc2_grok_destroy() {
    grk_deinitialize();
}
