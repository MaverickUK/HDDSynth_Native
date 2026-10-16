# PlatformIO extra script: make the boot wait for the USB serial console
# (WAIT_FOR_SERIAL_MS, see main.cpp) only when uploading over USB, i.e. when
# debugging from this machine. Plain builds, including GitHub Actions and
# the release UF2s, boot straight away.
#
# Added to projenv (our sources only), not env, so switching between build
# and upload recompiles a few files rather than the whole Arduino core.
Import("projenv")

WAIT_MS = 1500

if "upload" in COMMAND_LINE_TARGETS:
    projenv.Append(CPPDEFINES=[("WAIT_FOR_SERIAL_MS", WAIT_MS)])
    print("USB upload: boot waits up to %d ms for the serial console (debug)" % WAIT_MS)
