/*
 * Copyright (C) 2015  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "backup.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <getopt.h>
#include <sys/mount.h>

#include <archive.h>
#include <archive_entry.h>

#include <libmbp/bootimage.h>
#include <libmbp/cpiofile.h>

#include "autoclose/archive.h"
#include "autoclose/dir.h"
#include "util/archive.h"
#include "util/copy.h"
#include "util/directory.h"
#include "util/file.h"
#include "util/finally.h"
#include "util/logging.h"
#include "util/mount.h"
#include "util/path.h"
#include "util/string.h"
#include "util/time.h"

#include "image.h"
#include "multiboot.h"
#include "roms.h"
#include "wipe.h"

#define BACKUP_MNT_DIR          "/mb_mnt"

namespace mb
{

#define BACKUP_TARGET_SYSTEM            0x1
#define BACKUP_TARGET_CACHE             0x2
#define BACKUP_TARGET_DATA              0x4
#define BACKUP_TARGET_BOOT              0x8
#define BACKUP_TARGET_CONFIG            0x10
#define BACKUP_TARGET_ALL               (BACKUP_TARGET_SYSTEM \
                                        | BACKUP_TARGET_CACHE \
                                        | BACKUP_TARGET_DATA \
                                        | BACKUP_TARGET_BOOT \
                                        | BACKUP_TARGET_CONFIG)

#define BACKUP_NAME_SYSTEM              "system.tar"
#define BACKUP_NAME_CACHE               "cache.tar"
#define BACKUP_NAME_DATA                "data.tar"
#define BACKUP_NAME_BOOT_IMAGE          "boot.img"
#define BACKUP_NAME_CONFIG              "config.json"
#define BACKUP_NAME_THUMBNAIL           "thumbnail.webp"

enum class Result
{
    SUCCEEDED,
    FAILED,
    FILES_MISSING,
    BOOT_IMAGE_UNPATCHED
};

static int parse_targets_string(const std::string &targets)
{
    std::vector<std::string> targets_list = util::split(targets, ",");
    int result = 0;

    for (const std::string &target : targets_list) {
        if (target == "all") {
            result |= BACKUP_TARGET_ALL;
        } else if (target == "system") {
            result |= BACKUP_TARGET_SYSTEM;
        } else if (target == "cache") {
            result |= BACKUP_TARGET_CACHE;
        } else if (target == "data") {
            result |= BACKUP_TARGET_DATA;
        } else if (target == "boot") {
            result |= BACKUP_TARGET_BOOT;
        } else if (target == "config") {
            result |= BACKUP_TARGET_CONFIG;
        } else {
            return 0;
        }
    }

    return result;
}

static bool backup_directory(const std::string &output_file,
                             const std::string &directory,
                             const std::vector<std::string> &exclusions)
{
    autoclose::dir dp(autoclose::opendir(directory.c_str()));
    if (!dp) {
        LOGE("%s: %s", directory.c_str(), strerror(errno));
        return false;
    }

    std::vector<std::string> contents;
    dirent *ent;
    errno = 0;

    while ((ent = readdir(dp.get()))) {
        if (strcmp(ent->d_name, ".") == 0
                || strcmp(ent->d_name, "..") == 0
                || std::find(exclusions.begin(), exclusions.end(), ent->d_name)
                        != exclusions.end()) {
            continue;
        }
        contents.push_back(ent->d_name);
    }

    if (errno) {
        LOGE("%s: %s", directory.c_str(), strerror(errno));
        return false;
    }

    return util::libarchive_tar_create(output_file, directory, contents);
}

static bool restore_directory(const std::string &input_file,
                              const std::string &directory,
                              const std::vector<std::string> &exclusions)
{
    if (!wipe_directory(directory, exclusions)) {
        return false;
    }

    return util::libarchive_tar_extract(input_file, directory, {});
}

static bool backup_image(const std::string &output_file,
                         const std::string &image,
                         const std::vector<std::string> &exclusions)
{
    if (!util::mkdir_recursive(BACKUP_MNT_DIR, 0755) && errno != EEXIST) {
        LOGE("%s: %s", BACKUP_MNT_DIR, strerror(errno));
        return false;
    }

    fsck_ext4_image(image);

    if (!util::mount(image.c_str(), BACKUP_MNT_DIR, "ext4", MS_RDONLY, "")) {
        LOGE("Failed to mount %s at %s: %s", image.c_str(), BACKUP_MNT_DIR,
             strerror(errno));
        return false;
    }

    bool ret = backup_directory(output_file, BACKUP_MNT_DIR, exclusions);

    if (!util::umount(BACKUP_MNT_DIR)) {
        LOGE("Failed to unmount %s: %s", BACKUP_MNT_DIR, strerror(errno));
        return false;
    }

    rmdir(BACKUP_MNT_DIR);

    return ret;
}

static bool restore_image(const std::string &input_file,
                          const std::string &image,
                          uint64_t size,
                          const std::vector<std::string> &exclusions)
{
    if (!util::mkdir_parent(image, S_IRWXU)) {
        LOGE("%s: Failed to create parent directory: %s",
             image.c_str(), strerror(errno));
        return false;
    }

    struct stat sb;
    if (stat(image.c_str(), &sb) < 0) {
        if (errno == ENOENT) {
            auto result = create_ext4_image(image, size);
            if (result != CreateImageResult::SUCCEEDED) {
                return false;
            }
        } else {
            LOGE("%s: Failed to stat: %s", image.c_str(), strerror(errno));
            return false;
        }
    }

    if (!util::mkdir_recursive(BACKUP_MNT_DIR, 0755) && errno != EEXIST) {
        LOGE("%s: %s", BACKUP_MNT_DIR, strerror(errno));
        return false;
    }

    fsck_ext4_image(image);

    if (!util::mount(image.c_str(), BACKUP_MNT_DIR, "ext4", 0, "")) {
        LOGE("Failed to mount %s at %s: %s", image.c_str(), BACKUP_MNT_DIR,
             strerror(errno));
        return false;
    }

    bool ret = restore_directory(input_file, BACKUP_MNT_DIR, exclusions);

    if (!util::umount(BACKUP_MNT_DIR)) {
        LOGE("Failed to unmount %s: %s", BACKUP_MNT_DIR, strerror(errno));
        return false;
    }

    rmdir(BACKUP_MNT_DIR);

    return ret;
}

/*!
 * \brief Backup boot image of a ROM
 *
 * \param rom ROM
 * \param backup_dir Backup directory
 *
 * \return Result::SUCCEEDED if the boot image was successfully backed up
 *         Result::FAILED if an error occured
 *         Result::FILES_MISSING if the boot image doesn't exist
 */
