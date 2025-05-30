/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "fastboot.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <android-base/endian.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/parseint.h>
#include <android-base/parsenetaddress.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <build/version.h>
#include <libavb/libavb.h>
#include <liblp/liblp.h>
#include <liblp/super_layout_builder.h>
#include <platform_tools_version.h>
#include <sparse/sparse.h>
#include <zip.h>

#include "bootimg_utils.h"
#include "constants.h"
#include "diagnose_usb.h"
#include "fastboot_driver.h"
#include "fastboot_driver_interface.h"
#include "fs.h"
#include "storage.h"
#include "task.h"
#include "tcp.h"
#include "transport.h"
#include "udp.h"
#include "usb.h"
#include "util.h"
#include "vendor_boot_img_utils.h"

using android::base::borrowed_fd;
using android::base::ReadFully;
using android::base::Split;
using android::base::Trim;
using android::base::unique_fd;
using namespace std::placeholders;

#define FASTBOOT_INFO_VERSION 1

static const char* serial = nullptr;

static bool g_long_listing = false;
// Don't resparse files in too-big chunks.
// libsparse will support INT_MAX, but this results in large allocations, so
// let's keep it at 1GB to avoid memory pressure on the host.
static constexpr int64_t RESPARSE_LIMIT = 1 * 1024 * 1024 * 1024;
static int64_t target_sparse_limit = -1;

static unsigned g_base_addr = 0x10000000;
static boot_img_hdr_v2 g_boot_img_hdr = {};
static std::string g_cmdline;
static std::string g_dtb_path;

static bool g_disable_verity = false;
static bool g_disable_verification = false;

fastboot::FastBootDriver* fb = nullptr;

static std::vector<Image> images = {
        // clang-format off
    { "boot",     "boot.img",         "boot.sig",     "boot",     false, ImageType::BootCritical },
    { "bootloader",
                  "bootloader.img",   "",             "bootloader",
                                                                  true,  ImageType::Extra },
    { "init_boot",
                  "init_boot.img",    "init_boot.sig",
                                                      "init_boot",
                                                                  true,  ImageType::BootCritical },
    { "",    "boot_other.img",   "boot.sig",     "boot",     true,  ImageType::Normal },
    { "cache",    "cache.img",        "cache.sig",    "cache",    true,  ImageType::Extra },
    { "dtbo",     "dtbo.img",         "dtbo.sig",     "dtbo",     true,  ImageType::BootCritical },
    { "dts",      "dt.img",           "dt.sig",       "dts",      true,  ImageType::BootCritical },
    { "odm",      "odm.img",          "odm.sig",      "odm",      true,  ImageType::Normal },
    { "odm_dlkm", "odm_dlkm.img",     "odm_dlkm.sig", "odm_dlkm", true,  ImageType::Normal },
    { "product",  "product.img",      "product.sig",  "product",  true,  ImageType::Normal },
    { "pvmfw",    "pvmfw.img",        "pvmfw.sig",    "pvmfw",    true,  ImageType::BootCritical },
    { "radio",    "radio.img",        "",             "radio",    true,  ImageType::Extra },
    { "recovery", "recovery.img",     "recovery.sig", "recovery", true,  ImageType::BootCritical },
    { "super",    "super.img",        "super.sig",    "super",    true,  ImageType::Extra },
    { "system",   "system.img",       "system.sig",   "system",   false, ImageType::Normal },
    { "system_dlkm",
                  "system_dlkm.img",  "system_dlkm.sig",
                                                      "system_dlkm",
                                                                  true,  ImageType::Normal },
    { "system_ext",
                  "system_ext.img",   "system_ext.sig",
                                                      "system_ext",
                                                                  true,  ImageType::Normal },
    { "",    "system_other.img", "system.sig",   "system",   true,  ImageType::Normal },
    { "userdata", "userdata.img",     "userdata.sig", "userdata", true,  ImageType::Extra },
    { "vbmeta",   "vbmeta.img",       "vbmeta.sig",   "vbmeta",   true,  ImageType::BootCritical },
    { "vbmeta_system",
                  "vbmeta_system.img",
                                      "vbmeta_system.sig",
                                                      "vbmeta_system",
                                                                  true,  ImageType::BootCritical },
    { "vbmeta_vendor",
                  "vbmeta_vendor.img",
                                      "vbmeta_vendor.sig",
                                                      "vbmeta_vendor",
                                                                  true,  ImageType::BootCritical },
    { "vendor",   "vendor.img",       "vendor.sig",   "vendor",   true,  ImageType::Normal },
    { "vendor_boot",
                  "vendor_boot.img",  "vendor_boot.sig",
                                                      "vendor_boot",
                                                                  true,  ImageType::BootCritical },
    { "vendor_dlkm",
                  "vendor_dlkm.img",  "vendor_dlkm.sig",
                                                      "vendor_dlkm",
                                                                  true,  ImageType::Normal },
    { "vendor_kernel_boot",
                  "vendor_kernel_boot.img",
                                      "vendor_kernel_boot.sig",
                                                      "vendor_kernel_boot",
                                                                  true,  ImageType::BootCritical },
    { "",    "vendor_other.img", "vendor.sig",   "vendor",   true,  ImageType::Normal },
        // clang-format on
};

char* get_android_product_out() {
    char* dir = getenv("ANDROID_PRODUCT_OUT");
    if (dir == nullptr || dir[0] == '\0') {
        return nullptr;
    }
    return dir;
}

static std::string find_item_given_name(const std::string& img_name) {
    char* dir = get_android_product_out();
    if (!dir) {
        die("ANDROID_PRODUCT_OUT not set");
    }
    return std::string(dir) + "/" + img_name;
}

std::string find_item(const std::string& item) {
    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i].nickname.empty() && item == images[i].nickname) {
            return find_item_given_name(images[i].img_name);
        }
    }

    fprintf(stderr, "unknown partition '%s'\n", item.c_str());
    return "";
}

double last_start_time;

static void Status(const std::string& message) {
    if (!message.empty()) {
        static constexpr char kStatusFormat[] = "%-50s ";
        fprintf(stderr, kStatusFormat, message.c_str());
    }
    last_start_time = now();
}

static void Epilog(int status) {
    if (status) {
        fprintf(stderr, "FAILED (%s)\n", fb->Error().c_str());
        die("Command failed");
    } else {
        double split = now();
        fprintf(stderr, "OKAY [%7.3fs]\n", (split - last_start_time));
    }
}

static void InfoMessage(const std::string& info) {
    fprintf(stderr, "(bootloader) %s\n", info.c_str());
}

static void TextMessage(const std::string& text) {
    fprintf(stderr, "%s", text.c_str());
}

bool ReadFileToVector(const std::string& file, std::vector<char>* out) {
    out->clear();

    unique_fd fd(TEMP_FAILURE_RETRY(open(file.c_str(), O_RDONLY | O_CLOEXEC | O_BINARY)));
    if (fd == -1) {
        return false;
    }

    out->resize(get_file_size(fd));
    return ReadFully(fd, out->data(), out->size());
}

static int match_fastboot_with_serial(usb_ifc_info* info, const char* local_serial) {
    if (info->ifc_class != 0xff || info->ifc_subclass != 0x42 || info->ifc_protocol != 0x03) {
        return -1;
    }

    // require matching serial number or device path if requested
    // at the command line with the -s option.
    if (local_serial && (strcmp(local_serial, info->serial_number) != 0 &&
                         strcmp(local_serial, info->device_path) != 0))
        return -1;
    return 0;
}

static ifc_match_func match_fastboot(const char* local_serial = serial) {
    return [local_serial](usb_ifc_info* info) -> int {
        return match_fastboot_with_serial(info, local_serial);
    };
}

// output compatible with "adb devices"
static void PrintDevice(const char* local_serial, const char* status = nullptr,
                        const char* details = nullptr) {
    if (local_serial == nullptr || strlen(local_serial) == 0) {
        return;
    }

    if (g_long_listing) {
        printf("%-22s", local_serial);
    } else {
        printf("%s\t", local_serial);
    }

    if (status != nullptr && strlen(status) > 0) {
        printf(" %s", status);
    }

    if (g_long_listing) {
        if (details != nullptr && strlen(details) > 0) {
            printf(" %s", details);
        }
    }

    putchar('\n');
}

static int list_devices_callback(usb_ifc_info* info) {
    if (match_fastboot_with_serial(info, nullptr) == 0) {
        std::string serial = info->serial_number;
        std::string interface = info->interface;
        if (interface.empty()) {
            interface = "fastboot";
        }
        if (!info->writable) {
            serial = UsbNoPermissionsShortHelpText();
        }
        if (!serial[0]) {
            serial = "????????????";
        }

        PrintDevice(serial.c_str(), interface.c_str(), info->device_path);
    }

    return -1;
}

Result<NetworkSerial, FastbootError> ParseNetworkSerial(const std::string& serial) {
    Socket::Protocol protocol;
    const char* net_address = nullptr;
    int port = 0;

    if (android::base::StartsWith(serial, "tcp:")) {
        protocol = Socket::Protocol::kTcp;
        net_address = serial.c_str() + strlen("tcp:");
        port = tcp::kDefaultPort;
    } else if (android::base::StartsWith(serial, "udp:")) {
        protocol = Socket::Protocol::kUdp;
        net_address = serial.c_str() + strlen("udp:");
        port = udp::kDefaultPort;
    } else {
        return Error<FastbootError>(FastbootError::Type::NETWORK_SERIAL_WRONG_PREFIX)
               << "protocol prefix ('tcp:' or 'udp:') is missed: " << serial << ". "
               << "Expected address format:\n"
               << "<protocol>:<address>:<port> (tcp:localhost:5554)";
    }

    std::string error;
    std::string host;
    if (!android::base::ParseNetAddress(net_address, &host, &port, nullptr, &error)) {
        return Error<FastbootError>(FastbootError::Type::NETWORK_SERIAL_WRONG_ADDRESS)
               << "invalid network address '" << net_address << "': " << error;
    }

    return NetworkSerial{protocol, host, port};
}

