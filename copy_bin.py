import shutil
Import("env")

def after_build(source, target, env):
    firmware_path = str(target[0])
    shutil.copy(firmware_path, "firmware.bin")
    print("Firmware copied to firmware.bin")

env.AddPostAction("buildprog", after_build)