static Result backup_boot_image(const std::shared_ptr<Rom> &rom,
                                const std::string &backup_dir)
{
    std::string boot_image_path(rom->boot_image_path());
    std::string boot_image_backup(backup_dir);
    boot_image_backup += '/';
    boot_image_backup += BACKUP_NAME_BOOT_IMAGE;

    struct stat sb;
    if (stat(boot_image_path.c_str(), &sb) == 0) {
        LOGI("=== Backing up %s ===", boot_image_path.c_str());
        if (!util::copy_file(boot_image_path, boot_image_backup, 0)) {
            return Result::FAILED;
        }
    } else {
        LOGW("=== %s does not exist ===", boot_image_path.c_str());
        return Result::FILES_MISSING;
    }

    return Result::SUCCEEDED;
}

/*!
 * \brief Restore boot image for a ROM
 *
 * \param rom ROM
 * \param backup_dir Backup directory
 *
 * \return Result::SUCCEEDED if the boot image was successfully restored
 *         Result::FAILED if an error occured
 *         Result::FILES_MISSING if the boot image backup doesn't exist
 */
static Result restore_boot_image(const std::shared_ptr<Rom> &rom,
                                 const std::string &backup_dir)
{
    std::string boot_image_path(rom->boot_image_path());
    std::string boot_image_backup(backup_dir);
    boot_image_backup += '/';
    boot_image_backup += BACKUP_NAME_BOOT_IMAGE;

    struct stat sb;
    if (stat(boot_image_backup.c_str(), &sb) < 0) {
        LOGW("=== %s does not exist ===", boot_image_backup.c_str());
        return Result::FILES_MISSING;
    }

    LOGI("=== Restoring to %s ===", boot_image_path.c_str());

    // Set the ROM ID in the ramdisk
    mbp::BootImage bi;
    if (!bi.loadFile(boot_image_backup)) {
        LOGE("Failed to load boot image");
        return Result::FAILED;
    }
    mbp::CpioFile cpio;
    if (!cpio.load(bi.ramdiskImage())) {
        LOGE("Failed to load ramdisk image");
        return Result::FAILED;
    }

    // Set ROM ID
    std::vector<unsigned char> data(rom->id.begin(), rom->id.end());
    cpio.remove("romid");
    cpio.addFile(std::move(data), "romid", 0664);

    // Recreate ramdisk
    std::vector<unsigned char> new_ramdisk;
    if (!cpio.createData(&new_ramdisk)) {
        LOGE("Failed to create new ramdisk");
        return Result::FAILED;
    }
    bi.setRamdiskImage(std::move(new_ramdisk));

    // Re-loki if needed
    if (bi.wasType() == mbp::BootImage::Type::Loki) {
        std::vector<unsigned char> aboot_image;
        if (!util::file_read_all(ABOOT_PARTITION, &aboot_image)) {
            LOGE("Failed to read aboot partition: %s", strerror(errno));
            return Result::FAILED;
        }

        bi.setAbootImage(std::move(aboot_image));
        bi.setTargetType(mbp::BootImage::Type::Loki);
    }

    // Recreate boot image
    if (!bi.createFile(boot_image_path)) {
        LOGE("Failed to create new boot image");
        return Result::FAILED;
    }

    // We explicitly don't update the checksums here. The user needs to know the
    // risk of restoring a backup that can be modified by any app.

    return Result::SUCCEEDED;
}

