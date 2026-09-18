#ifdef BUILD_MAC
#include "sitMount.h"

#include <cstring>
#include <esp_heap_caps.h>

#include "../../stuffit/stuffit.h"
#include "../../stuffit/binhex.h"

#include "../../include/debug.h"

// lib/stuffit/ndif.h (Disk Copy 6 / NDIF chunked images: ndif_probe(),
// ndif_open(), ndif_extract(), ndif_close()) is picked up automatically
// when present. If it's ever missing (e.g. an older checkout), fall back
// to a name-based guess so an NDIF image at least mounts (as a raw,
// not-yet-decoded HD20 volume) instead of failing outright.
#if defined(__has_include)
#if __has_include("../../stuffit/ndif.h")
#include "../../stuffit/ndif.h"
#define SIT_MOUNT_HAVE_NDIF 1
#endif
#endif

#define SIT_MOUNT_MAX_IMAGE     (3u * 1024u * 1024u)   // PSRAM cap for the inner disk image
#define SIT_MOUNT_MAX_RSRC      (64u * 1024u)          // PSRAM cap for the resource fork
#define SIT_MOUNT_PROGRESS_STEP (256u * 1024u)         // Debug_printf every this many bytes

// ------------------------------------------------------------------- //
// sit_allocator backed by PSRAM, shared by hqx_open()/sit_open() so the
// whole archive-decode scratch (Arsenic's block buffers included) lands
// in PSRAM rather than eating into the small internal SRAM heap.
// ------------------------------------------------------------------- //
static void *sit_mount_alloc(size_t n, void *ctx)
{
    (void)ctx;
    return heap_caps_malloc(n, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
}

static void sit_mount_free(void *p, void *ctx)
{
    (void)ctx;
    if (p != nullptr)
        heap_caps_free(p);
}

static const sit_allocator sit_mount_allocator = { sit_mount_alloc, sit_mount_free, nullptr };

static bool sit_mount_has_image_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == nullptr)
        return false;
    static const char *exts[] = { ".image", ".img", ".dsk", ".dc42", ".hda", ".dmg", ".toast" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (strcasecmp(dot, exts[i]) == 0)
            return true;
    return false;
}

// ------------------------------------------------------------------- //
// sit_extract() sink: copies into a pre-sized buffer, logs progress.
// ------------------------------------------------------------------- //
struct sit_mount_sink_ctx
{
    uint8_t *buf;
    uint32_t cap;
    uint32_t pos;
    uint32_t next_progress_at;
    const char *what; // "data fork" / "resource fork", for the progress line
};

static int sit_mount_sink(const uint8_t *data, size_t n, void *vctx)
{
    sit_mount_sink_ctx *ctx = (sit_mount_sink_ctx *)vctx;
    if (ctx->pos + n > ctx->cap)
        return -1; // should not happen - caller sized the buffer from the entry's own data_len
    memcpy(ctx->buf + ctx->pos, data, n);
    ctx->pos += n;
    while (ctx->next_progress_at <= ctx->cap && ctx->pos >= ctx->next_progress_at)
    {
        Debug_printf("\nStuffIt: extracting %s: %u/%u bytes", ctx->what, ctx->next_progress_at, ctx->cap);
        ctx->next_progress_at += SIT_MOUNT_PROGRESS_STEP;
    }
    return 0;
}

void SitMount::release()
{
    if (image_fh != nullptr)
    {
        fclose(image_fh);
        image_fh = nullptr;
    }
    if (image_buf != nullptr)
    {
        heap_caps_free(image_buf);
        image_buf = nullptr;
    }
    if (rsrc_buf != nullptr)
    {
        heap_caps_free(rsrc_buf);
        rsrc_buf = nullptr;
    }
    image_len = 0;
    rsrc_len = 0;
    inner_filename[0] = '\0';
    kind = SIT_IMAGE_UNKNOWN;
    disk_type = MEDIATYPE_UNKNOWN;
}

