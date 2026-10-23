// fnfile:// - files FujiNet already has open, such as a disk image a Fuji device mounted
//
// fnfile://<name>/<path> reads the handle attached under <name>; <path> only names it.
// Whoever attaches a handle keeps owning it and must detach it before closing it.
#ifndef MEATLOAF_DEVICE_FNFILE
#define MEATLOAF_DEVICE_FNFILE

#include "meatloaf.h"
#include "fnio.h"

#include <string>
#include <unordered_map>


/********************************************************
 * MFileSystem
 ********************************************************/

class FnFileMFileSystem: public MFileSystem
{
public:
    FnFileMFileSystem(): MFileSystem("fnfile") {};

    static void attach(const std::string &name, fnFile *handle);
    static void detach(const std::string &name);
    static fnFile *handle(const std::string &name);

    bool handles(std::string name) override;
    MFile* getFile(std::string path) override;

private:
    static std::unordered_map<std::string, fnFile *> _handles;
};


/********************************************************
 * MFile
 ********************************************************/

class FnFileMFile: public MFile
{
public:
    FnFileMFile(std::string path);

    MStream* getSourceStream(std::ios_base::openmode mode=std::ios_base::in) override;
    MStream* getDecodedStream(std::shared_ptr<MStream> src) override;
    MStream* createStream(std::ios_base::openmode mode) override;

    bool isDirectory() override { return false; }
    bool rewindDirectory() override { return false; }
    MFile* getNextFileInDir() override { return nullptr; }
    bool exists() override { return !m_isNull; }
    bool remove() override { return false; }
    bool rename(std::string dest) override { return false; }
    time_t getLastWrite() override { return 0; }
    time_t getCreationTime() override { return 0; }
};


/********************************************************
 * MStream
 ********************************************************/

// Streams on one handle can be open at once, so each keeps its own position. They look the
// handle up on every call, so a stream outliving a detach fails instead of using a closed file.
class FnFileMStream: public MStream
{
public:
    explicit FnFileMStream(const std::string &name) : _name(name) {}

    bool isOpen() override { return _open && FnFileMFileSystem::handle(_name) != nullptr; }
    bool isRandomAccess() override { return true; }

    bool open(std::ios_base::openmode mode) override;
    void close() override { _open = false; }

    uint32_t read(uint8_t* buf, uint32_t size) override;
    uint32_t write(const uint8_t *buf, uint32_t size) override;

    using MStream::seek;
    bool seek(uint32_t pos) override;

private:
    std::string _name;
    bool _open = false;
};

#endif // MEATLOAF_DEVICE_FNFILE
