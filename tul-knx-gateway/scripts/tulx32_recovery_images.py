"""TULX32: this env only builds; every cable target is refused.

The shop layout (partitions_tulx32_recovery.csv) keeps a recovery system in
factory, started by a bootloader with an S1 check; bootloader, factory and
prov are fixed at delivery. PlatformIO's cable targets would destroy that:
upload writes the framework bootloader at 0x0 (the builder copies
FLASH_EXTRA_IMAGES into the esptool arguments before post scripts run, so the
recovery bootloader cannot be swapped in here), and erase / erase_upload wipe
the whole flash. The filesystem uploads are refused along with them.

Install firmware.bin from the recovery ("Install from a file"). A full image
for a cable flash in the lab comes from scripts/build_factory.sh and goes on
with esptool.
"""
Import("env")  # noqa: F821

CABLE_TARGETS = ("upload", "erase_upload", "erase", "uploadfs", "uploadfsota")


def refuse_cable_flash(source, target, env):
    print("tulx32_recovery_images: cable flashing is disabled for this env -- "
          "upload would replace the recovery bootloader, erase and erase_upload "
          "would wipe the recovery. Install firmware.bin from the recovery "
          "(\"Install from a file\"); for a full lab image use "
          "scripts/build_factory.sh and esptool.")
    return 1


for name in CABLE_TARGETS:
    env.AddPreAction(name, refuse_cable_flash)