/*!
 * \brief Backup configuration file and thumbnail for a ROM
 *
 * \param rom ROM
 * \param backup_dir Backup directory
 *
 * \return Result::SUCCEEDED if the configs were successfully backed up
 *         Result::FAILED if an error occured
 *         Result::FILES_MISSING if the configs don't exist
 */
static Result backup_configs(const std::shared_ptr<Rom> &rom,
                             const std::string &backup_dir)
{
    std::string config_path(rom->config_path());
    std::string thumbnail_path(rom->thumbnail_path());

    std::string config_backup(backup_dir);
    config_backup += '/';
    config_backup += BACKUP_NAME_CONFIG;
    std::string thumbnail_backup(backup_dir);
    thumbnail_backup += '/';
    thumbnail_backup += BACKUP_NAME_THUMBNAIL;

    Result ret = Result::SUCCEEDED;

    struct stat sb;
    if (stat(config_path.c_str(), &sb) == 0) {
        LOGI("=== Backing up %s ===", config_path.c_str());
        if (!util::copy_file(config_path, config_backup, 0)) {
            return Result::FAILED;
        }
    } else {
        LOGW("=== %s does not exist ===", config_path.c_str());
        ret = Result::FILES_MISSING;
    }
    if (stat(thumbnail_path.c_str(), &sb) == 0) {
        LOGI("=== Backing up %s ===", thumbnail_path.c_str());
        if (!util::copy_file(thumbnail_path, thumbnail_backup, 0)) {
            return Result::FAILED;
        }
    } else {
        LOGW("=== %s does not exist ===", thumbnail_path.c_str());
        ret = Result::FILES_MISSING;
    }

    return ret;
}

/*!
 * \brief Restore configuration file and thumbnail for a ROM
 *
 * \param rom ROM
 * \param backup_dir Backup directory
 *
 * \return Result::SUCCEEDED if the configs were successfully restored
 *         Result::FAILED if an error occured
 *         Result::FILES_MISSING if the backups of the configs don't exist
 */
static Result restore_configs(const std::shared_ptr<Rom> &rom,
                              const std::string &backup_dir)
{
    std::string config_path(rom->config_path());
    std::string thumbnail_path(rom->thumbnail_path());

    std::string config_backup(backup_dir);
    config_backup += '/';
    config_backup += BACKUP_NAME_CONFIG;
    std::string thumbnail_backup(backup_dir);
    thumbnail_backup += '/';
    thumbnail_backup += BACKUP_NAME_THUMBNAIL;

    Result ret = Result::SUCCEEDED;

    struct stat sb;
    if (stat(config_backup.c_str(), &sb) == 0) {
        LOGI("=== Restoring to %s ===", config_path.c_str());
        if (!util::copy_file(config_backup, config_path, 0)) {
            return Result::FAILED;
        }
    } else {
        LOGW("=== %s does not exist ===", config_backup.c_str());
        ret = Result::FILES_MISSING;
    }
    if (stat(thumbnail_backup.c_str(), &sb) == 0) {
        LOGI("=== Restoring to %s ===", thumbnail_path.c_str());
        if (!util::copy_file(thumbnail_backup, thumbnail_path, 0)) {
            return Result::FAILED;
        }
    } else {
        LOGW("=== %s does not exist ===", thumbnail_backup.c_str());
        ret = Result::FILES_MISSING;
    }

    return ret;
}

