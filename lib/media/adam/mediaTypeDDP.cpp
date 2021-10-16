#ifdef BUILD_ADAM
#include <memory.h>
#include <string.h>

#include "../../include/debug.h"
#include "../utils/utils.h"

#include "fnSystem.h"
#include "../device/adamnet/disk.h"

#include "mediaTypeDDP.h"

// Returns byte offset of given sector number (1-based)
uint32_t MediaTypeDDP::_block_to_offset(uint32_t blockNum)
{
    return blockNum * 1024;
}

// Returns TRUE if an error condition occurred
bool MediaTypeDDP::read(uint16_t blockNum, uint16_t *readcount)
{
    Debug_print("ATR READ\n");

    *readcount = 0;

    // Return an error if we're trying to read beyond the end of the disk
    if (blockNum > _media_num_blocks)
    {
        Debug_printf("::read block %d > %d\n", blockNum, _media_num_blocks);
        return true;
    }

    memset(_media_blockbuff, 0, sizeof(_media_blockbuff));

    bool err = false;
    // Perform a seek if we're not reading the sector after the last one we read
    if (blockNum != _media_last_block + 1)
    {
        uint32_t offset = _block_to_offset(blockNum);
        err = fseek(_media_fileh, offset, SEEK_SET) != 0;
    }

    if (err == false)
        err = fread(_media_blockbuff, 1, 1024, _media_fileh) != 1024;

    if (err == false)
        _media_last_block = blockNum;
    else
        _media_last_block = INVALID_SECTOR_VALUE;

    *readcount = 1024;

    return err;
}

// Returns TRUE if an error condition occurred
bool MediaTypeDDP::write(uint16_t blockNum, bool verify)
{
    Debug_printf("ATR WRITE\n", blockNum, _media_num_blocks);

    // Return an error if we're trying to write beyond the end of the disk
    if (blockNum > _media_num_blocks)
    {
        Debug_printf("::write block %d > %d\n", blockNum, _media_num_blocks);
        return true;
    }

    uint32_t offset = _block_to_offset(blockNum);

    _media_last_block = INVALID_SECTOR_VALUE;

    // Perform a seek if we're writing to the sector after the last one
    int e;
    if (blockNum != _media_last_block + 1)
    {
        e = fseek(_media_fileh, offset, SEEK_SET);
        if (e != 0)
        {
            Debug_printf("::write seek error %d\n", e);
            return true;
        }
    }
    // Write the data
    e = fwrite(_media_blockbuff, 1, 1024, _media_fileh);
    if (e != 1024)
    {
        Debug_printf("::write error %d, %d\n", e, errno);
        return true;
    }

    int ret = fflush(_media_fileh);    // This doesn't seem to be connected to anything in ESP-IDF VF, so it may not do anything
    ret = fsync(fileno(_media_fileh)); // Since we might get reset at any moment, go ahead and sync the file (not clear if fflush does this)
    Debug_printf("DDP::write fsync:%d\n", ret);

    _media_last_block = blockNum;

    return false;
}

void MediaTypeDDP::status(uint8_t statusbuff[4])
{
}

/*
    From Altirra manual:
    The format command formats a disk, writing 40 tracks and then verifying all sectors.
    All sectors are filleded with the data byte $00. On completion, the drive returns
    a sector-sized buffer containing a list of 16-bit bad sector numbers terminated by $FFFF.
*/
// Returns TRUE if an error condition occurred
bool MediaTypeDDP::format(uint16_t *responsesize)
{
    return false;
}

mediatype_t MediaTypeDDP::mount(FILE *f, uint32_t disksize)
{
    Debug_print("DDP MOUNT\n");

    _mediatype = MEDIATYPE_DDP;

    return _mediatype;
}

// Returns FALSE on error
bool MediaTypeDDP::create(FILE *f, uint32_t numBlocks)
{
    Debug_print("DDP CREATE\n");

    return true;
}
#endif /* BUILD_ATARI */