// Opens a new Transport connected to the particular device.
// arguments:
//
// local_serial - device to connect (can be a network or usb serial name)
// wait_for_device - flag indicates whether we need to wait for device
// announce - flag indicates whether we need to print error to stdout in case
// we cannot connect to the device
//
// The returned Transport is a singleton, so multiple calls to this function will return the same
// object, and the caller should not attempt to delete the returned Transport.
static std::unique_ptr<Transport> open_device(const char* local_serial, bool wait_for_device = true,
                                              bool announce = true) {
    const Result<NetworkSerial, FastbootError> network_serial = ParseNetworkSerial(local_serial);

    std::unique_ptr<Transport> transport;
    while (true) {
        if (network_serial.ok()) {
            std::string error;
            if (network_serial->protocol == Socket::Protocol::kTcp) {
                transport = tcp::Connect(network_serial->address, network_serial->port, &error);
            } else if (network_serial->protocol == Socket::Protocol::kUdp) {
                transport = udp::Connect(network_serial->address, network_serial->port, &error);
            }

            if (!transport && announce) {
                LOG(ERROR) << "error: " << error;
            }
        } else if (network_serial.error().code() ==
                   FastbootError::Type::NETWORK_SERIAL_WRONG_PREFIX) {
            // WRONG_PREFIX is special because it happens when user wants to communicate with USB
            // device
            transport = usb_open(match_fastboot(local_serial));
        } else {
            Expect(network_serial);
        }

        if (transport) {
            return transport;
        }

        if (!wait_for_device) {
            return transport;
        }

        if (announce) {
            announce = false;
            LOG(ERROR) << "< waiting for " << local_serial << ">";
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

static std::unique_ptr<Transport> NetworkDeviceConnected(bool print = false) {
    std::unique_ptr<Transport> transport;
    std::unique_ptr<Transport> result;

    ConnectedDevicesStorage storage;
    std::set<std::string> devices;
    if (storage.Exists()) {
        FileLock lock = storage.Lock();
        devices = storage.ReadDevices(lock);
    }

    for (const std::string& device : devices) {
        transport = open_device(device.c_str(), false, false);

        if (print) {
            PrintDevice(device.c_str(), transport ? "fastboot" : "offline");
        }

        if (transport) {
            result = std::move(transport);
        }
    }

    return result;
}

// Detects the fastboot connected device to open a new Transport.
// Detecting logic:
//
// if serial is provided - try to connect to this particular usb/network device
// othervise:
// 1. Check connected usb devices and return the last connected one
// 2. Check connected network devices and return the last connected one
// 2. If nothing is connected - wait for any device by repeating p. 1 and 2
//
// The returned Transport is a singleton, so multiple calls to this function will return the same
// object, and the caller should not attempt to delete the returned Transport.
static std::unique_ptr<Transport> open_device() {
    if (serial != nullptr) {
        return open_device(serial);
    }

    bool announce = true;
    std::unique_ptr<Transport> transport;
    while (true) {
        transport = usb_open(match_fastboot(nullptr));
        if (transport) {
            return transport;
        }

        transport = NetworkDeviceConnected();
        if (transport) {
            return transport;
        }

        if (announce) {
            announce = false;
            LOG(ERROR) << "< waiting for any device >";
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return transport;
}

static int Connect(int argc, char* argv[]) {
    if (argc != 1) {
        LOG(FATAL) << "connect command requires to receive only 1 argument. Usage:" << std::endl
                   << "fastboot connect [tcp:|udp:host:port]";
    }

    const char* local_serial = *argv;
    Expect(ParseNetworkSerial(local_serial));

    if (!open_device(local_serial, false)) {
        return 1;
    }

    ConnectedDevicesStorage storage;
    {
        FileLock lock = storage.Lock();
        std::set<std::string> devices = storage.ReadDevices(lock);
        devices.insert(local_serial);
        storage.WriteDevices(lock, devices);
    }

    return 0;
}

static int Disconnect(const char* local_serial) {
    Expect(ParseNetworkSerial(local_serial));

    ConnectedDevicesStorage storage;
    {
        FileLock lock = storage.Lock();
        std::set<std::string> devices = storage.ReadDevices(lock);
        devices.erase(local_serial);
        storage.WriteDevices(lock, devices);
    }

    return 0;
}

static int Disconnect() {
    ConnectedDevicesStorage storage;
    {
        FileLock lock = storage.Lock();
        storage.Clear(lock);
    }

    return 0;
}

static int Disconnect(int argc, char* argv[]) {
    switch (argc) {
        case 0: {
            return Disconnect();
        }
        case 1: {
            return Disconnect(*argv);
        }
        default:
            LOG(FATAL) << "disconnect command can receive only 0 or 1 arguments. Usage:"
                       << std::endl
                       << "fastboot disconnect # disconnect all devices" << std::endl
                       << "fastboot disconnect [tcp:|udp:host:port] # disconnect device";
    }

    return 0;
}

static void list_devices() {
    // We don't actually open a USB device here,
    // just getting our callback called so we can
    // list all the connected devices.
    usb_open(list_devices_callback);
    NetworkDeviceConnected(/* print */ true);
}

void syntax_error(const char* fmt, ...) {
    fprintf(stderr, "fastboot: usage: ");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
    exit(1);
}

static int show_help() {
    // clang-format off
    fprintf(stdout,
//                    1         2         3         4         5         6         7         8
//           12345678901234567890123456789012345678901234567890123456789012345678901234567890
            "usage: fastboot [OPTION...] COMMAND...\n"
            "\n"
            "flashing:\n"
            " update ZIP                 Flash all partitions from an update.zip package.\n"
            " flashall                   Flash all partitions from $ANDROID_PRODUCT_OUT.\n"
            "                            On A/B devices, flashed slot is set as active.\n"
            "                            Secondary images may be flashed to inactive slot.\n"
            " flash PARTITION [FILENAME] Flash given partition, using the image from\n"
            "                            $ANDROID_PRODUCT_OUT if no filename is given.\n"
            " flash vendor_boot:RAMDISK [FILENAME]\n"
            "                            Flash vendor_boot ramdisk, fetching the existing\n"
            "                            vendor_boot image and repackaging it with the new\n"
            "                            ramdisk.\n"
            " --dtb DTB                  If set with flash vendor_boot:RAMDISK, then\n"
            "                            update the vendor_boot image with provided DTB.\n"
            "\n"
            "basics:\n"
            " devices [-l]               List devices in bootloader (-l: with device paths).\n"
            " getvar NAME                Display given bootloader variable.\n"
            " reboot [bootloader]        Reboot device.\n"
            "\n"
            "locking/unlocking:\n"
            " flashing lock|unlock       Lock/unlock partitions for flashing\n"
            " flashing lock_critical|unlock_critical\n"
            "                            Lock/unlock 'critical' bootloader partitions.\n"
            " flashing get_unlock_ability\n"
            "                            Check whether unlocking is allowed (1) or not(0).\n"
            "\n"
            "advanced:\n"
            " erase PARTITION            Erase a flash partition.\n"
            " format[:FS_TYPE[:SIZE]] PARTITION\n"
            "                            Format a flash partition.\n"
            " set_active SLOT            Set the active slot.\n"
            " oem [COMMAND...]           Execute OEM-specific command.\n"
            " gsi wipe|disable|status    Wipe, disable or show status of a GSI installation\n"
            "                            (fastbootd only).\n"
            " wipe-super [SUPER_EMPTY]   Wipe the super partition. This will reset it to\n"
            "                            contain an empty set of default dynamic partitions.\n"
            " create-logical-partition NAME SIZE\n"
            "                            Create a logical partition with the given name and\n"
            "                            size, in the super partition.\n"
            " delete-logical-partition NAME\n"
            "                            Delete a logical partition with the given name.\n"
            " resize-logical-partition NAME SIZE\n"
            "                            Change the size of the named logical partition.\n"
            " snapshot-update cancel     On devices that support snapshot-based updates, cancel\n"
            "                            an in-progress update. This may make the device\n"
            "                            unbootable until it is reflashed.\n"
            " snapshot-update merge      On devices that support snapshot-based updates, finish\n"
            "                            an in-progress update if it is in the \"merging\"\n"
            "                            phase.\n"
            " fetch PARTITION OUT_FILE   Fetch a partition image from the device."
            "\n"
            "boot image:\n"
            " boot KERNEL [RAMDISK [SECOND]]\n"
            "                            Download and boot kernel from RAM.\n"
            " flash:raw PARTITION KERNEL [RAMDISK [SECOND]]\n"
            "                            Create boot image and flash it.\n"
            " --dtb DTB                  Specify path to DTB for boot image header version 2.\n"
            " --cmdline CMDLINE          Override kernel command line.\n"
            " --base ADDRESS             Set kernel base address (default: 0x10000000).\n"
            " --kernel-offset            Set kernel offset (default: 0x00008000).\n"
            " --ramdisk-offset           Set ramdisk offset (default: 0x01000000).\n"
            " --tags-offset              Set tags offset (default: 0x00000100).\n"
            " --dtb-offset               Set dtb offset (default: 0x01100000).\n"
            " --page-size BYTES          Set flash page size (default: 2048).\n"
            " --header-version VERSION   Set boot image header version.\n"
            " --os-version MAJOR[.MINOR[.PATCH]]\n"
            "                            Set boot image OS version (default: 0.0.0).\n"
            " --os-patch-level YYYY-MM-DD\n"
            "                            Set boot image OS security patch level.\n"
            // TODO: still missing: `second_addr`, `name`, `id`, `recovery_dtbo_*`.
            "\n"
            // TODO: what device(s) used this? is there any documentation?
            //" continue                               Continue with autoboot.\n"
            //"\n"
            "Android Things:\n"
            " stage IN_FILE              Sends given file to stage for the next command.\n"
            " get_staged OUT_FILE        Writes data staged by the last command to a file.\n"
            "\n"
            "options:\n"
            " -w                         Wipe userdata.\n"
            " -s SERIAL                  Specify a USB device.\n"
            " -s tcp|udp:HOST[:PORT]     Specify a network device.\n"
            " -S SIZE[K|M|G]             Break into sparse files no larger than SIZE.\n"
            " --force                    Force a flash operation that may be unsafe.\n"
            " --slot SLOT                Use SLOT; 'all' for both slots, 'other' for\n"
            "                            non-current slot (default: current active slot).\n"
            " --set-active[=SLOT]        Sets the active slot before rebooting.\n"
            " --skip-secondary           Don't flash secondary slots in flashall/update.\n"
            " --skip-reboot              Don't reboot device after flashing.\n"
            " --disable-verity           Sets disable-verity when flashing vbmeta.\n"
            " --disable-verification     Sets disable-verification when flashing vbmeta.\n"
            " --disable-super-optimization\n"
            "                            Disables optimizations on flashing super partition.\n"
            " --exclude-dynamic-partitions\n"
            "                            Excludes flashing of dynamic partitions.\n"
            " --disable-fastboot-info    Will collects tasks from image list rather than \n"
            "                            $OUT/fastboot-info.txt.\n"
            " --fs-options=OPTION[,OPTION]\n"
            "                            Enable filesystem features. OPTION supports casefold, projid, compress\n"
            // TODO: remove --unbuffered?
            " --unbuffered               Don't buffer input or output.\n"
            " --verbose, -v              Verbose output.\n"
            " --version                  Display version.\n"
            " --help, -h                 Show this message.\n"
        );
    // clang-format on
    return 0;
}

static std::vector<char> LoadBootableImage(const std::string& kernel, const std::string& ramdisk,
                                           const std::string& second_stage) {
    std::vector<char> kernel_data;
    if (!ReadFileToVector(kernel, &kernel_data)) {
        die("cannot load '%s': %s", kernel.c_str(), strerror(errno));
    }

    // Is this actually a boot image?
    if (kernel_data.size() < sizeof(boot_img_hdr_v3)) {
        die("cannot load '%s': too short", kernel.c_str());
    }
    if (!memcmp(kernel_data.data(), BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
        if (!g_cmdline.empty()) {
            bootimg_set_cmdline(reinterpret_cast<boot_img_hdr_v2*>(kernel_data.data()), g_cmdline);
        }

        if (!ramdisk.empty()) die("cannot boot a boot.img *and* ramdisk");

        return kernel_data;
    }

    std::vector<char> ramdisk_data;
    if (!ramdisk.empty()) {
        if (!ReadFileToVector(ramdisk, &ramdisk_data)) {
            die("cannot load '%s': %s", ramdisk.c_str(), strerror(errno));
        }
    }

    std::vector<char> second_stage_data;
    if (!second_stage.empty()) {
        if (!ReadFileToVector(second_stage, &second_stage_data)) {
            die("cannot load '%s': %s", second_stage.c_str(), strerror(errno));
        }
    }

    std::vector<char> dtb_data;
    if (!g_dtb_path.empty()) {
        if (g_boot_img_hdr.header_version != 2) {
            die("Argument dtb not supported for boot image header version %d\n",
                g_boot_img_hdr.header_version);
        }
        if (!ReadFileToVector(g_dtb_path, &dtb_data)) {
            die("cannot load '%s': %s", g_dtb_path.c_str(), strerror(errno));
        }
    }

    fprintf(stderr, "creating boot image...\n");

    std::vector<char> out;
    mkbootimg(kernel_data, ramdisk_data, second_stage_data, dtb_data, g_base_addr, g_boot_img_hdr,
              &out);

    if (!g_cmdline.empty()) {
        bootimg_set_cmdline(reinterpret_cast<boot_img_hdr_v2*>(out.data()), g_cmdline);
    }
    fprintf(stderr, "creating boot image - %zu bytes\n", out.size());
    return out;
}

static zip_int64_t ExtractToBuffer(zip_file_t *zip_file, std::vector<char>& out) {
    zip_int64_t bytes_read;
    zip_uint64_t pos = 0;
    while ((bytes_read = zip_fread(zip_file, &out[pos], out.size() - pos)) > 0) {
        pos += bytes_read;
    }
    return bytes_read;
}

static bool UnzipToMemory(unique_zip_t& zip, const char *entry_name,
                          std::vector<char>* out) {
    auto zip_file = zip_fopen(zip.get(), entry_name, 0);
    if (!zip_file) {
        fprintf(stderr, "archive does not contain '%s'\n", entry_name);
        return false;
    }

    zip_stat_t zstat;
    zip_stat_init(&zstat);
    if (zip_stat(zip.get(), entry_name, 0, &zstat) < 0 || !(zstat.valid & ZIP_STAT_SIZE)) {
        die("size of entry '%s' could not be determined\n", entry_name);
    }
    if (zstat.size > std::numeric_limits<size_t>::max()) {
        die("entry '%s' is too large: %" PRIu64, entry_name, zstat.size);
    }
    out->resize(zstat.size);

    fprintf(stderr, "extracting %s (%zu MB) to RAM...\n", entry_name,
            out->size() / 1024 / 1024);

    if (ExtractToBuffer(zip_file, *out) < 0) {
        auto error = zip_file_get_error(zip_file);
        die("failed to extract '%s': %s", entry_name, zip_error_strerror(error));
    }

    return true;
}

#if defined(_WIN32)

// TODO: move this to somewhere it can be shared.

#include <windows.h>

// Windows' tmpfile(3) requires administrator rights because
// it creates temporary files in the root directory.
static FILE* win32_tmpfile() {
    char temp_path[PATH_MAX];
    DWORD nchars = GetTempPath(sizeof(temp_path), temp_path);
    if (nchars == 0 || nchars >= sizeof(temp_path)) {
        die("GetTempPath failed, error %ld", GetLastError());
    }

    char filename[PATH_MAX];
    if (GetTempFileName(temp_path, "fastboot", 0, filename) == 0) {
        die("GetTempFileName failed, error %ld", GetLastError());
    }

    return fopen(filename, "w+bTD");
}

#define tmpfile win32_tmpfile

static int make_temporary_fd(const char* /*what*/) {
    // TODO: reimplement to avoid leaking a FILE*.
    return fileno(tmpfile());
}

#else

static std::string make_temporary_template() {
    const char* tmpdir = getenv("TMPDIR");
    if (tmpdir == nullptr) tmpdir = P_tmpdir;
    return std::string(tmpdir) + "/fastboot_userdata_XXXXXX";
}

static int make_temporary_fd(const char* what) {
    std::string path_template(make_temporary_template());
    int fd = mkstemp(&path_template[0]);
    if (fd == -1) {
        die("failed to create temporary file for %s with template %s: %s\n", path_template.c_str(),
            what, strerror(errno));
    }
    unlink(path_template.c_str());
    return fd;
}

#endif

static unique_fd UnzipToFile(unique_zip_t& zip, const char* entry_name) {
    unique_fd fd(make_temporary_fd(entry_name));

    auto zip_file = zip_fopen(zip.get(), entry_name, 0);
    if (!zip_file) {
        fprintf(stderr, "archive does not contain '%s'\n", entry_name);
        errno = ENOENT;
        return unique_fd();
    }

    zip_stat_t zstat;
    zip_stat_init(&zstat);
    if (zip_stat(zip.get(), entry_name, 0, &zstat) < 0 || !(zstat.valid & ZIP_STAT_SIZE)) {
        die("size of entry '%s' could not be determined\n", entry_name);
    }

    fprintf(stderr, "extracting %s (%" PRIu64 " MB) to disk...", entry_name,
            zstat.size / 1024 / 1024);

    double start = now();
    while (true) {
        // Emulate libziparchive, which uses a 32kB read/write buffer
        const size_t kBufSize = 32768;
        std::vector<char> out(kBufSize);
        auto bytes_read = ExtractToBuffer(zip_file, out);
        if (bytes_read < 0) {
            auto error = zip_file_get_error(zip_file);
            die("\nfailed to extract '%s': %s", entry_name, zip_error_strerror(error));
        }
        if (bytes_read == 0) {
            break;
        }

        ssize_t bytes_written;
        size_t pos = 0;
        while ((bytes_written = write(fd.get(), &out[pos], out.size() - pos)) > 0) {
            pos += bytes_written;
        }
        if (bytes_written != 0) {
            die("\nfailed to write '%s': %s", entry_name, strerror(errno));
        }
    }

    if (lseek(fd.get(), 0, SEEK_SET) != 0) {
        die("\nlseek on extracted file '%s' failed: %s", entry_name, strerror(errno));
    }

    fprintf(stderr, " took %.3fs\n", now() - start);

    return fd;
}

static bool CheckRequirement(const std::string& cur_product, const std::string& var,
                             const std::string& product, bool invert,
                             const std::vector<std::string>& options) {
    Status("Checking '" + var + "'");

    double start = now();

    if (!product.empty()) {
        if (product != cur_product) {
            double split = now();
            fprintf(stderr, "IGNORE, product is %s required only for %s [%7.3fs]\n",
                    cur_product.c_str(), product.c_str(), (split - start));
            return true;
        }
    }

    std::string var_value;
    if (fb->GetVar(var, &var_value) != fastboot::SUCCESS) {
        fprintf(stderr, "FAILED\n\n");
        fprintf(stderr, "Could not getvar for '%s' (%s)\n\n", var.c_str(), fb->Error().c_str());
        return false;
    }

    bool match = false;
    for (const auto& option : options) {
        if (option == var_value ||
            (option.back() == '*' &&
             !var_value.compare(0, option.length() - 1, option, 0, option.length() - 1))) {
            match = true;
            break;
        }
    }

    if (invert) {
        match = !match;
    }

    if (match) {
        double split = now();
        fprintf(stderr, "OKAY [%7.3fs]\n", (split - start));
        return true;
    }

    fprintf(stderr, "FAILED\n\n");
    fprintf(stderr, "Device %s is '%s'.\n", var.c_str(), var_value.c_str());
    fprintf(stderr, "Update %s '%s'", invert ? "rejects" : "requires", options[0].c_str());
    for (auto it = std::next(options.begin()); it != options.end(); ++it) {
        fprintf(stderr, " or '%s'", it->c_str());
    }
    fprintf(stderr, ".\n\n");
    return false;
}

bool ParseRequirementLine(const std::string& line, std::string* name, std::string* product,
                          bool* invert, std::vector<std::string>* options) {
    // "require product=alpha|beta|gamma"
    // "require version-bootloader=1234"
    // "require-for-product:gamma version-bootloader=istanbul|constantinople"
    // "require partition-exists=vendor"
    *product = "";
    *invert = false;

    auto require_reject_regex = std::regex{"(require\\s+|reject\\s+)?\\s*(\\S+)\\s*=\\s*(.*)"};
    auto require_product_regex =
            std::regex{"require-for-product:\\s*(\\S+)\\s+(\\S+)\\s*=\\s*(.*)"};
    std::smatch match_results;

    if (std::regex_match(line, match_results, require_reject_regex)) {
        *invert = Trim(match_results[1]) == "reject";
    } else if (std::regex_match(line, match_results, require_product_regex)) {
        *product = match_results[1];
    } else {
        return false;
    }

    *name = match_results[2];
    // Work around an unfortunate name mismatch.
    if (*name == "board") {
        *name = "product";
    }

    auto raw_options = Split(match_results[3], "|");
    for (const auto& option : raw_options) {
        auto trimmed_option = Trim(option);
        options->emplace_back(trimmed_option);
    }

    return true;
}

// "require partition-exists=x" is a special case, added because of the trouble we had when
// Pixel 2 shipped with new partitions and users used old versions of fastboot to flash them,
// missing out new partitions. A device with new partitions can use "partition-exists" to
// override the fields `optional_if_no_image` in the `images` array.
static void HandlePartitionExists(const std::vector<std::string>& options) {
    const std::string& partition_name = options[0];
    std::string has_slot;
    if (fb->GetVar("has-slot:" + partition_name, &has_slot) != fastboot::SUCCESS ||
        (has_slot != "yes" && has_slot != "no")) {
        die("device doesn't have required partition %s!", partition_name.c_str());
    }
    bool known_partition = false;
    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i].nickname.empty() && images[i].nickname == partition_name) {
            images[i].optional_if_no_image = false;
            known_partition = true;
        }
    }
    if (!known_partition) {
        die("device requires partition %s which is not known to this version of fastboot",
            partition_name.c_str());
    }
}

static void CheckRequirements(const std::string& data, bool force_flash) {
    std::string cur_product;
    if (fb->GetVar("product", &cur_product) != fastboot::SUCCESS) {
        fprintf(stderr, "getvar:product FAILED (%s)\n", fb->Error().c_str());
    }

    auto lines = Split(data, "\n");
    for (const auto& line : lines) {
        if (line.empty()) {
            continue;
        }

        std::string name;
        std::string product;
        bool invert;
        std::vector<std::string> options;

        if (!ParseRequirementLine(line, &name, &product, &invert, &options)) {
            fprintf(stderr, "android-info.txt syntax error: %s\n", line.c_str());
            continue;
        }
        if (name == "partition-exists") {
            HandlePartitionExists(options);
        } else {
            bool met = CheckRequirement(cur_product, name, product, invert, options);
            if (!met) {
                if (!force_flash) {
                    die("requirements not met!");
                } else {
                    fprintf(stderr, "requirements not met! but proceeding due to --force\n");
                }
            }
        }
    }
}

static void DisplayVarOrError(const std::string& label, const std::string& var) {
    std::string value;

    if (fb->GetVar(var, &value) != fastboot::SUCCESS) {
        Status("getvar:" + var);
        fprintf(stderr, "FAILED (%s)\n", fb->Error().c_str());
        return;
    }
    fprintf(stderr, "%s: %s\n", label.c_str(), value.c_str());
}

static void DumpInfo() {
    fprintf(stderr, "--------------------------------------------\n");
    DisplayVarOrError("Bootloader Version...", "version-bootloader");
    DisplayVarOrError("Baseband Version.....", "version-baseband");
    DisplayVarOrError("Serial Number........", "serialno");
    fprintf(stderr, "--------------------------------------------\n");
}

std::vector<SparsePtr> resparse_file(sparse_file* s, int64_t max_size) {
    if (max_size <= 0 || max_size > std::numeric_limits<uint32_t>::max()) {
        die("invalid max size %" PRId64, max_size);
    }

    const int files = sparse_file_resparse(s, max_size, nullptr, 0);
    if (files < 0) die("Failed to compute resparse boundaries");

    auto temp = std::make_unique<sparse_file*[]>(files);
    const int rv = sparse_file_resparse(s, max_size, temp.get(), files);
    if (rv < 0) die("Failed to resparse");

    std::vector<SparsePtr> out_s;
    for (int i = 0; i < files; i++) {
        out_s.emplace_back(temp[i], sparse_file_destroy);
    }
    return out_s;
}

static std::vector<SparsePtr> load_sparse_files(int fd, int64_t max_size) {
    SparsePtr s(sparse_file_import_auto(fd, false, true), sparse_file_destroy);
    if (!s) die("cannot sparse read file");

    return resparse_file(s.get(), max_size);
}

static uint64_t get_uint_var(const char* var_name, fastboot::IFastBootDriver* fb) {
    std::string value_str;
    if (fb->GetVar(var_name, &value_str) != fastboot::SUCCESS || value_str.empty()) {
        verbose("target didn't report %s", var_name);
        return 0;
    }

    // Some bootloaders (angler, for example) send spurious whitespace too.
    value_str = android::base::Trim(value_str);

    uint64_t value;
    if (!android::base::ParseUint(value_str, &value)) {
        fprintf(stderr, "couldn't parse %s '%s'\n", var_name, value_str.c_str());
        return 0;
    }
    if (value > 0) verbose("target reported %s of %" PRId64 " bytes", var_name, value);
    return value;
}

int64_t get_sparse_limit(int64_t size, const FlashingPlan* fp) {
    if (!fp) return 0;

    int64_t limit = int64_t(fp->sparse_limit);
    if (limit == 0) {
        // Unlimited, so see what the target device's limit is.
        // TODO: shouldn't we apply this limit even if you've used -S?
        if (target_sparse_limit == -1) {
            target_sparse_limit = static_cast<int64_t>(get_uint_var("max-download-size", fp->fb));
        }
        if (target_sparse_limit > 0) {
            limit = target_sparse_limit;
        } else {
            return 0;
        }
    }

    if (size > limit) {
        return std::min(limit, RESPARSE_LIMIT);
    }

    return 0;
}

static bool load_buf_fd(unique_fd fd, struct fastboot_buffer* buf, const FlashingPlan* fp) {
    int64_t sz = get_file_size(fd);
    if (sz == -1) {
        return false;
    }

    if (sparse_file* s = sparse_file_import(fd.get(), false, false)) {
        buf->image_size = sparse_file_len(s, false, false);
        if (buf->image_size < 0) {
            LOG(ERROR) << "Could not compute length of sparse file";
            return false;
        }
        sparse_file_destroy(s);
        buf->file_type = FB_BUFFER_SPARSE;
    } else {
        buf->image_size = sz;
        buf->file_type = FB_BUFFER_FD;
    }

    lseek(fd.get(), 0, SEEK_SET);
    int64_t limit = get_sparse_limit(sz, fp);
    buf->fd = std::move(fd);
    if (limit) {
        buf->files = load_sparse_files(buf->fd.get(), limit);
        if (buf->files.empty()) {
            return false;
        }
        buf->type = FB_BUFFER_SPARSE;
    } else {
        buf->type = FB_BUFFER_FD;
        buf->sz = sz;
    }

    return true;
}

static bool load_buf(const char* fname, struct fastboot_buffer* buf, const FlashingPlan* fp) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(fname, O_RDONLY | O_BINARY)));

    if (fd == -1) {
        return false;
    }

    struct stat s;
    if (fstat(fd.get(), &s)) {
        return false;
    }
    if (!S_ISREG(s.st_mode)) {
        errno = S_ISDIR(s.st_mode) ? EISDIR : EINVAL;
        return false;
    }

    return load_buf_fd(std::move(fd), buf, fp);
}