/*!
 * \brief Backup a partition for a ROM
 *
 * \param path Path to mountpoint/directory or image
 * \param backup_dir Backup directory
 * \param archive_name Backup archive name
 * \param is_image Whether \a path is an ext4 image
 * \param exclusions List of top-level directories to exclude from the backup
 *
 * \return Result::SUCCEEDED if the directory/image was successfully backed up
 *         Result::FAILED if an error occured
 *         Result::FILES_MISSING if \a path does not exist
 */
static Result backup_partition(const std::string &path,
                               const std::string &backup_dir,
                               const std::string &archive_name,
                               bool is_image,
                               const std::vector<std::string> &exclusions)
{
    std::string archive(backup_dir);
    archive += '/';
    archive += archive_name;

    bool ret = false;

    struct stat sb;
    if (stat(path.c_str(), &sb) == 0) {
        LOGI("=== Backing up %s ===", path.c_str());
        if (is_image) {
            ret = backup_image(archive, path, exclusions);
        } else {
            ret = backup_directory(archive, path, exclusions);
        }
    } else {
        LOGW("=== %s does not exist ===", path.c_str());
        return Result::FILES_MISSING;
    }

    return ret ? Result::SUCCEEDED : Result::FAILED;
}

/*!
 * \brief Restore a partition for a ROM
 *
 * \param path Path to mountpoint/directory or image
 * \param backup_dir Backup directory
 * \param archive_name Backup archive name
 * \param is_image Whether \a path is an ext4 image
 * \param exclusions List of top-level directories to exclude from the wipe
 *                   process before restoring
 *
 * \return Result::SUCCEEDED if the directory/image was successfully restored
 *         Result::FAILED if an error occured
 *         Result::FILES_MISSING if \a archive_name does not exist in
 *         \a backup_dir
 */
static Result restore_partition(const std::string &path,
                                const std::string &backup_dir,
                                const std::string &archive_name,
                                bool is_image,
                                uint64_t image_size,
                                const std::vector<std::string> &exclusions)
{
    std::string archive(backup_dir);
    archive += '/';
    archive += archive_name;

    bool ret = false;

    struct stat sb;
    if (stat(archive.c_str(), &sb) == 0) {
        LOGI("=== Restoring to %s ===", path.c_str());
        if (is_image) {
            ret = restore_image(archive, path, image_size, exclusions);
        } else {
            ret = restore_directory(archive, path, exclusions);
        }
    } else {
        LOGW("=== %s does not exist ===", archive.c_str());
        return Result::FILES_MISSING;
    }

    return ret ? Result::SUCCEEDED : Result::FAILED;
}

