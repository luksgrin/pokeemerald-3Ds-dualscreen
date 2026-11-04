"""Host test of the bridge leg of local wireless play: builds
src/3ds_local.c with LOCAL_LINK_UDP against the libctru doubles in
tests/local_host, runs it against tools/frlg_bridge/bridge.py on loopback,
and expects the join, the adapter's PEER_* exchange and a burst of messages
to come back clean. The on-device wireless needs none of this; it checks
the leg toward the Switch (docs/frlg-switch-compatibility).
"""
import argparse
import subprocess
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent  # 3ds_port
REPO = PORT.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--keep-going", action="store_true",
                        help="run the test even if the compile printed warnings")
    args = parser.parse_args()

    binary = PORT / "build" / "local_udp_test"
    binary.parent.mkdir(parents=True, exist_ok=True)
    cc = [args.cc, "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
          "-DLOCAL_LINK_UDP", "-Iinclude", "-Itests/local_host",
          "src/3ds_local.c", "tests/local_udp_test.c", "-pthread",
          "-o", str(binary)]
    print("$ %s" % " ".join(cc), flush=True)
    build = subprocess.run(cc, cwd=PORT)
    if build.returncode:
        return build.returncode

    sys.path.insert(0, str(REPO / "tools" / "frlg_bridge"))
    import bridge as bridge_mod

    bridge = bridge_mod.Bridge(kind=2, loopback=True, verbose=True)
    thread = threading.Thread(target=bridge.run, daemon=True)
    thread.start()

    try:
        run = subprocess.run([str(binary)], cwd=PORT, timeout=120)
        return run.returncode
    finally:
        bridge.stop.set()
        if bridge.sock is not None:
            bridge.sock.close()
        thread.join(timeout=2)


if __name__ == "__main__":
    sys.exit(main())