static void rewrite_vbmeta_buffer(struct fastboot_buffer* buf, bool vbmeta_in_boot) {
    // Buffer needs to be at least the size of the VBMeta struct which
    // is 256 bytes.
    if (buf->sz < 256) {
        return;
    }

    std::string data;
    if (!android::base::ReadFdToString(buf->fd, &data)) {
        die("Failed reading from vbmeta");
    }

    uint64_t vbmeta_offset = 0;
    if (vbmeta_in_boot) {
        // Tries to locate top-level vbmeta from boot.img footer.
        uint64_t footer_offset = buf->sz - AVB_FOOTER_SIZE;
        if (0 != data.compare(footer_offset, AVB_FOOTER_MAGIC_LEN, AVB_FOOTER_MAGIC)) {
            die("Failed to find AVB_FOOTER at offset: %" PRId64 ", is BOARD_AVB_ENABLE true?",
                footer_offset);
        }
        const AvbFooter* footer = reinterpret_cast<const AvbFooter*>(data.c_str() + footer_offset);
        vbmeta_offset = be64toh(footer->vbmeta_offset);
    }
    // Ensures there is AVB_MAGIC at vbmeta_offset.
    if (0 != data.compare(vbmeta_offset, AVB_MAGIC_LEN, AVB_MAGIC)) {
        die("Failed to find AVB_MAGIC at offset: %" PRId64, vbmeta_offset);
    }

    fprintf(stderr, "Rewriting vbmeta struct at offset: %" PRId64 "\n", vbmeta_offset);

    // There's a 32-bit big endian |flags| field at offset 120 where
    // bit 0 corresponds to disable-verity and bit 1 corresponds to
    // disable-verification.
    //
    // See external/avb/libavb/avb_vbmeta_image.h for the layout of
    // the VBMeta struct.
    uint64_t flags_offset = 123 + vbmeta_offset;
    if (g_disable_verity) {
        data[flags_offset] |= 0x01;
    }
    if (g_disable_verification) {
        data[flags_offset] |= 0x02;
    }

    unique_fd fd(make_temporary_fd("vbmeta rewriting"));
    if (!android::base::WriteStringToFd(data, fd)) {
        die("Failed writing to modified vbmeta");
    }
    buf->fd = std::move(fd);
    lseek(buf->fd.get(), 0, SEEK_SET);
}

