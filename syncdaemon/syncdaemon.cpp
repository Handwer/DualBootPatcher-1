/*
 * Copyright (C) 2014  Xiao-Long Chen <chenxiaolong@cxl.epac.to>
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <inotify-cxx.h>

#include "common.h"
#include "configfile.h"
#include "task.h"


Inotify *notify = nullptr;

// Delay sync for 30 seconds to avoid running multiple times when there are
// a ton of inotify events
SingleDelayedTask task(30);
std::thread *task_thread = nullptr;

struct rominformation {
    // Mount points
    std::string system;
    std::string cache;
    std::string data;

    // Identifiers
    std::string id;
};

struct apkinformation {
    std::string apkfile;
    std::string apkdir;
    std::string libdir;
    std::string cacheddex;
};

std::vector<struct rominformation *> roms;

// std::to_string is not available on Android
template <typename T> std::string to_string(T value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

template <typename T> void free_vector(std::vector<T> const& vector) {
    for (unsigned int i = 0; i < vector.size(); i++) {
        delete vector[i];
    }
}

bool is_booted_in_primary() {
    return !exists_directory(RAW_SYSTEM)
           || is_same_inode(RAW_SYSTEM + SEP + BUILD_PROP,
                            SYSTEM + SEP + BUILD_PROP);
}

void populate_roms() {
    struct rominformation *info = nullptr;

    if (is_booted_in_primary()) {
        info = new rominformation();

        info->system = SYSTEM;
        info->cache = CACHE;
        info->data = DATA;

        info->id = PRIMARY_ID;

        roms.push_back(info);
    } else if (exists_file(RAW_SYSTEM + SEP + BUILD_PROP)) {
        info = new rominformation();

        info->system = RAW_SYSTEM;
        info->cache = RAW_CACHE;
        info->data = RAW_DATA;

        info->id = PRIMARY_ID;

        roms.push_back(info);
    }

    if (exists_file(RAW_SYSTEM + SEP + SECONDARY_ID + SEP + BUILD_PROP)) {
        info = new rominformation();

        info->system = RAW_SYSTEM + SEP + SECONDARY_ID;
        info->cache = RAW_CACHE + SEP + SECONDARY_ID;
        info->data = RAW_DATA + SEP + SECONDARY_ID;

        info->id = SECONDARY_ID;

        roms.push_back(info);
    } else if (exists_file(SYSTEM + SEP + SECONDARY_ID + SEP + BUILD_PROP)) {
        info = new rominformation();

        info->system = SYSTEM + SEP + SECONDARY_ID;
        info->cache = CACHE + SEP + SECONDARY_ID;
        info->data = DATA + SEP + SECONDARY_ID;

        info->id = SECONDARY_ID;

        roms.push_back(info);
    }

    int max = 10;
    for (int i = 0; i < max; i++) {
        std::string id = MULTI_ID_PREFIX + to_string(i);
        std::string raw_system_path = RAW_CACHE + SEP + id + SYSTEM;
        std::string raw_cache_path = RAW_SYSTEM + SEP + id + CACHE;
        std::string raw_data_path = RAW_DATA + SEP + id;
        std::string system_path = CACHE + SEP + id + SYSTEM;
        std::string cache_path = SYSTEM + SEP + id + CACHE;
        std::string data_path = DATA + SEP + id;

        if (exists_directory(raw_system_path)) {
            info = new rominformation();

            info->system = raw_system_path;
            info->cache = raw_cache_path;
            info->data = raw_data_path;

            info->id = id;

            roms.push_back(info);
        } else if (exists_directory(system_path)) {
            info = new rominformation();

            info->system = system_path;
            info->cache = cache_path;
            info->data = data_path;

            info->id = id;

            roms.push_back(info);
        }
    }

    for (unsigned int i = 0; i < roms.size(); i++) {
        LOGV("Discovered ROM ID %s", roms[i]->id.c_str());
        LOGV("- System: %s", roms[i]->system.c_str());
        LOGV("- Cache: %s", roms[i]->cache.c_str());
        LOGV("- Data: %s", roms[i]->data.c_str());
    }
}

std::string get_current_rom() {
    for (unsigned int i = 0; i < roms.size(); i++) {
        if (is_same_inode(SYSTEM + SEP + BUILD_PROP,
                roms[i]->system + SEP + BUILD_PROP)) {
            return roms[i]->id;
        }
    }

    return "";
}

int get_apk_information(struct rominformation *info, std::string package,
                        struct apkinformation *apkinfo) {
    // Find the apk path
    std::string appdir = info->data + SEP + APP_DIR;

    std::string name = search_directory(appdir, package + "-");
    if (name.empty()) {
        return -1;
    }

    apkinfo->apkfile = appdir + SEP + name;
    apkinfo->apkdir = appdir;

    // Find the native shared library path
    std::string libdir = info->data + SEP + APP_LIB_DIR;

    name = search_directory(libdir, package + "-");
    if (!name.empty()) {
        apkinfo->libdir = libdir + SEP + name;
    }

    // Find the dex cache file
    std::string dexcachedir1 = info->data + SEP + DEX_CACHE_DIR;
    std::string dexcachedir2 = info->cache + SEP + DEX_CACHE_DIR;
    std::string dexname1 = search_directory(dexcachedir1,
            DEX_CACHE_PREFIX + package + "-");
    std::string dexname2 = search_directory(dexcachedir2,
            DEX_CACHE_PREFIX + package + "-");

    if (!dexname1.empty()) {
        apkinfo->cacheddex = dexcachedir1 + SEP + dexname1;
    } else if (!dexname2.empty()) {
        apkinfo->cacheddex = dexcachedir2 + SEP + dexname2;
    }

    return 0;
}

void sync_package(std::string package, std::vector<std::string> rom_ids) {
    std::string ids;
    for (unsigned int i = 0; i < rom_ids.size(); i++) {
        if (i == rom_ids.size() - 1) {
            ids += rom_ids[i];
        } else {
            ids += rom_ids[i] + ", ";
        }
    }
    LOGV("Attempting to share %s across %s", package.c_str(), ids.c_str());

    std::vector<struct apkinformation *> apkinfos;
    // Android's time_t isn't quite correct
    // time_t -> __kernel_time_t -> long
    //time_t ts_latest = 0;
    unsigned long ts_latest = 0;
    struct rominformation *latest = nullptr;
    struct apkinformation *latestapk = nullptr;

    for (unsigned int i = 0; i < rom_ids.size(); i++) {
        struct rominformation *info = nullptr;

        for (unsigned int j = 0; j < roms.size(); j++) {
            if (roms[j]->id == rom_ids[i]) {
                info = roms[j];
            }
        }

        if (info == nullptr) {
            LOGE("ROM ID %s in config for %s does not exist",
                    rom_ids[i].c_str(), package.c_str());

            // Remove ID for non-existant ROM
            //LOGD("Removing %s from config", rom_ids[i].c_str());
            //ConfigFile::remove_rom_id(package, rom_ids[i]);

            continue;
        }

        struct apkinformation *apkinfo = new apkinformation();
        if (get_apk_information(info, package, apkinfo) != 0) {
            // Copy package to other ROMs if it's not already installed
            apkinfo->apkfile = "";
            apkinfo->apkdir = info->data + SEP + APP_DIR;
            apkinfo->libdir = "";
            apkinfo->cacheddex = "";
            LOGV("%s does not exist in %s", package.c_str(), info->id.c_str());
        }

        // Record timestamp of latest version
        if (!apkinfo->apkfile.empty()) {
            struct stat s;
            if (stat(apkinfo->apkfile.c_str(), &s) != 0) {
                delete apkinfo;
                continue;
            }

            if (s.st_mtime > ts_latest) {
                ts_latest = s.st_mtime;
                latest = info;
                latestapk = apkinfo;
            }
        }

        apkinfos.push_back(apkinfo);
    }

    if (latestapk == nullptr) {
        LOGE("Package %s is not installed!", package.c_str());
        free_vector(apkinfos);
        return;
    }

    LOGV("  - Latest version is in ROM: %s", latest->id.c_str());

    for (unsigned int i = 0; i < apkinfos.size(); i++) {
        if (apkinfos[i] == latestapk) {
            continue;
        }

        // Keep filename of latest version
        // Android's basename and dirname in libgen.h seems to have a memleak
        std::string targetname = basename2(latestapk->apkfile);
        std::string targetapk = apkinfos[i]->apkdir + "/" + targetname;

        LOGV("  - Source: %s", latestapk->apkfile.c_str());
        LOGV("  - Target: %s", targetapk.c_str());

        if (is_same_inode(latestapk->apkfile, apkinfos[i]->apkfile)) {
            LOGW("  - Skipping because inodes are the same");
            continue;
        }

        // Remove existing version from other ROMs
        if (!apkinfos[i]->apkfile.empty()
                && remove(apkinfos[i]->apkfile.c_str()) != 0) {
            LOGE("Failed to remove apk %s", apkinfos[i]->apkfile.c_str());
        }

        if (!apkinfos[i]->libdir.empty()
                && recursively_delete(apkinfos[i]->libdir) != 0) {
            LOGE("Failed to remove native library directory %s",
                    apkinfos[i]->libdir.c_str());
        }

        if (!apkinfos[i]->cacheddex.empty()
                && remove(apkinfos[i]->cacheddex.c_str()) != 0) {
            LOGE("Failed to remove cached dex %s",
                    apkinfos[i]->cacheddex.c_str());
        }

        if (link(latestapk->apkfile.c_str(), targetapk.c_str()) != 0) {
            LOGE("Failed to hard link %s to %s", latestapk->apkfile.c_str(),
                    targetapk.c_str());
        }

        LOGV("  - Successfully shared package");
    }

    free_vector(apkinfos);
}

void sync_packages() {
    if (roms.size() == 0) {
        populate_roms();
    }

    LOGD("Reloading configuration file");

    if (!ConfigFile::load_config()) {
        LOGE("Failed to load configuration file");
        return;
    }

    std::vector<std::string> packages = ConfigFile::get_packages();
    for (unsigned int i = 0; i < packages.size(); i++) {
        std::string package = packages[i];
        std::vector<std::string> rom_ids = ConfigFile::get_rom_ids(package);
        // TODO: Share data in the future?

        sync_package(package, rom_ids);
    }
}

void cleanup(int) {
    LOGV("Cleaning up ...");

    if (notify != nullptr) {
        notify->Close();
    }

    if (task_thread != nullptr) {
        task.kill();
        task_thread->join();
    }

    for (unsigned int i = 0; i < roms.size(); i++) {
        delete roms[i];
    }
}

void setup_signals() {
    struct sigaction sa;
    sa.sa_handler = cleanup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int start_monitoring() {
    // Make sure packages are synced once
    sync_packages();

    try {
        notify = new Inotify();

        std::string appDir = DATA + SEP + APP_DIR;
        std::string configDir = ConfigFile::get_config_dir();

        InotifyWatch appDirWatch(appDir, IN_CREATE | IN_DELETE);
        InotifyWatch configDirWatch(configDir, IN_CREATE | IN_MODIFY);

        notify->Add(appDirWatch);
        notify->Add(configDirWatch);

        while (true) {
            notify->WaitForEvents();

            size_t count = notify->GetEventCount();
            while (count > 0) {
                InotifyEvent event;
                bool gotEvent = notify->GetEvent(&event);

                if (gotEvent) {
                    if (event.GetWatch() == &configDirWatch
                            && event.GetName() == CONFIG_FILE) {
                        LOGV("Config file was updated");
                        task.execute();
                    } else if (event.GetWatch() == &appDirWatch) {
                        std::string name(event.GetName());

                        if (ends_with(name, ".apk")) {
                            size_t pos = name.rfind("-");
                            if (pos != std::string::npos) {
                                name.erase(pos);
                            }

                            if (ConfigFile::contains_package(name)) {
                                if (event.GetMask() & IN_CREATE) {
                                    LOGD("%s was created", name.c_str());
                                } else if (event.GetMask() & IN_DELETE) {
                                    LOGD("%s was deleted", name.c_str());
                                }

                                // Instead of dealing with the inotify events during
                                // an upgrade, we'll delay the syncing
                                task.execute();
                            } else {
                                LOGV("%s is not shared", name.c_str());
                            }
                        }
                    }
                }

                count--;
            }
        }
    } catch (InotifyException &e) {
        LOGE("Inotify exception occurred: %s", e.GetMessage().c_str());
    } catch (std::exception &e) {
        LOGE("STL exception occurred: %s", e.what());
    } catch (...) {
        LOGE("Unknown exception occurred");
    }

    return -1;
}

void delayed_task_thread(SingleDelayedTask& t) {
    while (t.wait()) {
        sync_packages();
    }
}

int begin() {
    // Setup thread for future syncs
    std::thread thread(std::bind(delayed_task_thread, std::ref(task)));
    task_thread = &thread;

    return start_monitoring();
}

void write_pid(int fd) {
    std::ostringstream ss;
    ss << "pid=" << getpid() << std::endl;
    ss << "version=" << VERSION << std::endl;
    std::string text = ss.str();
    write(fd, text.c_str(), text.size());
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        std::cout << VERSION << std::endl;
        return EXIT_SUCCESS;
    }

    if (geteuid() != 0) {
        LOGE("syncdaemon needs to be run as root");
        return EXIT_FAILURE;
    }

    int pid_fd = open("/data/local/tmp/syncdaemon.pid", O_CREAT | O_RDWR, 0644);
    int rc = flock(pid_fd, LOCK_EX | LOCK_NB);
    if (rc < 0 && errno == EWOULDBLOCK) {
        close(pid_fd);
        LOGV("Another instance of syncdaemon is already running");
        return EXIT_FAILURE;
    }

    // Android's open() doesn't work properly...
    // https://github.com/android/platform_bionic/blob/master/libc/include/fcntl.h
    chmod("/data/local/tmp/syncdaemon.pid", 0644);

    // Clear file
    ftruncate(pid_fd, 0);

    write_pid(pid_fd);
    close(pid_fd);
    setup_signals();

    if (argc > 1 && strcmp(argv[1], "--runonce") == 0) {
        sync_packages();
        return EXIT_SUCCESS;
    } else {
        return begin() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
}