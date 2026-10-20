Import("env")
import os, re, shutil, json, datetime

FIRMWARE_NAMES = {
    "esp32-nodisp":     "ESP32_NODISP",
    "esp32-sh1106":     "ESP32_SH1106",
    "esp32-ssd1306":    "ESP32_SSD1306",
    "esp32-noota":      "ESP32_NOOTA",
    "esp32c3-nodisp":   "ESP32C3_NODISP",
    "esp32c3-noota":    "ESP32C3_NOOTA",
    "esp32c3-sh1106":   "ESP32C3_SH1106",
    "esp32c3-ssd1306":  "ESP32C3_SSD1306",
    "esp32s3-sh1106":   "ESP32S3_SH1106",
    "esp32s3-noota":    "ESP32S3_NOOTA",
    "esp32s3-N16R8":    "ESP32S3_N16R8",
    "TTGO-TWR":         "ESP32S3_TWR",
}

def copy_firmware(source, target, env):
    # Extract VERSION and VERSION_BUILD from SCons CPPDEFINES
    defines = {}
    for item in env.get("CPPDEFINES", []):
        if isinstance(item, (list, tuple)) and len(item) == 2:
            defines[str(item[0])] = str(item[1])

    version_raw = re.sub(r'[\\"\']', '', defines.get("VERSION", "0.0"))
    version = version_raw.replace(".", "")
    version_build = re.sub(r'[\\"\']', '', defines.get("VERSION_BUILD", "a"))

    env_name = env["PIOENV"]
    prefix = FIRMWARE_NAMES.get(env_name, env_name)

    output_dir = os.path.join(env.subst("$PROJECT_DIR"), ".pio", "build", "firmware")
    os.makedirs(output_dir, exist_ok=True)

    src = str(target[0])
    dst = os.path.join(output_dir, "{}_v{}-{}.bin".format(prefix, version_raw, version_build))

    shutil.copy2(src, dst)
    print("\n*** Firmware saved: {} ***\n".format(dst))

    # LU6JMF - Marcelo Ferreira
    # Also drop a copy in a plain, visible folder at the project root (not
    # inside the dot-hidden .pio/) so it's easy to find in Finder without
    # toggling hidden files.
    easy_dir = os.path.join(env.subst("$PROJECT_DIR"), "firmware_releases")
    os.makedirs(easy_dir, exist_ok=True)
    easy_dst = os.path.join(easy_dir, os.path.basename(dst))
    shutil.copy2(src, easy_dst)
    print("*** Firmware also saved (easy-to-find copy): {} ***\n".format(easy_dst))

    # LU6JMF 2.1.6: keep the firmware.elf of every build (newest 10). It is needed to turn the
    # LAST CRASH addresses shown on /txdebug into source file:line (xtensa addr2line).
    try:
        elf_src = os.path.join(os.path.dirname(src), "firmware.elf")
        elf_dir = os.path.join(easy_dir, "elf")
        os.makedirs(elf_dir, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        elf_dst = os.path.join(elf_dir, "{}_v{}-{}_{}.elf".format(prefix, version_raw, version_build, stamp))
        shutil.copy2(elf_src, elf_dst)
        olds = sorted(f for f in os.listdir(elf_dir) if f.startswith(prefix + "_") and f.endswith(".elf"))
        for f in olds[:-10]:
            os.remove(os.path.join(elf_dir, f))
        print("*** ELF kept for crash decoding: {} ***\n".format(elf_dst))
    except Exception as e:
        print("*** Could not keep the ELF: {} ***\n".format(e))

    # Write version info (used by webservice.cpp handle_check_version to detect
    # new firmware releases). JSON is used instead of plain text so it can be
    # parsed easily with ArduinoJson on the device.
    tag = "v{}-{}".format(version_raw, version_build)
    filename = "{}_{}.bin".format(prefix, tag)
    today = datetime.date.today().isoformat()
    version_info = {
        "version": version_raw,
        "build": version_build,
        "date": today,
        "tag": tag,
        "filename": filename,
        # Filesystem/icons version: bumped independently by hand when the
        # data/ folder (icons) actually changes - kept equal to the firmware
        # version by default, edit manually if only icons change.
        "fs_version": version_raw,
        "fs_build": version_build,
        "fs_date": today,
        "fs_filename": "littlefs.bin",
    }
    version_info_path = os.path.join(output_dir, "version.json")
    with open(version_info_path, "w") as f:
        json.dump(version_info, f, indent=2)
    print("*** Version info saved: {} ***\n".format(version_info_path))

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)