static bool has_vbmeta_partition() {
    std::string partition_type;
    return fb->GetVar("partition-type:vbmeta", &partition_type) == fastboot::SUCCESS ||
           fb->GetVar("partition-type:vbmeta_a", &partition_type) == fastboot::SUCCESS ||
           fb->GetVar("partition-type:vbmeta_b", &partition_type) == fastboot::SUCCESS;
}

static bool is_vbmeta_partition(const std::string& partition) {
    return android::base::EndsWith(partition, "vbmeta") ||
           android::base::EndsWith(partition, "vbmeta_a") ||
           android::base::EndsWith(partition, "vbmeta_b");
}

// Note: this only works in userspace fastboot. In the bootloader, use
// should_flash_in_userspace().
bool is_logical(const std::string& partition) {
    std::string value;
    return fb->GetVar("is-logical:" + partition, &value) == fastboot::SUCCESS && value == "yes";
}

static uint64_t get_partition_size(const std::string& partition) {
    std::string partition_size_str;
    if (fb->GetVar("partition-size:" + partition, &partition_size_str) != fastboot::SUCCESS) {
        if (!is_logical(partition)) {
            return 0;
        }
        die("cannot get partition size for %s", partition.c_str());
    }

    partition_size_str = fb_fix_numeric_var(partition_size_str);
    uint64_t partition_size;
    if (!android::base::ParseUint(partition_size_str, &partition_size)) {
        if (!is_logical(partition)) {
            return 0;
        }
        die("Couldn't parse partition size '%s'.", partition_size_str.c_str());
    }
    return partition_size;
}

static void copy_avb_footer(const ImageSource* source, const std::string& partition,
                            struct fastboot_buffer* buf) {
    if (buf->sz < AVB_FOOTER_SIZE || is_logical(partition) ||
        should_flash_in_userspace(source, partition)) {
        return;
    }

    // If the image is sparse, moving the footer will simply corrupt the sparse
    // format, so currently we don't support moving the footer on sparse files.
    if (buf->file_type == FB_BUFFER_SPARSE) {
        LOG(ERROR) << "Warning: skip copying " << partition << " image avb footer due to sparse "
                   << "image.";
        return;
    }

    // If overflows and negative, it should be < buf->sz.
    int64_t partition_size = static_cast<int64_t>(get_partition_size(partition));

    if (partition_size == buf->sz) {
        return;
    }
    // Some device bootloaders might not implement `fastboot getvar partition-size:boot[_a|_b]`.
    // In this case, partition_size will be zero.
    if (partition_size < buf->sz) {
        fprintf(stderr,
                "Warning: skip copying %s image avb footer"
                " (%s partition size: %" PRId64 ", %s image size: %" PRId64 ").\n",
                partition.c_str(), partition.c_str(), partition_size, partition.c_str(), buf->sz);
        return;
    }

    // IMPORTANT: after the following read, we need to reset buf->fd before return (if not die).
    // Because buf->fd will still be used afterwards.
    std::string data;
    if (!android::base::ReadFdToString(buf->fd, &data)) {
        die("Failed reading from %s", partition.c_str());
    }

    uint64_t footer_offset = buf->sz - AVB_FOOTER_SIZE;
    if (0 != data.compare(footer_offset, AVB_FOOTER_MAGIC_LEN, AVB_FOOTER_MAGIC)) {
        lseek(buf->fd.get(), 0, SEEK_SET);  // IMPORTANT: resets buf->fd before return.
        return;
    }

    const std::string tmp_fd_template = partition + " rewriting";
    unique_fd fd(make_temporary_fd(tmp_fd_template.c_str()));
    if (!android::base::WriteStringToFd(data, fd)) {
        die("Failed writing to modified %s", partition.c_str());
    }
    lseek(fd.get(), partition_size - AVB_FOOTER_SIZE, SEEK_SET);
    if (!android::base::WriteStringToFd(data.substr(footer_offset), fd)) {
        die("Failed copying AVB footer in %s", partition.c_str());
    }
    buf->fd = std::move(fd);
    buf->sz = partition_size;
    lseek(buf->fd.get(), 0, SEEK_SET);
}

void flash_partition_files(const std::string& partition, const std::vector<SparsePtr>& files) {
    for (size_t i = 0; i < files.size(); i++) {
        sparse_file* s = files[i].get();
        int64_t sz = sparse_file_len(s, true, false);
        if (sz < 0) {
            LOG(FATAL) << "Could not compute length of sparse image for " << partition;
        }
        fb->FlashPartition(partition, s, sz, i + 1, files.size());
    }
}

static void flash_buf(const ImageSource* source, const std::string& partition,
                      struct fastboot_buffer* buf, const bool apply_vbmeta) {
    copy_avb_footer(source, partition, buf);

    // Rewrite vbmeta if that's what we're flashing and modification has been requested.
    if (g_disable_verity || g_disable_verification) {
        // The vbmeta partition might have additional prefix if running in virtual machine
        // e.g., guest_vbmeta_a.
        if (apply_vbmeta) {
            rewrite_vbmeta_buffer(buf, false /* vbmeta_in_boot */);
        } else if (!has_vbmeta_partition() &&
                   (partition == "boot" || partition == "boot_a" || partition == "boot_b")) {
            rewrite_vbmeta_buffer(buf, true /* vbmeta_in_boot */);
        }
    }

    switch (buf->type) {
        case FB_BUFFER_SPARSE: {
            flash_partition_files(partition, buf->files);
            break;
        }
        case FB_BUFFER_FD:
            fb->FlashPartition(partition, buf->fd, buf->sz);
            break;
        default:
            die("unknown buffer type: %d", buf->type);
    }
}

std::string get_current_slot() {
    std::string current_slot;
    if (fb->GetVar("current-slot", &current_slot) != fastboot::SUCCESS) return "";
    if (current_slot[0] == '_') current_slot.erase(0, 1);
    return current_slot;
}

static int get_slot_count(fastboot::IFastBootDriver* fb) {
    std::string var;
    int count = 0;
    if (fb->GetVar("slot-count", &var) != fastboot::SUCCESS ||
        !android::base::ParseInt(var, &count)) {
        return 0;
    }
    return count;
}

bool supports_AB(fastboot::IFastBootDriver* fb) {
    return get_slot_count(fb) >= 2;
}

// Given a current slot, this returns what the 'other' slot is.
static std::string get_other_slot(const std::string& current_slot, int count) {
    if (count == 0) return "";

    char next = (current_slot[0] - 'a' + 1) % count + 'a';
    return std::string(1, next);
}

static std::string get_other_slot(const std::string& current_slot) {
    return get_other_slot(current_slot, get_slot_count(fb));
}

static std::string get_other_slot(int count) {
    return get_other_slot(get_current_slot(), count);
}

static std::string get_other_slot() {
    return get_other_slot(get_current_slot(), get_slot_count(fb));
}

static std::string verify_slot(const std::string& slot_name, bool allow_all) {
    std::string slot = slot_name;
    if (slot == "all") {
        if (allow_all) {
            return "all";
        } else {
            int count = get_slot_count(fb);
            if (count > 0) {
                return "a";
            } else {
                die("No known slots");
            }
        }
    }

    int count = get_slot_count(fb);
    if (count == 0) die("Device does not support slots");

    if (slot == "other") {
        std::string other = get_other_slot(count);
        if (other == "") {
            die("No known slots");
        }
        return other;
    }

    if (slot.size() == 1 && (slot[0] - 'a' >= 0 && slot[0] - 'a' < count)) return slot;

    fprintf(stderr, "Slot %s does not exist. supported slots are:\n", slot.c_str());
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "%c\n", (char)(i + 'a'));
    }

    exit(1);
}

static std::string verify_slot(const std::string& slot) {
    return verify_slot(slot, true);
}

static void do_for_partition(const std::string& part, const std::string& slot,
                             const std::function<void(const std::string&)>& func, bool force_slot) {
    std::string has_slot;
    std::string current_slot;
    // |part| can be vendor_boot:default. Append slot to the first token.
    auto part_tokens = android::base::Split(part, ":");

    if (fb->GetVar("has-slot:" + part_tokens[0], &has_slot) != fastboot::SUCCESS) {
        /* If has-slot is not supported, the answer is no. */
        has_slot = "no";
    }
    if (has_slot == "yes") {
        if (slot == "") {
            current_slot = get_current_slot();
            if (current_slot == "") {
                die("Failed to identify current slot");
            }
            part_tokens[0] += "_" + current_slot;
        } else {
            part_tokens[0] += "_" + slot;
        }
        func(android::base::Join(part_tokens, ":"));
    } else {
        if (force_slot && slot != "") {
            fprintf(stderr, "Warning: %s does not support slots, and slot %s was requested.\n",
                    part_tokens[0].c_str(), slot.c_str());
        }
        func(part);
    }
}