bool SitMount::classify()
{
    kind = SIT_IMAGE_UNKNOWN;
    disk_type = MEDIATYPE_UNKNOWN;

    if (image_len >= 0x54 && image_buf[0x52] == 0x01 && image_buf[0x53] == 0x00)
    {
        // DiskCopy 4.2 magic
        kind = SIT_IMAGE_FLOPPY;
        disk_type = MEDIATYPE_DC42;
    }
    else if (image_len == 409600 || image_len == 819200)
    {
        // Raw 400K/800K sector dump
        kind = SIT_IMAGE_FLOPPY;
        disk_type = MEDIATYPE_DSK;
    }
    else if (image_len >= 0x402 && image_buf[0x400] == 'B' && image_buf[0x401] == 'D')
    {
        // HFS master directory block signature
        kind = SIT_IMAGE_HD20;
        disk_type = MEDIATYPE_DSK;
    }
    else if (image_len >= 2 && image_buf[0] == 'E' && image_buf[1] == 'R')
    {
        // Driver Descriptor Map signature (a drive image, not just a volume)
        kind = SIT_IMAGE_HD20;
        disk_type = MEDIATYPE_DSK;
    }
#if !defined(SIT_MOUNT_HAVE_NDIF)
    else if (rsrc_len > 0 && sit_mount_has_image_ext(inner_filename))
    {
        // No lib/stuffit/ndif.h in this checkout to give us a real
        // ndif_probe() - guess from the name instead so an NDIF image at
        // least mounts (as a raw, not-yet-decoded HD20 volume) rather
        // than failing outright. sit_mount_decode_ndif() in extract()
        // handles the real case when ndif.h is present.
        Debug_printf("\nStuffIt: '%s' has a resource fork and an image-like name - guessing Disk Copy "
                     "6/NDIF, treating its data fork as a raw HD20 volume of its stored size (this WILL "
                     "be garbage without ndif.h to decode it)",
                     inner_filename);
        kind = SIT_IMAGE_HD20;
        disk_type = MEDIATYPE_DSK;
    }
#endif

    return kind != SIT_IMAGE_UNKNOWN;
}

