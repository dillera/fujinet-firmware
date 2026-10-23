#include "fnfile.h"

#include "string_utils.h"

#include <cstdio>


/********************************************************
 * MFileSystem implementations
 ********************************************************/

std::unordered_map<std::string, fnFile *> FnFileMFileSystem::_handles;

void FnFileMFileSystem::attach(const std::string &name, fnFile *handle)
{
    _handles[name] = handle;
}

void FnFileMFileSystem::detach(const std::string &name)
{
    _handles.erase(name);
}

fnFile *FnFileMFileSystem::handle(const std::string &name)
{
    auto entry = _handles.find(name);
    return entry == _handles.end() ? nullptr : entry->second;
}

bool FnFileMFileSystem::handles(std::string name)
{
    return mstr::equals(name, (char *)"fnfile:", false);
}

MFile* FnFileMFileSystem::getFile(std::string path)
{
    return new FnFileMFile(path);
}


/********************************************************
 * MFile implementations
 ********************************************************/

FnFileMFile::FnFileMFile(std::string path): MFile(path)
{
    fnFile *f = FnFileMFileSystem::handle(host);
    m_isNull = (f == nullptr);
    if (f != nullptr && fnio::fseek(f, 0, SEEK_END) == 0)
        size = fnio::ftell(f);
}

MStream* FnFileMFile::getSourceStream(std::ios_base::openmode mode)
{
    MStream* stream = createStream(mode);
    stream->open(mode);
    return stream;
}

MStream* FnFileMFile::getDecodedStream(std::shared_ptr<MStream> src)
{
    return src.get();
}

MStream* FnFileMFile::createStream(std::ios_base::openmode mode)
{
    MStream* stream = new FnFileMStream(host);
    stream->url = url;
    return stream;
}


/********************************************************
 * MStream implementations
 ********************************************************/

bool FnFileMStream::open(std::ios_base::openmode mode)
{
    fnFile *f = FnFileMFileSystem::handle(_name);
    if (f == nullptr || fnio::fseek(f, 0, SEEK_END) != 0)
        return false;

    _size = fnio::ftell(f);
    _position = 0;
    _open = true;
    return true;
}

uint32_t FnFileMStream::read(uint8_t* buf, uint32_t size)
{
    fnFile *f = FnFileMFileSystem::handle(_name);
    if (!_open || f == nullptr || buf == nullptr)
        return 0;

    if (size > available())
        size = available();
    if (size == 0 || fnio::fseek(f, _position, SEEK_SET) != 0)
        return 0;

    uint32_t bytesRead = fnio::fread(buf, 1, size, f);
    _position += bytesRead;
    return bytesRead;
}

uint32_t FnFileMStream::write(const uint8_t *buf, uint32_t size)
{
    fnFile *f = FnFileMFileSystem::handle(_name);
    if (!_open || f == nullptr || buf == nullptr || fnio::fseek(f, _position, SEEK_SET) != 0)
        return 0;

    uint32_t written = fnio::fwrite(buf, 1, size, f);
    _position += written;
    if (_position > _size)
        _size = _position;
    return written;
}

bool FnFileMStream::seek(uint32_t pos)
{
    if (!isOpen())
        return false;

    _position = pos;
    return true;
}