/* This function will find the real partition name given a base name, and a slot. If slot is NULL or
 * empty, it will use the current slot. If slot is "all", it will return a list of all possible
 * partition names. If force_slot is true, it will fail if a slot is specified, and the given
 * partition does not support slots.
 */
void do_for_partitions(const std::string& part, const std::string& slot,
                       const std::function<void(const std::string&)>& func, bool force_slot) {
    std::string has_slot;
    // |part| can be vendor_boot:default. Query has-slot on the first token only.
    auto part_tokens = android::base::Split(part, ":");

    if (slot == "all") {
        if (fb->GetVar("has-slot:" + part_tokens[0], &has_slot) != fastboot::SUCCESS) {
            die("Could not check if partition %s has slot %s", part_tokens[0].c_str(),
                slot.c_str());
        }
        if (has_slot == "yes") {
            for (int i = 0; i < get_slot_count(fb); i++) {
                do_for_partition(part, std::string(1, (char)(i + 'a')), func, force_slot);
            }
        } else {
            do_for_partition(part, "", func, force_slot);
        }
    } else {
        do_for_partition(part, slot, func, force_slot);
    }
}

// Fetch a partition from the device to a given fd. This is a wrapper over FetchToFd to fetch
// the full image.
static uint64_t fetch_partition(const std::string& partition, borrowed_fd fd,
                                fastboot::IFastBootDriver* fb) {
    uint64_t fetch_size = get_uint_var(FB_VAR_MAX_FETCH_SIZE, fb);
    if (fetch_size == 0) {
        die("Unable to get %s. Device does not support fetch command.", FB_VAR_MAX_FETCH_SIZE);
    }
    uint64_t partition_size = get_partition_size(partition);
    if (partition_size <= 0) {
        die("Invalid partition size for partition %s: %" PRId64, partition.c_str(), partition_size);
    }

    uint64_t offset = 0;
    while (offset < partition_size) {
        uint64_t chunk_size = std::min(fetch_size, partition_size - offset);
        if (fb->FetchToFd(partition, fd, offset, chunk_size) != fastboot::RetCode::SUCCESS) {
            die("Unable to fetch %s (offset=%" PRIx64 ", size=%" PRIx64 ")", partition.c_str(),
                offset, chunk_size);
        }
        offset += chunk_size;
    }
    return partition_size;
}

static void do_fetch(const std::string& partition, const std::string& slot_override,
                     const std::string& outfile, fastboot::IFastBootDriver* fb) {
    unique_fd fd(TEMP_FAILURE_RETRY(
            open(outfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_BINARY, 0644)));
    auto fetch = std::bind(fetch_partition, _1, borrowed_fd(fd), fb);
    do_for_partitions(partition, slot_override, fetch, false /* force slot */);
}

// Return immediately if not flashing a vendor boot image. If flashing a vendor boot image,
// repack vendor_boot image with an updated ramdisk. After execution, buf is set
// to the new image to flash, and return value is the real partition name to flash.
static std::string repack_ramdisk(const char* pname, struct fastboot_buffer* buf,
                                  fastboot::IFastBootDriver* fb) {
    std::string_view pname_sv{pname};
    struct fastboot_buffer dtb_buf = {.sz = 0, .fd = unique_fd(-1)};

    if (!android::base::StartsWith(pname_sv, "vendor_boot:") &&
        !android::base::StartsWith(pname_sv, "vendor_boot_a:") &&
        !android::base::StartsWith(pname_sv, "vendor_boot_b:")) {
        return std::string(pname_sv);
    }
    if (buf->type != FB_BUFFER_FD) {
        die("Flashing sparse vendor ramdisk image is not supported.");
    }
    if (buf->sz <= 0) {
        die("repack_ramdisk() sees negative size: %" PRId64, buf->sz);
    }
    std::string partition(pname_sv.substr(0, pname_sv.find(':')));
    std::string ramdisk(pname_sv.substr(pname_sv.find(':') + 1));

    if (!g_dtb_path.empty()) {
        if (!load_buf(g_dtb_path.c_str(), &dtb_buf, nullptr)) {
            die("cannot load '%s': %s", g_dtb_path.c_str(), strerror(errno));
        }

        if (dtb_buf.type != FB_BUFFER_FD) {
            die("Flashing sparse vendor ramdisk image with dtb is not supported.");
        }
        if (dtb_buf.sz <= 0) {
            die("repack_ramdisk() sees invalid dtb size: %" PRId64, buf->sz);
        }
        verbose("Updating DTB with %s", pname_sv.data());
    }

    unique_fd vendor_boot(make_temporary_fd("vendor boot repack"));
    uint64_t vendor_boot_size = fetch_partition(partition, vendor_boot, fb);
    auto repack_res = replace_vendor_ramdisk(vendor_boot, vendor_boot_size, ramdisk, buf->fd,
                                             static_cast<uint64_t>(buf->sz), dtb_buf.fd,
                                             static_cast<uint64_t>(dtb_buf.sz));
    if (!repack_res.ok()) {
        die("%s", repack_res.error().message().c_str());
    }

    buf->fd = std::move(vendor_boot);
    buf->sz = vendor_boot_size;
    buf->image_size = vendor_boot_size;
    return partition;
}

void do_flash(const char* pname, const char* fname, const bool apply_vbmeta,
              const FlashingPlan* fp) {
    if (!fp) {
        die("do flash was called without a valid flashing plan");
    }
    verbose("Do flash %s %s", pname, fname);
    struct fastboot_buffer buf;

    if (fp->source) {
        unique_fd fd = fp->source->OpenFile(fname);
        if (fd < 0 || !load_buf_fd(std::move(fd), &buf, fp)) {
            die("could not load '%s': %s", fname, strerror(errno));
        }
        std::vector<char> signature_data;
        std::string file_string(fname);
        if (fp->source->ReadFile(file_string.substr(0, file_string.find('.')) + ".sig",
                                 &signature_data)) {
            fb->Download("signature", signature_data);
            fb->RawCommand("signature", "installing signature");
        }
    } else if (!load_buf(fname, &buf, fp)) {
        die("cannot load '%s': %s", fname, strerror(errno));
    }

    if (is_logical(pname)) {
        fb->ResizePartition(pname, std::to_string(buf.image_size));
    }
    std::string flash_pname = repack_ramdisk(pname, &buf, fp->fb);
    flash_buf(fp->source.get(), flash_pname, &buf, apply_vbmeta);
}

// Sets slot_override as the active slot. If slot_override is blank,
// set current slot as active instead. This clears slot-unbootable.
static void set_active(const std::string& slot_override) {
    if (!supports_AB(fb)) return;

    if (slot_override != "") {
        fb->SetActive(slot_override);
    } else {
        std::string current_slot = get_current_slot();
        if (current_slot != "") {
            fb->SetActive(current_slot);
        }
    }
}

bool is_userspace_fastboot() {
    std::string value;
    return fb->GetVar("is-userspace", &value) == fastboot::SUCCESS && value == "yes";
}

void reboot_to_userspace_fastboot() {
    fb->RebootTo("fastboot");
    if (fb->WaitForDisconnect() != fastboot::SUCCESS) {
        die("Error waiting for USB disconnect.");
    }
    fb->set_transport(nullptr);

    // Not all platforms support WaitForDisconnect. There also isn't a great way to tell whether
    // or not WaitForDisconnect is supported. So, just wait a bit extra for everyone, in order to
    // make sure that the device has had time to initiate its reboot and disconnect itself.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    fb->set_transport(open_device());

    if (!is_userspace_fastboot()) {
        die("Failed to boot into userspace fastboot; one or more components might be unbootable.");
    }

    // Reset target_sparse_limit after reboot to userspace fastboot. Max
    // download sizes may differ in bootloader and fastbootd.
    target_sparse_limit = -1;
}

static void CancelSnapshotIfNeeded() {
    std::string merge_status = "none";
    if (fb->GetVar(FB_VAR_SNAPSHOT_UPDATE_STATUS, &merge_status) == fastboot::SUCCESS &&
        !merge_status.empty() && merge_status != "none") {
        fb->SnapshotUpdateCommand("cancel");
    }
}

std::string GetPartitionName(const ImageEntry& entry, const std::string& current_slot) {
    auto slot = entry.second;
    if (slot.empty()) {
        slot = current_slot;
    }
    if (slot.empty()) {
        return entry.first->part_name;
    }
    if (slot == "all") {
        LOG(FATAL) << "Cannot retrieve a singular name when using all slots";
    }
    return entry.first->part_name + "_" + slot;
}

std::unique_ptr<FlashTask> ParseFlashCommand(const FlashingPlan* fp,
                                             const std::vector<std::string>& parts) {
    bool apply_vbmeta = false;
    std::string slot = fp->slot_override;
    std::string partition;
    std::string img_name;
    for (auto& part : parts) {
        if (part == "--apply-vbmeta") {
            apply_vbmeta = true;
        } else if (part == "--slot-other") {
            slot = fp->secondary_slot;
        } else if (partition.empty()) {
            partition = part;
        } else if (img_name.empty()) {
            img_name = part;
        } else {
            LOG(ERROR) << "unknown argument" << part
                       << " in fastboot-info.txt. parts: " << android::base::Join(parts, " ");
            return nullptr;
        }
    }
    if (partition.empty()) {
        LOG(ERROR) << "partition name not found when parsing fastboot-info.txt. parts: "
                   << android::base::Join(parts, " ");
        return nullptr;
    }
    if (img_name.empty()) {
        img_name = partition + ".img";
    }
    return std::make_unique<FlashTask>(slot, partition, img_name, apply_vbmeta, fp);
}

std::unique_ptr<RebootTask> ParseRebootCommand(const FlashingPlan* fp,
                                               const std::vector<std::string>& parts) {
    if (parts.empty()) return std::make_unique<RebootTask>(fp);
    if (parts.size() > 1) {
        LOG(ERROR) << "unknown arguments in reboot {target} in fastboot-info.txt: "
                   << android::base::Join(parts, " ");
        return nullptr;
    }
    return std::make_unique<RebootTask>(fp, parts[0]);
}

std::unique_ptr<WipeTask> ParseWipeCommand(const FlashingPlan* fp,
                                           const std::vector<std::string>& parts) {
    if (parts.size() != 1) {
        LOG(ERROR) << "unknown arguments in erase {partition} in fastboot-info.txt: "
                   << android::base::Join(parts, " ");
        return nullptr;
    }
    return std::make_unique<WipeTask>(fp, parts[0]);
}

std::unique_ptr<Task> ParseFastbootInfoLine(const FlashingPlan* fp,
                                            const std::vector<std::string>& command) {
    if (command.size() == 0) {
        return nullptr;
    }
    std::unique_ptr<Task> task;

    if (command[0] == "flash") {
        task = ParseFlashCommand(fp, std::vector<std::string>{command.begin() + 1, command.end()});
    } else if (command[0] == "reboot") {
        task = ParseRebootCommand(fp, std::vector<std::string>{command.begin() + 1, command.end()});
    } else if (command[0] == "update-super" && command.size() == 1) {
        task = std::make_unique<UpdateSuperTask>(fp);
    } else if (command[0] == "erase" && command.size() == 2) {
        task = ParseWipeCommand(fp, std::vector<std::string>{command.begin() + 1, command.end()});
    }
    if (!task) {
        LOG(ERROR) << "unknown command parsing fastboot-info.txt line: "
                   << android::base::Join(command, " ");
    }
    return task;
}