#if defined(SIT_MOUNT_HAVE_NDIF)
// Decodes an NDIF (Disk Copy 6) image in place: *image_buf currently holds
// the archive entry's raw data fork (the still chunk-compressed NDIF
// stream); rsrc_buf/rsrc_len hold its resource fork (the 'bcem' block map
// lives in there). On success, *image_buf/*image_len are replaced with a
// freshly allocated, fully decoded raw image (block_count * 512 bytes);
// the original compressed buffer is freed. On failure, *image_buf is left
// untouched and the caller should treat this as a normal classify()
// failure.
static bool sit_mount_decode_ndif(uint8_t **image_buf, uint32_t *image_len,
                                   const uint8_t *rsrc_buf, size_t rsrc_len,
                                   const char *inner_filename)
{
    // ndif_open() reads the (still compressed) data fork through a FILE*,
    // seeking it per chunk - fmemopen() over the buffer we already have
    // gives it exactly that, with no extra copy.
    FILE *ndif_data_fh = fmemopen(*image_buf, *image_len, "rb");
    if (ndif_data_fh == nullptr)
    {
        Debug_printf("\nStuffIt: NDIF fmemopen() failed for '%s'", inner_filename);
        return false;
    }

    ndif_image nd;
    int nrc = ndif_open(&nd, ndif_data_fh, rsrc_buf, rsrc_len, &sit_mount_allocator);
    if (nrc != NDIF_OK)
    {
        Debug_printf("\nStuffIt: NDIF decode of '%s' failed to open: %s", inner_filename, ndif_strerror(nrc));
        fclose(ndif_data_fh);
        return false;
    }

    uint32_t decoded_len = nd.block_count * 512u;
    if (decoded_len == 0 || decoded_len > SIT_MOUNT_MAX_IMAGE)
    {
        Debug_printf("\nStuffIt: NDIF image '%s' decodes to %u bytes, over the %u byte PSRAM cap",
                     inner_filename, decoded_len, SIT_MOUNT_MAX_IMAGE);
        ndif_close(&nd);
        fclose(ndif_data_fh);
        return false;
    }

    uint8_t *decoded_buf = (uint8_t *)heap_caps_malloc(decoded_len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (decoded_buf == nullptr)
    {
        Debug_printf("\nStuffIt: no PSRAM for a %u byte decoded NDIF image ('%s')", decoded_len, inner_filename);
        ndif_close(&nd);
        fclose(ndif_data_fh);
        return false;
    }

    sit_mount_sink_ctx nctx = { decoded_buf, decoded_len, 0, SIT_MOUNT_PROGRESS_STEP, "NDIF image" };
    int erc = ndif_extract(&nd, sit_mount_sink, &nctx);

    ndif_close(&nd);
    fclose(ndif_data_fh); // done with the compressed chunk data now

    if (erc != NDIF_OK)
    {
        Debug_printf("\nStuffIt: NDIF decode of '%s' failed: %s", inner_filename, ndif_strerror(erc));
        heap_caps_free(decoded_buf);
        return false;
    }

    // Swap in the decoded raw image; the compressed data fork buffer this
    // call was given is no longer needed.
    heap_caps_free(*image_buf);
    *image_buf = decoded_buf;
    *image_len = nctx.pos;
    return true;
}
#endif // SIT_MOUNT_HAVE_NDIF

// sit_archive is large (its StuffIt5 directory table alone is tens of
// KB - SIT_MAX_DIR_TABLE entries of SIT_MAX_PATH bytes each) and
// sit_entry is ~300 bytes; extract() below heap-allocates all three
// (through sit_mount_allocator, so they land in PSRAM) rather than
// keeping them as locals, to keep its own stack frame small. This
// helper frees whichever of them are non-null, for use on every one
// of extract()'s many early-return paths.
static void sit_mount_free_locals(sit_archive *ar, sit_entry *e, sit_entry *best)
{
    if (ar != nullptr)
        sit_mount_allocator.free(ar, sit_mount_allocator.ctx);
    if (e != nullptr)
        sit_mount_allocator.free(e, sit_mount_allocator.ctx);
    if (best != nullptr)
        sit_mount_allocator.free(best, sit_mount_allocator.ctx);
}

bool SitMount::extract(FILE *archive_fh, const char *archive_filename)
{
    release();

    sit_archive *ar = (sit_archive *)sit_mount_allocator.alloc(sizeof(sit_archive), sit_mount_allocator.ctx);
    sit_entry *e = (sit_entry *)sit_mount_allocator.alloc(sizeof(sit_entry), sit_mount_allocator.ctx);
    sit_entry *best = (sit_entry *)sit_mount_allocator.alloc(sizeof(sit_entry), sit_mount_allocator.ctx);
    if (ar == nullptr || e == nullptr || best == nullptr)
    {
        Debug_printf("\nStuffIt: no PSRAM for archive/entry state for '%s'", archive_filename);
        sit_mount_free_locals(ar, e, best);
        return false;
    }

    // --- Is this a BinHex-wrapped archive? ---
    bool looks_like_hqx = false;
    const char *dot = strrchr(archive_filename, '.');
    if (dot != nullptr && strcasecmp(dot, ".hqx") == 0)
        looks_like_hqx = true;

    if (!looks_like_hqx)
    {
        char peek[80];
        if (fseek(archive_fh, 0, SEEK_SET) == 0)
        {
            size_t got = fread(peek, 1, sizeof(peek) - 1, archive_fh);
            peek[got] = '\0';
            if (strstr(peek, "This file must be converted with BinHex") != nullptr)
                looks_like_hqx = true;
        }
    }
    if (fseek(archive_fh, 0, SEEK_SET) != 0)
    {
        Debug_printf("\nStuffIt: cannot seek archive '%s'", archive_filename);
        sit_mount_free_locals(ar, e, best);
        return false;
    }

    hqx_file hqx;
    bool have_hqx = false;
    FILE *hqx_fh = nullptr;
    FILE *sit_source = archive_fh;

    if (looks_like_hqx)
    {
        int hrc = hqx_open(archive_fh, &sit_mount_allocator, &hqx);
        if (hrc == SIT_OK)
        {
            have_hqx = true;
            hqx_fh = hqx_data_fork(&hqx);
            if (hqx_fh == nullptr)
            {
                Debug_printf("\nStuffIt: hqx_data_fork() (fmemopen) failed for '%s'", archive_filename);
                hqx_close(&hqx);
                sit_mount_free_locals(ar, e, best);
                return false;
            }
            sit_source = hqx_fh;
        }
        else if (hrc == SIT_E_FORMAT)
        {
            // .hqx extension but no banner found - try it as a bare .sit
            fseek(archive_fh, 0, SEEK_SET);
            sit_source = archive_fh;
        }
        else
        {
            Debug_printf("\nStuffIt: BinHex decode of '%s' failed: %s", archive_filename, sit_strerror(hrc));
            sit_mount_free_locals(ar, e, best);
            return false;
        }
    }

    int rc = sit_open(ar, sit_source, &sit_mount_allocator);
    if (rc != SIT_OK)
    {
        Debug_printf("\nStuffIt: '%s' is not a recognized SIT!/StuffIt5 archive: %s",
                     archive_filename, sit_strerror(rc));
        if (hqx_fh != nullptr)
            fclose(hqx_fh);
        if (have_hqx)
            hqx_close(&hqx);
        sit_mount_free_locals(ar, e, best);
        return false;
    }

    // Walk every entry once, remembering the best disk-image candidate:
    // a preferred extension wins outright, otherwise the largest data
    // fork wins. Resource-only entries and folders (data_len==0) are
    // skipped. sit_extract() can be called on a saved sit_entry after the
    // iterator has moved on (it seeks the FILE* via the entry's own
    // stored offsets), so a single forward pass is enough.
    bool have_candidate = false;
    bool best_preferred = false;

    while ((rc = sit_next_entry(ar, e)) == 1)
    {
        if (e->data_len == 0)
            continue;

        bool preferred = sit_mount_has_image_ext(e->path);
        if (!have_candidate ||
            (preferred && !best_preferred) ||
            (preferred == best_preferred && e->data_len > best->data_len))
        {
            *best = *e;
            best_preferred = preferred;
            have_candidate = true;
        }
    }

    if (rc < 0)
    {
        Debug_printf("\nStuffIt: error walking '%s': %s", archive_filename, sit_strerror(rc));
        sit_close(ar);
        if (hqx_fh != nullptr)
            fclose(hqx_fh);
        if (have_hqx)
            hqx_close(&hqx);
        sit_mount_free_locals(ar, e, best);
        return false;
    }

    if (!have_candidate)
    {
        Debug_printf("\nStuffIt: '%s' has no disk image entry", archive_filename);
        sit_close(ar);
        if (hqx_fh != nullptr)
            fclose(hqx_fh);
        if (have_hqx)
            hqx_close(&hqx);
        sit_mount_free_locals(ar, e, best);
        return false;
    }

    if (best->data_len > SIT_MOUNT_MAX_IMAGE)
    {
        Debug_printf("\nStuffIt: '%s' inner image '%s' is %u bytes, over the %u byte PSRAM cap",
                     archive_filename, best->path, best->data_len, SIT_MOUNT_MAX_IMAGE);
        sit_close(ar);
        if (hqx_fh != nullptr)
            fclose(hqx_fh);
        if (have_hqx)
            hqx_close(&hqx);
        sit_mount_free_locals(ar, e, best);
        return false;
    }

    image_buf = (uint8_t *)heap_caps_malloc(best->data_len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (image_buf == nullptr)
    {
        Debug_printf("\nStuffIt: no PSRAM for a %u byte image ('%s')", best->data_len, best->path);
        sit_close(ar);
        if (hqx_fh != nullptr)
            fclose(hqx_fh);
        if (have_hqx)
            hqx_close(&hqx);
        sit_mount_free_locals(ar, e, best);
        return false;
    }

    sit_mount_sink_ctx dctx = { image_buf, best->data_len, 0, SIT_MOUNT_PROGRESS_STEP, "data fork" };
    sit_progress prog;
    rc = sit_extract(ar, best, SIT_FORK_DATA, sit_mount_sink, &dctx, &prog);
    if (rc != SIT_OK)
    {
        Debug_printf("\nStuffIt: extracting '%s' failed: %s", best->path, sit_strerror(rc));
        sit_close(ar);
        if (hqx_fh != nullptr)
            fclose(hqx_fh);
        if (have_hqx)
            hqx_close(&hqx);
        sit_mount_free_locals(ar, e, best);
        release();
        return false;
    }
    image_len = dctx.pos;

    // Resource fork: best-effort, small cap, kept only for a future NDIF
    // block-map hook - never fatal to the mount if it's missing/too big.
    if (best->rsrc_len > 0 && best->rsrc_len <= SIT_MOUNT_MAX_RSRC)
    {
        rsrc_buf = (uint8_t *)heap_caps_malloc(best->rsrc_len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (rsrc_buf != nullptr)
        {
            sit_mount_sink_ctx rctx = { rsrc_buf, best->rsrc_len, 0, SIT_MOUNT_PROGRESS_STEP, "resource fork" };
            rc = sit_extract(ar, best, SIT_FORK_RSRC, sit_mount_sink, &rctx, &prog);
            if (rc == SIT_OK)
            {
                rsrc_len = rctx.pos;
            }
            else
            {
                Debug_printf("\nStuffIt: resource fork of '%s' not extracted (%s) - continuing without it",
                             best->path, sit_strerror(rc));
                heap_caps_free(rsrc_buf);
                rsrc_buf = nullptr;
            }
        }
    }
    else if (best->rsrc_len > SIT_MOUNT_MAX_RSRC)
    {
        Debug_printf("\nStuffIt: resource fork of '%s' is %u bytes, over the %u byte cap - skipping",
                     best->path, best->rsrc_len, SIT_MOUNT_MAX_RSRC);
    }

    sit_close(ar);
    if (hqx_fh != nullptr)
        fclose(hqx_fh);
    if (have_hqx)
        hqx_close(&hqx);

    const char *base = strrchr(best->path, '/');
    base = (base != nullptr) ? base + 1 : best->path;
    strncpy(inner_filename, base, sizeof(inner_filename) - 1);
    inner_filename[sizeof(inner_filename) - 1] = '\0';

    // ar/e/best have served their purpose (the archive is closed and its
    // best-candidate entry copied out above) - free them now rather than
    // holding PSRAM through the NDIF decode/classify steps below.
    sit_mount_free_locals(ar, e, best);

#if defined(SIT_MOUNT_HAVE_NDIF)
    if (rsrc_buf != nullptr && ndif_probe(rsrc_buf, rsrc_len))
    {
        Debug_printf("\nStuffIt: '%s' is an NDIF (Disk Copy 6) image - decoding", inner_filename);
        if (!sit_mount_decode_ndif(&image_buf, &image_len, rsrc_buf, rsrc_len, inner_filename))
        {
            release();
            return false;
        }
    }
    // A real ndif_probe() has now definitively ruled NDIF in or out;
    // nothing past this point needs the resource fork either way.
    if (rsrc_buf != nullptr)
    {
        heap_caps_free(rsrc_buf);
        rsrc_buf = nullptr;
        rsrc_len = 0;
    }
#endif

    if (!classify())
    {
        Debug_printf("\nStuffIt: cannot classify inner image '%s' (%u bytes) from '%s' - not DC42, "
                     "not 400K/800K, not HFS, not a drive image",
                     inner_filename, image_len, archive_filename);
        release();
        return false;
    }

    image_fh = fmemopen(image_buf, image_len, "rb+");
    if (image_fh == nullptr)
    {
        Debug_printf("\nStuffIt: fmemopen() failed for extracted image '%s'", inner_filename);
        release();
        return false;
    }

    return true;
}

#endif // BUILD_MAC