static bool backup_rom(const std::shared_ptr<Rom> &rom,
                       const std::string &output_dir, int targets)
{
    if (!targets) {
        LOGE("No backup targets specified");
        return false;
    }

    const std::string system_path(rom->full_system_path());
    const std::string cache_path(rom->full_cache_path());
    const std::string data_path(rom->full_data_path());
    const std::string boot_image_path(rom->boot_image_path());
    const std::string config_path(rom->config_path());
    const std::string thumbnail_path(rom->thumbnail_path());

    LOGI("Backing up:");
    LOGI("- ROM ID: %s", rom->id.c_str());
    LOGI("- Targets:");
    if (targets & BACKUP_TARGET_SYSTEM) {
        LOGI("  - System: %s", system_path.c_str());
    }
    if (targets & BACKUP_TARGET_CACHE) {
        LOGI("  - Cache: %s", cache_path.c_str());
    }
    if (targets & BACKUP_TARGET_DATA) {
        LOGI("  - Data: %s", data_path.c_str());
    }
    if (targets & BACKUP_TARGET_BOOT) {
        LOGI("  - Boot image: %s", boot_image_path.c_str());
    }
    if (targets & BACKUP_TARGET_CONFIG) {
        LOGI("  - Configs: %s", config_path.c_str());
        LOGI("             %s", thumbnail_path.c_str());
    }
    LOGI("- Backup directory: %s", output_dir.c_str());

    // Backup boot image
    if (targets & BACKUP_TARGET_BOOT
            && backup_boot_image(rom, output_dir) == Result::FAILED) {
        return false;
    }

    // Backup configs
    if (targets & BACKUP_TARGET_CONFIG
            && backup_configs(rom, output_dir) == Result::FAILED) {
        return false;
    }

    // Backup system
    if (targets & BACKUP_TARGET_SYSTEM) {
        Result ret = backup_partition(
                system_path, output_dir, BACKUP_NAME_SYSTEM,
                rom->system_is_image, { "multiboot" });
        if (ret == Result::FAILED) {
            return false;
        }
    }

    // Backup cache
    if (targets & BACKUP_TARGET_CACHE) {
        Result ret = backup_partition(
                cache_path, output_dir, BACKUP_NAME_CACHE,
                rom->cache_is_image, { "multiboot" });
        if (ret == Result::FAILED) {
            return false;
        }
    }

    // Backup data
    if (targets & BACKUP_TARGET_DATA) {
        Result ret = backup_partition(
                data_path, output_dir, BACKUP_NAME_DATA,
                rom->data_is_image, { "media", "multiboot" });
        if (ret == Result::FAILED) {
            return false;
        }
    }

    return true;
}

static bool restore_rom(const std::shared_ptr<Rom> &rom,
                        const std::string &input_dir, int targets)
{
    if (!targets) {
        LOGE("No restore targets specified");
        return false;
    }

    const std::string system_path(rom->full_system_path());
    const std::string cache_path(rom->full_cache_path());
    const std::string data_path(rom->full_data_path());
    const std::string boot_image_path(rom->boot_image_path());
    const std::string config_path(rom->config_path());
    const std::string thumbnail_path(rom->thumbnail_path());

    LOGI("Restoring:");
    LOGI("- ROM ID: %s", rom->id.c_str());
    LOGI("- Targets:");
    if (targets & BACKUP_TARGET_SYSTEM) {
        LOGI("  - System: %s", system_path.c_str());
    }
    if (targets & BACKUP_TARGET_CACHE) {
        LOGI("  - Cache: %s", cache_path.c_str());
    }
    if (targets & BACKUP_TARGET_DATA) {
        LOGI("  - Data: %s", data_path.c_str());
    }
    if (targets & BACKUP_TARGET_BOOT) {
        LOGI("  - Boot image: %s", boot_image_path.c_str());
    }
    if (targets & BACKUP_TARGET_CONFIG) {
        LOGI("  - Configs: %s", config_path.c_str());
        LOGI("             %s", thumbnail_path.c_str());
    }
    LOGI("- Backup directory: %s", input_dir.c_str());

    std::string multiboot_dir(MULTIBOOT_DIR);
    multiboot_dir += '/';
    multiboot_dir += rom->id;
    if (!util::mkdir_recursive(multiboot_dir, 0775)) {
        LOGE("%s: Failed to create directory: %s",
             multiboot_dir.c_str(), strerror(errno));
        return false;
    }

    // Restore boot image
    if (targets & BACKUP_TARGET_BOOT
            && restore_boot_image(rom, input_dir) == Result::FAILED) {
        return false;
    }

    // Restore configs
    if (targets & BACKUP_TARGET_CONFIG
            && restore_configs(rom, input_dir) == Result::FAILED) {
        return false;
    }

    fix_multiboot_permissions();

    // Restore system
    if (targets & BACKUP_TARGET_SYSTEM) {
        uint64_t image_size = util::mount_get_total_size(
                Roms::get_system_partition().c_str());
        if (image_size == 0) {
            LOGE("Failed to get the size of the system partition");
            return false;
        }

        Result ret = restore_partition(
                system_path, input_dir, BACKUP_NAME_SYSTEM,
                rom->system_is_image, image_size, {});
        if (ret == Result::FAILED) {
            return false;
        }
    }

    // Restore cache
    if (targets & BACKUP_TARGET_CACHE) {
        Result ret = restore_partition(
                cache_path, input_dir, BACKUP_NAME_CACHE,
                rom->cache_is_image, DEFAULT_IMAGE_SIZE, {});
        if (ret == Result::FAILED) {
            return false;
        }
    }

    // Restore data
    if (targets & BACKUP_TARGET_DATA) {
        Result ret = restore_partition(
                data_path, input_dir, BACKUP_NAME_DATA,
                rom->data_is_image, DEFAULT_IMAGE_SIZE, { "media" });
        if (ret == Result::FAILED) {
            return false;
        }
    }

    return true;
}