bool AddResizeTasks(const FlashingPlan* fp, std::vector<std::unique_ptr<Task>>* tasks) {
    // expands "resize-partitions" into individual commands : resize {os_partition_1}, resize
    // {os_partition_2}, etc.
    std::vector<std::unique_ptr<Task>> resize_tasks;
    std::optional<size_t> loc;
    std::vector<char> contents;
    if (!fp->source->ReadFile("super_empty.img", &contents)) {
        return false;
    }
    auto metadata = android::fs_mgr::ReadFromImageBlob(contents.data(), contents.size());
    if (!metadata) {
        return false;
    }
    for (size_t i = 0; i < tasks->size(); i++) {
        if (auto flash_task = tasks->at(i)->AsFlashTask()) {
            if (FlashTask::IsDynamicPartition(fp->source.get(), flash_task)) {
                if (!loc) {
                    loc = i;
                }
                resize_tasks.emplace_back(std::make_unique<ResizeTask>(
                        fp, flash_task->GetPartition(), "0", fp->slot_override));
            }
        }
    }
    // if no logical partitions (although should never happen since system will always need to be
    // flashed)
    if (!loc) {
        return false;
    }
    tasks->insert(tasks->begin() + loc.value(), std::make_move_iterator(resize_tasks.begin()),
                  std::make_move_iterator(resize_tasks.end()));
    return true;
}

static bool IsIgnore(const std::vector<std::string>& command) {
    if (command.size() == 0 || command[0][0] == '#') {
        return true;
    }
    return false;
}

bool CheckFastbootInfoRequirements(const std::vector<std::string>& command,
                                   uint32_t host_tool_version) {
    if (command.size() != 2) {
        LOG(ERROR) << "unknown characters in version info in fastboot-info.txt -> "
                   << android::base::Join(command, " ");
        return false;
    }
    if (command[0] != "version") {
        LOG(ERROR) << "unknown characters in version info in fastboot-info.txt -> "
                   << android::base::Join(command, " ");
        return false;
    }

    uint32_t fastboot_info_version;
    if (!android::base::ParseUint(command[1], &fastboot_info_version)) {
        LOG(ERROR) << "version number contains non-numeric characters in fastboot-info.txt -> "
                   << android::base::Join(command, " ");
        return false;
    }

    LOG(VERBOSE) << "Checking 'fastboot-info.txt version'";
    if (fastboot_info_version <= host_tool_version) {
        return true;
    }

    LOG(ERROR) << "fasboot-info.txt version: " << command[1]
               << " not compatible with host tool version --> " << host_tool_version;
    return false;
}

std::vector<std::unique_ptr<Task>> ParseFastbootInfo(const FlashingPlan* fp,
                                                     const std::vector<std::string>& file) {
    std::vector<std::unique_ptr<Task>> tasks;
    // Get os_partitions that need to be resized
    for (auto& text : file) {
        std::vector<std::string> command = android::base::Tokenize(text, " ");
        if (IsIgnore(command)) {
            continue;
        }
        if (command.size() > 1 && command[0] == "version") {
            if (!CheckFastbootInfoRequirements(command, FASTBOOT_INFO_VERSION)) {
                return {};
            }
            continue;
        } else if (command.size() >= 2 && command[0] == "if-wipe") {
            if (!fp->wants_wipe) {
                continue;
            }
            command.erase(command.begin());
        }
        auto task = ParseFastbootInfoLine(fp, command);
        if (!task) {
            return {};
        }
        tasks.emplace_back(std::move(task));
    }

    if (auto flash_super_task = OptimizedFlashSuperTask::Initialize(fp, tasks)) {
        tasks.emplace_back(std::move(flash_super_task));
    } else {
        if (!AddResizeTasks(fp, &tasks)) {
            LOG(WARNING) << "Failed to add resize tasks";
        }
    }

    return tasks;
}

FlashAllTool::FlashAllTool(FlashingPlan* fp) : fp_(fp) {}

void FlashAllTool::Flash() {
    DumpInfo();
    CheckRequirements();

    // Change the slot first, so we boot into the correct recovery image when
    // using fastbootd.
    if (fp_->slot_override == "all") {
        set_active("a");
    } else {
        set_active(fp_->slot_override);
    }

    DetermineSlot();

    CancelSnapshotIfNeeded();

    tasks_ = CollectTasks();

    for (auto& task : tasks_) {
        task->Run();
    }
    return;
}

std::vector<std::unique_ptr<Task>> FlashAllTool::CollectTasks() {
    std::vector<std::unique_ptr<Task>> tasks;
    if (fp_->should_use_fastboot_info) {
        tasks = CollectTasksFromFastbootInfo();

    } else {
        tasks = CollectTasksFromImageList();
    }
    if (fp_->exclude_dynamic_partitions) {
        auto is_non_static_flash_task = [&](const auto& task) -> bool {
            if (auto flash_task = task->AsFlashTask()) {
                if (!should_flash_in_userspace(fp_->source.get(),
                                               flash_task->GetPartitionAndSlot())) {
                    return false;
                }
            }
            return true;
        };
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(), is_non_static_flash_task),
                    tasks.end());
    }
    return tasks;
}

void FlashAllTool::CheckRequirements() {
    std::vector<char> contents;
    if (!fp_->source->ReadFile("android-info.txt", &contents)) {
        die("could not read android-info.txt");
    }
    ::CheckRequirements({contents.data(), contents.size()}, fp_->force_flash);
}

void FlashAllTool::DetermineSlot() {
    if (fp_->slot_override.empty()) {
        fp_->current_slot = get_current_slot();
    } else {
        fp_->current_slot = fp_->slot_override;
    }

    if (fp_->skip_secondary) {
        return;
    }
    if (fp_->slot_override != "" && fp_->slot_override != "all") {
        fp_->secondary_slot = get_other_slot(fp_->slot_override);
    } else {
        fp_->secondary_slot = get_other_slot();
    }
    if (fp_->secondary_slot == "") {
        if (supports_AB(fb)) {
            fprintf(stderr, "Warning: Could not determine slot for secondary images. Ignoring.\n");
        }
        fp_->skip_secondary = true;
    }
}

void FlashAllTool::CollectImages() {
    for (size_t i = 0; i < images.size(); ++i) {
        std::string slot = fp_->slot_override;
        if (images[i].IsSecondary()) {
            if (fp_->skip_secondary) {
                continue;
            }
            slot = fp_->secondary_slot;
        }
        if (images[i].type == ImageType::BootCritical) {
            boot_images_.emplace_back(&images[i], slot);
        } else if (images[i].type == ImageType::Normal) {
            os_images_.emplace_back(&images[i], slot);
        }
    }
}

std::vector<std::unique_ptr<Task>> FlashAllTool::CollectTasksFromImageList() {
    CollectImages();
    // First flash boot partitions. We allow this to happen either in userspace
    // or in bootloader fastboot.
    std::vector<std::unique_ptr<Task>> tasks;
    AddFlashTasks(boot_images_, tasks);

    // Sync the super partition. This will reboot to userspace fastboot if needed.
    tasks.emplace_back(std::make_unique<UpdateSuperTask>(fp_));

    AddFlashTasks(os_images_, tasks);

    if (auto flash_super_task = OptimizedFlashSuperTask::Initialize(fp_, tasks)) {
        tasks.emplace_back(std::move(flash_super_task));
    } else {
        // Resize any logical partition to 0, so each partition is reset to 0
        // extents, and will achieve more optimal allocation.
        if (!AddResizeTasks(fp_, &tasks)) {
            LOG(WARNING) << "Failed to add resize tasks";
        }
    }

    return tasks;
}

std::vector<std::unique_ptr<Task>> FlashAllTool::CollectTasksFromFastbootInfo() {
    std::vector<std::unique_ptr<Task>> tasks;
    std::vector<char> contents;
    if (!fp_->source->ReadFile("fastboot-info.txt", &contents)) {
        LOG(VERBOSE) << "Flashing from hardcoded images. fastboot-info.txt is empty or does not "
                        "exist";
        return CollectTasksFromImageList();
    }
    tasks = ParseFastbootInfo(fp_, Split({contents.data(), contents.size()}, "\n"));
    return tasks;
}

void FlashAllTool::AddFlashTasks(const std::vector<std::pair<const Image*, std::string>>& images,
                                 std::vector<std::unique_ptr<Task>>& tasks) {
    for (const auto& [image, slot] : images) {
        fastboot_buffer buf;
        unique_fd fd = fp_->source->OpenFile(image->img_name);
        if (fd < 0 || !load_buf_fd(std::move(fd), &buf, fp_)) {
            if (image->optional_if_no_image) {
                continue;
            }
            die("could not load '%s': %s", image->img_name.c_str(), strerror(errno));
        }
        tasks.emplace_back(std::make_unique<FlashTask>(slot, image->part_name, image->img_name,
                                                       is_vbmeta_partition(image->part_name), fp_));
    }
}

bool ZipImageSource::ReadFile(const std::string& name, std::vector<char>* out) const {
    return UnzipToMemory(zip_, name.c_str(), out);
}

unique_fd ZipImageSource::OpenFile(const std::string& name) const {
    return UnzipToFile(zip_, name.c_str());
}

static void do_update(const char* filename, FlashingPlan* fp) {
    int err;
    unique_zip_t zip(zip_open(filename, ZIP_RDONLY, &err), zip_close);
    if (err != 0) {
        zip_error_t error;
        zip_error_init_with_code(&error, err);
        die("failed to open zip file '%s': %s", filename, zip_error_strerror(&error));
    }
    fp->source.reset(new ZipImageSource(zip));
    FlashAllTool tool(fp);
    tool.Flash();
}

bool LocalImageSource::ReadFile(const std::string& name, std::vector<char>* out) const {
    auto path = find_item_given_name(name);
    if (path.empty()) {
        return false;
    }
    return ReadFileToVector(path, out);
}

unique_fd LocalImageSource::OpenFile(const std::string& name) const {
    auto path = find_item_given_name(name);
    return unique_fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_BINARY)));
}

static void do_flashall(FlashingPlan* fp) {
    fp->source.reset(new LocalImageSource());
    FlashAllTool tool(fp);
    tool.Flash();
}

static std::string next_arg(std::vector<std::string>* args) {
    if (args->empty()) syntax_error("expected argument");
    std::string result = args->front();
    args->erase(args->begin());
    return result;
}

static void do_oem_command(const std::string& cmd, std::vector<std::string>* args) {
    if (args->empty()) syntax_error("empty oem command");

    std::string command(cmd);
    while (!args->empty()) {
        command += " " + next_arg(args);
    }
    fb->RawCommand(command, "");
}

static unsigned fb_get_flash_block_size(std::string name) {
    std::string sizeString;
    if (fb->GetVar(name, &sizeString) != fastboot::SUCCESS || sizeString.empty()) {
        // This device does not report flash block sizes, so return 0.
        return 0;
    }
    sizeString = fb_fix_numeric_var(sizeString);

    unsigned size;
    if (!android::base::ParseUint(sizeString, &size)) {
        fprintf(stderr, "Couldn't parse %s '%s'.\n", name.c_str(), sizeString.c_str());
        return 0;
    }
    if ((size & (size - 1)) != 0) {
        fprintf(stderr, "Invalid %s %u: must be a power of 2.\n", name.c_str(), size);
        return 0;
    }
    return size;
}

