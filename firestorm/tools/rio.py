#!/usr/bin/env python3
"""Talk to the shim's virtual RIO board over its control port.

The shim's virtual Remote I/O board, inside a pod's MW4.exe, listens on
udp://127.0.0.1:9100 ([rio] control_port in shim.ini). This sends it one
command and prints what comes back, or listens for what the game tells the
board - the lamps among it.

usage:
  rio.py tap 24                 press and release button 0x24
  rio.py press 40 / release 40  hold and let go of the trigger
  rio.py axes T LP RP SY SX     hold the five analog values; "axes off" to stop
  rio.py snap                   snapshots of every display
  rio.py listen [seconds]       print what the game sends the board
  rio.py sweep FIRST LAST       tap each button in turn, a snapshot after each

  --port N                      another control port
"""
import socket
import sys
import time


def main(argv):
    port = 9100
    if "--port" in argv:
        i = argv.index("--port")
        port = int(argv[i + 1])
        del argv[i:i + 2]
    if not argv:
        print(__doc__)
        return 1
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    dest = ("127.0.0.1", port)

    def send(text):
        s.sendto(text.encode(), dest)

    if argv[0] == "listen":
        until = time.time() + (float(argv[1]) if len(argv) > 1 else 30)
        send("hello")
        while time.time() < until:
            try:
                data, _ = s.recvfrom(512)
                print(time.strftime("%H:%M:%S"), data.decode(errors="replace"))
            except socket.timeout:
                pass
        return 0
    if argv[0] == "sweep":
        first, last = int(argv[1], 16), int(argv[2], 16)
        for b in range(first, last + 1):
            send("tap %02X" % b)
            time.sleep(0.6)
            send("snap")
            time.sleep(0.6)
            print("tapped %02X" % b)
        return 0
    send(" ".join(argv))
    try:
        data, _ = s.recvfrom(512)
        print(data.decode(errors="replace"))
    except socket.timeout:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
