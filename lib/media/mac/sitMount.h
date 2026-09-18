/*
 * sitMount.h - unstuff a StuffIt/BinHex archive into a PSRAM-backed disk
 * image so the normal Mac disk mount path (macFloppy::mount()) can mount
 * it exactly as if it were a plain host file.
 *
 * This is glue code between lib/stuffit (the pure C99 archive/BinHex
 * reader) and the Mac media layer; it is original to this project and is
 * BUILD_MAC only.
 */
#ifndef _SIT_MOUNT_H
#define _SIT_MOUNT_H
#ifdef BUILD_MAC

#include <cstdint>
#include <cstdio>
#include "mediaType.h"

// What the archive's chosen entry turned out to contain, once its bytes
// have been looked at (magic bytes / size), not just its name.
enum sit_image_kind_t
{
    SIT_IMAGE_UNKNOWN = 0,
    SIT_IMAGE_FLOPPY,   // DiskCopy 4.2 or a raw 400K/800K sector dump
    SIT_IMAGE_HD20      // HFS volume or a drive image (Driver Descriptor Map)
};

// Holds the extraction state for one archive-backed mount: the PSRAM
// image buffer, the small resource-fork buffer kept for a future NDIF
// hook, and the fmemopen() FILE* handed to the normal mount path.
//
// Ownership: once extract() succeeds, image_fh is normally passed
// straight into macFloppy::mount(), which stores it as the MediaType
// subclass's _media_fileh; that object's own unmount() is what actually
// fclose()s it (see MediaType::unmount() in mediaType.cpp). Because
// fmemopen()'s fclose() does not free the backing buffer, the owner of a
// SitMount (macFloppy) must still free image_buf/rsrc_buf itself - but
// must not fclose() image_fh a second time once _disk->unmount() already
// has. The caller does this by clearing image_fh to nullptr right after
// _disk->unmount() and before deleting/releasing the SitMount - see
// macFloppy::unmount() in lib/device/mac/floppy.cpp.
class SitMount
{
public:
    SitMount() {}
    ~SitMount() { release(); }

    // Extract archive_fh (a StuffIt "SIT!"/StuffIt5 archive, optionally
    // BinHex-wrapped) and pick the disk image entry inside it. Does not
    // take ownership of archive_fh and does not close it - the caller
    // decides what to do with the archive handle once this returns.
    //
    // On success, returns true with image_fh/image_len/inner_filename/
    // kind/disk_type filled in. On failure, returns false, logs a clear
    // Debug_printf explaining why, and leaves this object in a released
    // (all-null) state.
    bool extract(FILE *archive_fh, const char *archive_filename);

    // Frees image_buf/rsrc_buf and, if still non-null, fclose()s
    // image_fh. Safe to call more than once and on a never-extracted
    // instance. Set image_fh to nullptr before calling this if something
    // else already closed it (see the ownership note above).
    void release();

    FILE *image_fh = nullptr;              // fmemopen(image_buf, image_len, "rb+")
    uint8_t *image_buf = nullptr;          // heap_caps_malloc'd, MALLOC_CAP_SPIRAM
    uint32_t image_len = 0;
    char inner_filename[256] = {0};        // basename of the entry chosen inside the archive
    sit_image_kind_t kind = SIT_IMAGE_UNKNOWN;
    mediatype_t disk_type = MEDIATYPE_UNKNOWN; // MEDIATYPE_DC42 or MEDIATYPE_DSK, matching kind

private:
    uint8_t *rsrc_buf = nullptr;           // resource fork, capped small; for a future NDIF hook
    uint32_t rsrc_len = 0;

    bool classify();
};

#endif // BUILD_MAC
#endif // _SIT_MOUNT_H