void fb_perform_format(const std::string& partition, int skip_if_not_supported,
                       const std::string& type_override, const std::string& size_override,
                       const unsigned fs_options, const FlashingPlan* fp) {
    std::string partition_type, partition_size;

    struct fastboot_buffer buf;
    const char* errMsg = nullptr;
    const struct fs_generator* gen = nullptr;
    TemporaryFile output;
    unique_fd fd;

    unsigned int limit = INT_MAX;
    if (target_sparse_limit > 0 && target_sparse_limit < limit) {
        limit = target_sparse_limit;
    }
    if (fp->sparse_limit > 0 && fp->sparse_limit < limit) {
        limit = fp->sparse_limit;
    }

    if (fb->GetVar("partition-type:" + partition, &partition_type) != fastboot::SUCCESS) {
        errMsg = "Can't determine partition type.\n";
        goto failed;
    }
    if (!type_override.empty()) {
        if (partition_type != type_override) {
            fprintf(stderr, "Warning: %s type is %s, but %s was requested for formatting.\n",
                    partition.c_str(), partition_type.c_str(), type_override.c_str());
        }
        partition_type = type_override;
    }

    if (fb->GetVar("partition-size:" + partition, &partition_size) != fastboot::SUCCESS) {
        errMsg = "Unable to get partition size\n";
        goto failed;
    }
    if (!size_override.empty()) {
        if (partition_size != size_override) {
            fprintf(stderr, "Warning: %s size is %s, but %s was requested for formatting.\n",
                    partition.c_str(), partition_size.c_str(), size_override.c_str());
        }
        partition_size = size_override;
    }
    partition_size = fb_fix_numeric_var(partition_size);

    gen = fs_get_generator(partition_type);
    if (!gen) {
        if (skip_if_not_supported) {
            fprintf(stderr, "Erase successful, but not automatically formatting.\n");
            fprintf(stderr, "File system type %s not supported.\n", partition_type.c_str());
            return;
        }
        die("Formatting is not supported for file system with type '%s'.", partition_type.c_str());
    }

    int64_t size;
    if (!android::base::ParseInt(partition_size, &size)) {
        die("Couldn't parse partition size '%s'.", partition_size.c_str());
    }

    unsigned eraseBlkSize, logicalBlkSize;
    eraseBlkSize = fb_get_flash_block_size("erase-block-size");
    logicalBlkSize = fb_get_flash_block_size("logical-block-size");

    if (fs_generator_generate(gen, output.path, size, eraseBlkSize, logicalBlkSize, fs_options)) {
        die("Cannot generate image for %s", partition.c_str());
    }

    fd.reset(open(output.path, O_RDONLY));
    if (fd == -1) {
        die("Cannot open generated image: %s", strerror(errno));
    }
    if (!load_buf_fd(std::move(fd), &buf, fp)) {
        die("Cannot read image: %s", strerror(errno));
    }

    flash_buf(fp->source.get(), partition, &buf, is_vbmeta_partition(partition));
    return;

failed:
    if (skip_if_not_supported) {
        fprintf(stderr, "Erase successful, but not automatically formatting.\n");
        if (errMsg) fprintf(stderr, "%s", errMsg);
    }
    fprintf(stderr, "FAILED (%s)\n", fb->Error().c_str());
    if (!skip_if_not_supported) {
        die("Command failed");
    }
}

bool should_flash_in_userspace(const ImageSource* source, const std::string& partition_name) {
    if (!source) {
        if (!get_android_product_out()) {
            return false;
        }
        auto path = find_item_given_name("super_empty.img");
        if (path.empty() || access(path.c_str(), R_OK)) {
            return false;
        }
        auto metadata = android::fs_mgr::ReadFromImageFile(path);
        if (!metadata) {
            return false;
        }
        return should_flash_in_userspace(*metadata.get(), partition_name);
    }
    std::vector<char> contents;
    if (!source->ReadFile("super_empty.img", &contents)) {
        return false;
    }
    auto metadata = android::fs_mgr::ReadFromImageBlob(contents.data(), contents.size());
    return should_flash_in_userspace(*metadata.get(), partition_name);
}

static bool wipe_super(const android::fs_mgr::LpMetadata& metadata, const std::string& slot,
                       std::string* message, const FlashingPlan* fp) {
    auto super_device = GetMetadataSuperBlockDevice(metadata);
    auto block_size = metadata.geometry.logical_block_size;
    auto super_bdev_name = android::fs_mgr::GetBlockDevicePartitionName(*super_device);

    if (super_bdev_name != "super") {
        // retrofit devices do not allow flashing to the retrofit partitions,
        // so enable it if we can.
        fb->RawCommand("oem allow-flash-super");
    }

    // Note: do not use die() in here, since we want TemporaryDir's destructor
    // to be called.
    TemporaryDir temp_dir;

    bool ok;
    if (metadata.block_devices.size() > 1) {
        ok = WriteSplitImageFiles(temp_dir.path, metadata, block_size, {}, true);
    } else {
        auto image_path = std::string(temp_dir.path) + "/" + std::string(super_bdev_name) + ".img";
        ok = WriteToImageFile(image_path, metadata, block_size, {}, true);
    }
    if (!ok) {
        *message = "Could not generate a flashable super image file";
        return false;
    }

    for (const auto& block_device : metadata.block_devices) {
        auto partition = android::fs_mgr::GetBlockDevicePartitionName(block_device);
        bool force_slot = !!(block_device.flags & LP_BLOCK_DEVICE_SLOT_SUFFIXED);

        std::string image_name;
        if (metadata.block_devices.size() > 1) {
            image_name = "super_" + partition + ".img";
        } else {
            image_name = partition + ".img";
        }

        auto image_path = std::string(temp_dir.path) + "/" + image_name;
        auto flash = [&](const std::string& partition_name) {
            do_flash(partition_name.c_str(), image_path.c_str(), false, fp);
        };
        do_for_partitions(partition, slot, flash, force_slot);

        unlink(image_path.c_str());
    }
    return true;
}

static void do_wipe_super(const std::string& image, const std::string& slot_override,
                          const FlashingPlan* fp) {
    if (access(image.c_str(), R_OK) != 0) {
        die("Could not read image: %s", image.c_str());
    }
    auto metadata = android::fs_mgr::ReadFromImageFile(image);
    if (!metadata) {
        die("Could not parse image: %s", image.c_str());
    }

    auto slot = slot_override;
    if (slot.empty()) {
        slot = get_current_slot();
    }

    std::string message;
    if (!wipe_super(*metadata.get(), slot, &message, fp)) {
        die(message);
    }
}

static void FastbootLogger(android::base::LogId /* id */, android::base::LogSeverity severity,
                           const char* /* tag */, const char* /* file */, unsigned int /* line */,
                           const char* message) {
    switch (severity) {
        case android::base::INFO:
            fprintf(stdout, "%s\n", message);
            break;
        case android::base::ERROR:
            fprintf(stderr, "%s\n", message);
            break;
        default:
            verbose("%s\n", message);
    }
}

static void FastbootAborter(const char* message) {
    die("%s", message);
}