static bool ensure_partitions_mounted(void)
{
    std::string system_partition(Roms::get_system_partition());
    std::string cache_partition(Roms::get_cache_partition());
    std::string data_partition(Roms::get_data_partition());

    if (system_partition.empty() || !util::is_mounted(system_partition)) {
        fprintf(stderr, "System partition is not mounted\n");
        return false;
    }
    if (cache_partition.empty() || !util::is_mounted(cache_partition)) {
        fprintf(stderr, "Cache partition is not mounted\n");
        return false;
    }
    if (data_partition.empty() || !util::is_mounted(data_partition)) {
        fprintf(stderr, "Data partition is not mounted\n");
        return false;
    }

    return true;
}

static bool is_valid_backup_name(const std::string &name)
{
    // No empty strings, hidden paths, '..', or directory separators
    return !name.empty()
            && name[0] != '.'
            && name.find("..") == std::string::npos
            && name.find('/') == std::string::npos;
}

static void backup_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: backup -r <romid> -t <targets> [-n <name>] [OPTION...]\n\n"
            "Options:\n"
            "  -r, --romid      ROM ID to backup\n"
            "  -t, --targets    Comma-separated list of targets to backup\n"
            "                   (Default: 'all')\n"
            "  -n, --name       Name of backup\n"
            "                   (Default: YYYY.MM.DD-HH.MM.SS)\n"
            "  -d, --backupdir  Directory to store backups\n"
            "                   (Default: " MULTIBOOT_BACKUP_DIR ")\n"
            "  -f, --force      Allow overwriting old backup with the same name\n"
            "  -h, --help       Display this help message\n"
            "\n"
            "Valid backup targets: 'all' or some combination of the following:\n"
            "  system,cache,data,boot,config\n"
            "\n"
            "NOTE: This tool is still in development and the arguments above\n"
            "have not yet been finalized.\n");
}

static void restore_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: restore -r <romid> -t <targets> -n <name> [OPTION...]\n\n"
            "Options:\n"
            "  -r, --romid      ROM ID to restore to\n"
            "  -t, --targets    Comma-separated list of targets to restore\n"
            "                   (Default: 'all')\n"
            "  -n, --name       Name of backup to restore\n"
            "  -d, --backupdir  Directory containing backups\n"
            "                   (Default: " MULTIBOOT_BACKUP_DIR ")\n"
            "  -h, --help       Display this help message\n"
            "\n"
            "Valid backup targets: 'all' or some combination of the following:\n"
            "  system,cache,data,boot,config\n"
            "\n"
            "NOTE: This tool is still in development and the arguments above\n"
            "have not yet been finalized.\n");
}

