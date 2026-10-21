# Diagnostic: ask for Jose-Test's records with a one-shot (legacy unicast) mDNS query
# and with a QU query, and print who answers.
import socket, struct, sys, time

def q(name, qtype, qu=False):
    hdr = struct.pack(">HHHHHH", 0x1234, 0, 1, 0, 0, 0)
    body = b"".join(bytes([len(p)]) + p.encode() for p in name.split(".") if p) + b"\0"
    return hdr + body + struct.pack(">HH", qtype, 0x8001 if qu else 1)

ip = sys.argv[1]
for label, dest, qu in [("legacy->mcast", ("224.0.0.251", 5353), False),
                        ("legacy->unicast", (ip, 5353), False),
                        ("legacy->loopback", ("127.0.0.1", 5353), False)]:
    for name, t in [("Jose-Test._netaudio-arc._udp.local", 33), ("Jose-Test.local", 1)]:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(ip))
        s.settimeout(2)
        s.sendto(q(name, t), dest)
        got = []
        try:
            while True:
                d, a = s.recvfrom(9000)
                an = struct.unpack(">H", d[6:8])[0]
                got.append(f"{a} answers={an} len={len(d)}")
        except socket.timeout:
            pass
        print(label, name, "->", got or "NO REPLY")