int FastBootTool::Main(int argc, char* argv[]) {
    android::base::InitLogging(argv, FastbootLogger, FastbootAborter);
    std::unique_ptr<FlashingPlan> fp = std::make_unique<FlashingPlan>();

    int longindex;
    std::string next_active;

    g_boot_img_hdr.kernel_addr = 0x00008000;
    g_boot_img_hdr.ramdisk_addr = 0x01000000;
    g_boot_img_hdr.second_addr = 0x00f00000;
    g_boot_img_hdr.tags_addr = 0x00000100;
    g_boot_img_hdr.page_size = 2048;
    g_boot_img_hdr.dtb_addr = 0x01100000;

    const struct option longopts[] = {{"base", required_argument, 0, 0},
                                      {"cmdline", required_argument, 0, 0},
                                      {"disable-verification", no_argument, 0, 0},
                                      {"disable-verity", no_argument, 0, 0},
                                      {"disable-super-optimization", no_argument, 0, 0},
                                      {"exclude-dynamic-partitions", no_argument, 0, 0},
                                      {"disable-fastboot-info", no_argument, 0, 0},
                                      {"force", no_argument, 0, 0},
                                      {"fs-options", required_argument, 0, 0},
                                      {"header-version", required_argument, 0, 0},
                                      {"help", no_argument, 0, 'h'},
                                      {"kernel-offset", required_argument, 0, 0},
                                      {"os-patch-level", required_argument, 0, 0},
                                      {"os-version", required_argument, 0, 0},
                                      {"page-size", required_argument, 0, 0},
                                      {"ramdisk-offset", required_argument, 0, 0},
                                      {"set-active", optional_argument, 0, 'a'},
                                      {"skip-reboot", no_argument, 0, 0},
                                      {"skip-secondary", no_argument, 0, 0},
                                      {"slot", required_argument, 0, 0},
                                      {"tags-offset", required_argument, 0, 0},
                                      {"dtb", required_argument, 0, 0},
                                      {"dtb-offset", required_argument, 0, 0},
                                      {"unbuffered", no_argument, 0, 0},
                                      {"verbose", no_argument, 0, 'v'},
                                      {"version", no_argument, 0, 0},
                                      {0, 0, 0, 0}};

    serial = getenv("FASTBOOT_DEVICE");
    if (!serial) {
        serial = getenv("ANDROID_SERIAL");
    }

    int c;
    while ((c = getopt_long(argc, argv, "a::hls:S:vw", longopts, &longindex)) != -1) {
        if (c == 0) {
            std::string name{longopts[longindex].name};
            if (name == "base") {
                g_base_addr = strtoul(optarg, 0, 16);
            } else if (name == "cmdline") {
                g_cmdline = optarg;
            } else if (name == "disable-verification") {
                g_disable_verification = true;
            } else if (name == "disable-verity") {
                g_disable_verity = true;
            } else if (name == "disable-super-optimization") {
                fp->should_optimize_flash_super = false;
            } else if (name == "exclude-dynamic-partitions") {
                fp->exclude_dynamic_partitions = true;
                fp->should_optimize_flash_super = false;
            } else if (name == "disable-fastboot-info") {
                fp->should_use_fastboot_info = false;
            } else if (name == "force") {
                fp->force_flash = true;
            } else if (name == "fs-options") {
                fp->fs_options = ParseFsOption(optarg);
            } else if (name == "header-version") {
                g_boot_img_hdr.header_version = strtoul(optarg, nullptr, 0);
            } else if (name == "dtb") {
                g_dtb_path = optarg;
            } else if (name == "kernel-offset") {
                g_boot_img_hdr.kernel_addr = strtoul(optarg, 0, 16);
            } else if (name == "os-patch-level") {
                ParseOsPatchLevel(&g_boot_img_hdr, optarg);
            } else if (name == "os-version") {
                ParseOsVersion(&g_boot_img_hdr, optarg);
            } else if (name == "page-size") {
                g_boot_img_hdr.page_size = strtoul(optarg, nullptr, 0);
                if (g_boot_img_hdr.page_size == 0) die("invalid page size");
            } else if (name == "ramdisk-offset") {
                g_boot_img_hdr.ramdisk_addr = strtoul(optarg, 0, 16);
            } else if (name == "skip-reboot") {
                fp->skip_reboot = true;
            } else if (name == "skip-secondary") {
                fp->skip_secondary = true;
            } else if (name == "slot") {
                fp->slot_override = optarg;
            } else if (name == "dtb-offset") {
                g_boot_img_hdr.dtb_addr = strtoul(optarg, 0, 16);
            } else if (name == "tags-offset") {
                g_boot_img_hdr.tags_addr = strtoul(optarg, 0, 16);
            } else if (name == "unbuffered") {
                setvbuf(stdout, nullptr, _IONBF, 0);
                setvbuf(stderr, nullptr, _IONBF, 0);
            } else if (name == "version") {
                fprintf(stdout, "fastboot version %s-%s\n", PLATFORM_TOOLS_VERSION,
                        android::build::GetBuildNumber().c_str());
                fprintf(stdout, "Installed as %s\n", android::base::GetExecutablePath().c_str());
                return 0;
            } else {
                die("unknown option %s", longopts[longindex].name);
            }
        } else {
            switch (c) {
                case 'a':
                    fp->wants_set_active = true;
                    if (optarg) next_active = optarg;
                    break;
                case 'h':
                    return show_help();
                case 'l':
                    g_long_listing = true;
                    break;
                case 's':
                    serial = optarg;
                    break;
                case 'S':
                    if (!android::base::ParseByteCount(optarg, &fp->sparse_limit)) {
                        die("invalid sparse limit %s", optarg);
                    }
                    break;
                case 'v':
                    set_verbose();
                    break;
                case 'w':
                    fp->wants_wipe = true;
                    break;
                case '?':
                    return 1;
                default:
                    abort();
            }
        }
    }

    argc -= optind;
    argv += optind;

    if (argc == 0 && !fp->wants_wipe && !fp->wants_set_active) syntax_error("no command");

    if (argc > 0 && !strcmp(*argv, "devices")) {
        list_devices();
        return 0;
    }

    if (argc > 0 && !strcmp(*argv, "connect")) {
        argc -= optind;
        argv += optind;
        return Connect(argc, argv);
    }

    if (argc > 0 && !strcmp(*argv, "disconnect")) {
        argc -= optind;
        argv += optind;
        return Disconnect(argc, argv);
    }

    if (argc > 0 && !strcmp(*argv, "help")) {
        return show_help();
    }

    std::unique_ptr<Transport> transport = open_device();
    if (!transport) {
        return 1;
    }
    fastboot::DriverCallbacks driver_callbacks = {
            .prolog = Status,
            .epilog = Epilog,
            .info = InfoMessage,
            .text = TextMessage,
    };

    fastboot::FastBootDriver fastboot_driver(std::move(transport), driver_callbacks, false);
    fb = &fastboot_driver;
    fp->fb = &fastboot_driver;

    const double start = now();

    if (fp->slot_override != "") fp->slot_override = verify_slot(fp->slot_override);
    if (next_active != "") next_active = verify_slot(next_active, false);

    if (fp->wants_set_active) {
        if (next_active == "") {
            if (fp->slot_override == "") {
                std::string current_slot;
                if (fb->GetVar("current-slot", &current_slot) == fastboot::SUCCESS) {
                    if (current_slot[0] == '_') current_slot.erase(0, 1);
                    next_active = verify_slot(current_slot, false);
                } else {
                    fp->wants_set_active = false;
                }
            } else {
                next_active = verify_slot(fp->slot_override, false);
            }
        }
    }
    std::vector<std::unique_ptr<Task>> tasks;
    std::vector<std::string> args(argv, argv + argc);
    while (!args.empty()) {
        std::string command = next_arg(&args);

        if (command == FB_CMD_GETVAR) {
            std::string variable = next_arg(&args);
            DisplayVarOrError(variable, variable);
        } else if (command == FB_CMD_ERASE) {
            std::string partition = next_arg(&args);
            auto erase = [&](const std::string& partition) {
                std::string partition_type;
                if (fb->GetVar("partition-type:" + partition, &partition_type) ==
                            fastboot::SUCCESS &&
                    fs_get_generator(partition_type) != nullptr) {
                    fprintf(stderr, "******** Did you mean to fastboot format this %s partition?\n",
                            partition_type.c_str());
                }

                fb->Erase(partition);
            };
            do_for_partitions(partition, fp->slot_override, erase, true);
        } else if (android::base::StartsWith(command, "format")) {
            // Parsing for: "format[:[type][:[size]]]"
            // Some valid things:
            //  - select only the size, and leave default fs type:
            //    format::0x4000000 userdata
            //  - default fs type and size:
            //    format userdata
            //    format:: userdata
            std::vector<std::string> pieces = android::base::Split(command, ":");
            std::string type_override;
            if (pieces.size() > 1) type_override = pieces[1].c_str();
            std::string size_override;
            if (pieces.size() > 2) size_override = pieces[2].c_str();

            std::string partition = next_arg(&args);

            auto format = [&](const std::string& partition) {
                fb_perform_format(partition, 0, type_override, size_override, fp->fs_options,
                                  fp.get());
            };
            do_for_partitions(partition, fp->slot_override, format, true);
        } else if (command == "signature") {
            std::string filename = next_arg(&args);
            std::vector<char> data;
            if (!ReadFileToVector(filename, &data)) {
                die("could not load '%s': %s", filename.c_str(), strerror(errno));
            }
            if (data.size() != 256) die("signature must be 256 bytes (got %zu)", data.size());
            fb->Download("signature", data);
            fb->RawCommand("signature", "installing signature");
        } else if (command == FB_CMD_REBOOT) {
            if (args.size() == 1) {
                std::string reboot_target = next_arg(&args);
                tasks.emplace_back(std::make_unique<RebootTask>(fp.get(), reboot_target));
            } else if (!fp->skip_reboot) {
                tasks.emplace_back(std::make_unique<RebootTask>(fp.get()));
            }
            if (!args.empty()) syntax_error("junk after reboot command");
        } else if (command == FB_CMD_REBOOT_BOOTLOADER) {
            tasks.emplace_back(std::make_unique<RebootTask>(fp.get(), "bootloader"));
        } else if (command == FB_CMD_REBOOT_RECOVERY) {
            tasks.emplace_back(std::make_unique<RebootTask>(fp.get(), "recovery"));
        } else if (command == FB_CMD_REBOOT_FASTBOOT) {
            tasks.emplace_back(std::make_unique<RebootTask>(fp.get(), "fastboot"));
        } else if (command == FB_CMD_CONTINUE) {
            fb->Continue();
        } else if (command == FB_CMD_BOOT) {
            std::string kernel = next_arg(&args);
            std::string ramdisk;
            if (!args.empty()) ramdisk = next_arg(&args);
            std::string second_stage;
            if (!args.empty()) second_stage = next_arg(&args);
            auto data = LoadBootableImage(kernel, ramdisk, second_stage);
            fb->Download("boot.img", data);
            fb->Boot();
        } else if (command == FB_CMD_FLASH) {
            std::string pname = next_arg(&args);
            std::string fname;
            if (!args.empty()) {
                fname = next_arg(&args);
            } else {
                fname = find_item(pname);
            }
            if (fname.empty()) die("cannot determine image filename for '%s'", pname.c_str());

            FlashTask task(fp->slot_override, pname, fname, is_vbmeta_partition(pname), fp.get());
            task.Run();
        } else if (command == "flash:raw") {
            std::string partition = next_arg(&args);
            std::string kernel = next_arg(&args);
            std::string ramdisk;
            if (!args.empty()) ramdisk = next_arg(&args);
            std::string second_stage;
            if (!args.empty()) second_stage = next_arg(&args);

            auto data = LoadBootableImage(kernel, ramdisk, second_stage);
            auto flashraw = [&data](const std::string& partition) {
                fb->FlashPartition(partition, data);
            };
            do_for_partitions(partition, fp->slot_override, flashraw, true);
        } else if (command == "flashall") {
            if (fp->slot_override == "all") {
                fprintf(stderr,
                        "Warning: slot set to 'all'. Secondary slots will not be flashed.\n");
                fp->skip_secondary = true;
            }
            do_flashall(fp.get());

            if (!fp->skip_reboot) {
                tasks.emplace_back(std::make_unique<RebootTask>(fp.get()));
            }
        } else if (command == "update") {
            bool slot_all = (fp->slot_override == "all");
            if (slot_all) {
                fprintf(stderr,
                        "Warning: slot set to 'all'. Secondary slots will not be flashed.\n");
            }
            std::string filename = "update.zip";
            if (!args.empty()) {
                filename = next_arg(&args);
            }
            do_update(filename.c_str(), fp.get());
            if (!fp->skip_reboot) {
                tasks.emplace_back(std::make_unique<RebootTask>(fp.get()));
            }
        } else if (command == FB_CMD_SET_ACTIVE) {
            std::string slot = verify_slot(next_arg(&args), false);
            fb->SetActive(slot);
        } else if (command == "stage") {
            std::string filename = next_arg(&args);

            struct fastboot_buffer buf;
            if (!load_buf(filename.c_str(), &buf, fp.get()) || buf.type != FB_BUFFER_FD) {
                die("cannot load '%s'", filename.c_str());
            }
            fb->Download(filename, buf.fd.get(), buf.sz);
        } else if (command == "get_staged") {
            std::string filename = next_arg(&args);
            fb->Upload(filename);
        } else if (command == FB_CMD_OEM) {
            do_oem_command(FB_CMD_OEM, &args);
        } else if (command == "flashing") {
            if (args.empty()) {
                syntax_error("missing 'flashing' command");
            } else if (args.size() == 1 &&
                       (args[0] == "unlock" || args[0] == "lock" || args[0] == "unlock_critical" ||
                        args[0] == "lock_critical" || args[0] == "get_unlock_ability")) {
                do_oem_command("flashing", &args);
            } else {
                syntax_error("unknown 'flashing' command %s", args[0].c_str());
            }
        } else if (command == FB_CMD_CREATE_PARTITION) {
            std::string partition = next_arg(&args);
            std::string size = next_arg(&args);
            fb->CreatePartition(partition, size);
        } else if (command == FB_CMD_DELETE_PARTITION) {
            std::string partition = next_arg(&args);
            tasks.emplace_back(std::make_unique<DeleteTask>(fp.get(), partition));
        } else if (command == FB_CMD_RESIZE_PARTITION) {
            std::string partition = next_arg(&args);
            std::string size = next_arg(&args);
            std::unique_ptr<ResizeTask> resize_task =
                    std::make_unique<ResizeTask>(fp.get(), partition, size, fp->slot_override);
            resize_task->Run();
        } else if (command == "gsi") {
            if (args.empty()) syntax_error("invalid gsi command");
            std::string cmd("gsi");
            while (!args.empty()) {
                cmd += ":" + next_arg(&args);
            }
            fb->RawCommand(cmd, "");
        } else if (command == "wipe-super") {
            std::string image;
            if (args.empty()) {
                image = find_item_given_name("super_empty.img");
            } else {
                image = next_arg(&args);
            }
            do_wipe_super(image, fp->slot_override, fp.get());
        } else if (command == "snapshot-update") {
            std::string arg;
            if (!args.empty()) {
                arg = next_arg(&args);
            }
            if (!arg.empty() && (arg != "cancel" && arg != "merge")) {
                syntax_error("expected: snapshot-update [cancel|merge]");
            }
            fb->SnapshotUpdateCommand(arg);
        } else if (command == FB_CMD_FETCH) {
            std::string partition = next_arg(&args);
            std::string outfile = next_arg(&args);
            do_fetch(partition, fp->slot_override, outfile, fp->fb);
        } else {
            syntax_error("unknown command %s", command.c_str());
        }
    }

    if (fp->wants_wipe) {
        if (fp->force_flash) {
            CancelSnapshotIfNeeded();
        }
        std::vector<std::unique_ptr<Task>> wipe_tasks;
        std::vector<std::string> partitions = {"userdata", "cache", "metadata"};
        for (const auto& partition : partitions) {
            wipe_tasks.emplace_back(std::make_unique<WipeTask>(fp.get(), partition));
        }
        tasks.insert(tasks.begin(), std::make_move_iterator(wipe_tasks.begin()),
                     std::make_move_iterator(wipe_tasks.end()));
    }
    if (fp->wants_set_active) {
        fb->SetActive(next_active);
    }
    for (auto& task : tasks) {
        task->Run();
    }
    fprintf(stderr, "Finished. Total time: %.3fs\n", (now() - start));

    return 0;
}

void FastBootTool::ParseOsPatchLevel(boot_img_hdr_v1* hdr, const char* arg) {
    unsigned year, month, day;
    if (sscanf(arg, "%u-%u-%u", &year, &month, &day) != 3) {
        syntax_error("OS patch level should be YYYY-MM-DD: %s", arg);
    }
    if (year < 2000 || year >= 2128) syntax_error("year out of range: %d", year);
    if (month < 1 || month > 12) syntax_error("month out of range: %d", month);
    hdr->SetOsPatchLevel(year, month);
}

void FastBootTool::ParseOsVersion(boot_img_hdr_v1* hdr, const char* arg) {
    unsigned major = 0, minor = 0, patch = 0;
    std::vector<std::string> versions = android::base::Split(arg, ".");
    if (versions.size() < 1 || versions.size() > 3 ||
        (versions.size() >= 1 && !android::base::ParseUint(versions[0], &major)) ||
        (versions.size() >= 2 && !android::base::ParseUint(versions[1], &minor)) ||
        (versions.size() == 3 && !android::base::ParseUint(versions[2], &patch)) ||
        (major > 0x7f || minor > 0x7f || patch > 0x7f)) {
        syntax_error("bad OS version: %s", arg);
    }
    hdr->SetOsVersion(major, minor, patch);
}

unsigned FastBootTool::ParseFsOption(const char* arg) {
    unsigned fsOptions = 0;

    std::vector<std::string> options = android::base::Split(arg, ",");
    if (options.size() < 1) syntax_error("bad options: %s", arg);

    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i] == "casefold")
            fsOptions |= (1 << FS_OPT_CASEFOLD);
        else if (options[i] == "projid")
            fsOptions |= (1 << FS_OPT_PROJID);
        else if (options[i] == "compress")
            fsOptions |= (1 << FS_OPT_COMPRESS);
        else
            syntax_error("unsupported options: %s", options[i].c_str());
    }
    return fsOptions;
}