int backup_main(int argc, char *argv[])
{
    int opt;

    static const char *short_options = "r:t:n:d:fh";
    static struct option long_options[] = {
        {"romid",     required_argument, 0, 'r'},
        {"targets",   required_argument, 0, 't'},
        {"name",      required_argument, 0, 'n'},
        {"backupdir", required_argument, 0, 'd'},
        {"force",     no_argument,       0, 'f'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int long_index = 0;

    std::string romid;
    std::string targets_str("all");
    std::string name;
    std::string backupdir(MULTIBOOT_BACKUP_DIR);
    bool force = false;

    if (!util::format_time("%Y.%m.%d-%H.%M.%S", &name)) {
        fprintf(stderr, "Failed to format current time\n");
        return false;
    }

    while ((opt = getopt_long(argc, argv, short_options,
            long_options, &long_index)) != -1) {
        switch (opt) {
        case 'r':
            romid = optarg;
            break;
        case 't':
            targets_str = optarg;
            break;
        case 'n':
            name = optarg;
            break;
        case 'd':
            backupdir = optarg;
            break;
        case 'f':
            force = true;
            break;
        case 'h':
            backup_usage(stdout);
            return EXIT_SUCCESS;
        default:
            backup_usage(stderr);
            return EXIT_FAILURE;
        }
    }

    // There should be no other arguments
    if (argc - optind != 0) {
        backup_usage(stderr);
        return EXIT_FAILURE;
    }

    if (romid.empty()) {
        fprintf(stderr, "No ROM ID specified\n");
        return EXIT_FAILURE;
    }

    int targets = parse_targets_string(targets_str);
    if (!targets) {
        fprintf(stderr, "Invalid targets: %s\n", targets_str.c_str());
        return EXIT_FAILURE;
    }

    if (!is_valid_backup_name(name)) {
        fprintf(stderr, "Invalid backup name: %s\n", name.c_str());
        return EXIT_FAILURE;
    }

    if (!ensure_partitions_mounted()) {
        return EXIT_FAILURE;
    }

    Roms roms;
    roms.add_installed();

    auto rom = roms.find_by_id(romid);
    if (!rom) {
        fprintf(stderr, "ROM '%s' is not installed\n", romid.c_str());
        return EXIT_FAILURE;
    }

    std::string output_dir(backupdir);
    output_dir += "/";
    output_dir += name;

    struct stat sb;
    if (!force && stat(output_dir.c_str(), &sb) == 0) {
        fprintf(stderr, "Backup '%s' already exists. Choose another name or "
                "pass -f/--force to use this name anyway.\n", name.c_str());
        return EXIT_FAILURE;
    }

    if (!util::mkdir_recursive(output_dir, 0755)) {
        fprintf(stderr, "%s: %s\n", output_dir.c_str(), strerror(errno));
        return EXIT_FAILURE;
    }

    bool ret = backup_rom(rom, output_dir, targets);
    if (ret) {
        LOGI("=== Finished ===");
        return EXIT_SUCCESS;
    } else {
        LOGI("=== Failed ===");
        return EXIT_FAILURE;
    }
}

int restore_main(int argc, char *argv[])
{
    int opt;

    static const char *short_options = "r:t:n:d:h";
    static struct option long_options[] = {
        {"romid",     required_argument, 0, 'r'},
        {"targets",   required_argument, 0, 't'},
        {"name",      required_argument, 0, 'n'},
        {"backupdir", required_argument, 0, 'd'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int long_index = 0;

    std::string romid;
    std::string targets_str("all");
    std::string name;
    std::string backupdir(MULTIBOOT_BACKUP_DIR);

    while ((opt = getopt_long(argc, argv, short_options,
            long_options, &long_index)) != -1) {
        switch (opt) {
        case 'r':
            romid = optarg;
            break;
        case 't':
            targets_str = optarg;
            break;
        case 'n':
            name = optarg;
            break;
        case 'd':
            backupdir = optarg;
            break;
        case 'h':
            restore_usage(stdout);
            return EXIT_SUCCESS;
        default:
            restore_usage(stderr);
            return EXIT_FAILURE;
        }
    }

    // There should be no other arguments
    if (argc - optind != 0) {
        restore_usage(stderr);
        return EXIT_FAILURE;
    }

    if (romid.empty()) {
        fprintf(stderr, "No ROM ID specified\n");
        return EXIT_FAILURE;
    }

    if (name.empty()) {
        fprintf(stderr, "No backup name specified\n");
        return EXIT_FAILURE;
    }

    int targets = parse_targets_string(targets_str);
    if (!targets) {
        fprintf(stderr, "Invalid targets: %s\n", targets_str.c_str());
        return EXIT_FAILURE;
    }

    if (!is_valid_backup_name(name)) {
        fprintf(stderr, "Invalid backup name: %s\n", name.c_str());
        return EXIT_FAILURE;
    }

    if (!ensure_partitions_mounted()) {
        return EXIT_FAILURE;
    }

    auto rom = Roms::create_rom(romid);
    if (!rom) {
        fprintf(stderr, "Invalid ROM ID: '%s'\n", romid.c_str());
        return EXIT_FAILURE;
    }

    std::string input_dir(backupdir);
    input_dir += "/";
    input_dir += name;

    struct stat sb;
    if (stat(input_dir.c_str(), &sb) < 0) {
        fprintf(stderr, "Backup '%s' does not exist\n", name.c_str());
        return EXIT_FAILURE;
    }

    bool ret = restore_rom(rom, input_dir, targets);
    if (ret) {
        LOGI("=== Finished ===");
        return EXIT_SUCCESS;
    } else {
        LOGI("=== Failed ===");
        return EXIT_FAILURE;
    }
}

}