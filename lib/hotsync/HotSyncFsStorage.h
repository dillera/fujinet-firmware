#ifndef HOTSYNC_FS_STORAGE_H
#define HOTSYNC_FS_STORAGE_H

#include "HotSyncStorage.h"

class FileSystem;

// HotSyncStorage on a FujiNet FileSystem (the SD card), rooted at a folder
// such as "/palm".
class HotSyncFsStorage : public HotSyncStorage
{
public:
    HotSyncFsStorage(FileSystem &fs, std::string root) : _fs(fs), _root(std::move(root)) {}

    // Creates install/ up front, so a file can be queued into it by copying.
    success_is_true create_install_folder();

    std::vector<std::string> pending_installs() override;
    success_is_true read_install(const std::string &file_name, ByteBuffer &out) override;
    success_is_true mark_installed(const std::string &file_name) override;
    success_is_true write_backup(const std::string &user, const std::string &file_name,
                                 const ByteBuffer &data) override;
    success_is_true read_state(const std::string &user, const std::string &name,
                               ByteBuffer &out) override;
    success_is_true write_state(const std::string &user, const std::string &name,
                                const ByteBuffer &data) override;

private:
    success_is_true read_file(const std::string &file, ByteBuffer &out);
    success_is_true write_file(const std::string &dir, const std::string &file_name,
                               const ByteBuffer &data);
    std::string path(const std::string &folder, const std::string &file_name = "") const;
    success_is_true ensure_dir(const std::string &dir);

    FileSystem &_fs;
    std::string _root;
};

#endif // HOTSYNC_FS_STORAGE_H
