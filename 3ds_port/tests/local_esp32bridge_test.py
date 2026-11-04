"""Host test of the ESP32 bridge firmware's 3DS leg: builds the game side
(src/3ds_local.c, wire-only) exactly as local_udp_test.py does, builds the
firmware's state machine for the host (tools/frlg_bridge/esp32/main/bridge.c
with test/bridge_host.c, none of the board's Wi-Fi code), runs the shim as
a program, and expects the same join, the adapter's PEER_* exchange and a
burst of messages to come back clean. What the test cannot see is the
board itself: the softAP, and the Switch leg (docs/frlg-switch-compatibility).
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent  # 3ds_port
REPO = PORT.parent
ESP32 = REPO / "tools" / "frlg_bridge" / "esp32"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--keep-going", action="store_true",
                        help="run the test even if the compile printed warnings")
    args = parser.parse_args()

    binary = PORT / "build" / "local_esp32bridge_test"
    shim = PORT / "build" / "esp32_bridge_host"
    binary.parent.mkdir(parents=True, exist_ok=True)

    # The air's builds (uds, both) cannot run here, but they must compile:
    # the declarations-only UDS in tests/local_host/3ds.h is enough for that.
    for name, flags in [("uds", []), ("both", ["-DLOCAL_LINK_UDP"])]:
        check = [args.cc, "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                 *flags, "-fsyntax-only", "-Iinclude", "-Itests/local_host",
                 "src/3ds_local.c"]
        print("$ %s" % " ".join(check), flush=True)
        built = subprocess.run(check, cwd=PORT)
        if built.returncode:
            return built.returncode

    game = [args.cc, "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DLOCAL_LINK_UDP", "-DLOCAL_LINK_NO_UDS", "-Iinclude", "-Itests/local_host",
            "src/3ds_local.c", "tests/local_udp_test.c", "-pthread",
            "-o", str(binary)]
    print("$ %s" % " ".join(game), flush=True)
    build = subprocess.run(game, cwd=PORT)
    if build.returncode:
        return build.returncode

    board = [args.cc, "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
             str(ESP32 / "main" / "bridge.c"), str(ESP32 / "test" / "bridge_host.c"),
             "-o", str(shim)]
    print("$ %s" % " ".join(board), flush=True)
    build = subprocess.run(board, cwd=PORT)
    if build.returncode:
        return build.returncode

    # The shim's beacons go to broadcast and, with --loopback, to 127.0.0.1:
    # the game learns its host from the beacon's source.
    run = subprocess.Popen([str(shim), "--kind", "2", "--loopback"], cwd=PORT)
    try:
        time.sleep(0.3) # its socket, bound before the game starts looking
        game_run = subprocess.run([str(binary)], cwd=PORT, timeout=120)
        return game_run.returncode
    finally:
        run.terminate()
        try:
            run.wait(timeout=2)
        except subprocess.TimeoutExpired:
            run.kill()


if __name__ == "__main__":
    sys.exit(main())
