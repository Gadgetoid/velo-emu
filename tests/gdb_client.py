import socket
import sys
import time

GENERAL_VECTOR = 0x80000080
PC_REGISTER = 0x25
PACKET_SIZE = 0x4000


class Client:
    def __init__(self, port):
        for attempt in range(50):
            try:
                self.connection = socket.create_connection(("127.0.0.1", port), timeout=30)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise SystemExit("no GDB stub on port %d" % port)
        self.buffer = b""

    def send(self, payload):
        data = payload.encode("latin-1")
        self.connection.sendall(b"$%s#%02x" % (data, sum(data) & 0xFF))

    def receive(self):
        while True:
            start = self.buffer.find(b"$")
            end = self.buffer.find(b"#", start)
            if start >= 0 and end >= 0 and len(self.buffer) >= end + 3:
                payload = self.buffer[start + 1:end].decode("latin-1")
                self.buffer = self.buffer[end + 3:]
                return payload
            chunk = self.connection.recv(65536)
            if not chunk:
                raise SystemExit("stub closed the connection")
            self.buffer += chunk

    def request(self, payload):
        self.send(payload)
        while True:
            reply = self.receive()
            if not reply.startswith("O") or reply == "OK":
                return reply

    def monitor(self, command):
        self.send("qRcmd," + command.encode().hex())
        output = ""
        while True:
            reply = self.receive()
            if reply.startswith("O") and reply != "OK":
                output += bytes.fromhex(reply[1:]).decode("latin-1")
            else:
                return output

    def register(self, number):
        return int.from_bytes(bytes.fromhex(self.request("p%x" % number)), "little")


def check(name, condition, detail=""):
    if not condition:
        raise SystemExit("%s failed %s" % (name, detail))


def main():
    client = Client(int(sys.argv[1]))
    client.request("QStartNoAckMode")
    supported = client.request("qSupported:xmlRegisters=mips")
    check("qSupported", "qXfer:features:read+" in supported and "qXfer:libraries:read+" in supported, supported)
    check("stop reason", client.request("?").startswith("T05"))
    target = client.request("qXfer:features:read:target.xml:0,fff")
    check("target.xml", target.startswith("l") and "org.gnu.gdb.mips.cpu" in target and 'bitsize="32"' in target)
    registers = client.request("g")
    check("registers", len(registers) == 72 * 8, str(len(registers)))
    processes = client.monitor("processes")
    check("processes", "NK.EXE" in processes and "filesys.exe" in processes, processes)
    check("memory", len(client.request("m80000080,8")) == 16)
    check("breakpoint insert", client.request("Z0,%x,4" % GENERAL_VECTOR) == "OK")
    stop = client.request("c")
    check("breakpoint stop", stop.startswith("T05"), stop)
    check("breakpoint pc", client.register(PC_REGISTER) == GENERAL_VECTOR, hex(client.register(PC_REGISTER)))
    check("breakpoint remove", client.request("z0,%x,4" % GENERAL_VECTOR) == "OK")
    stop = client.request("s")
    check("step", stop.startswith("T05") and client.register(PC_REGISTER) == GENERAL_VECTOR + 4, hex(client.register(PC_REGISTER)))
    libraries = client.request("qXfer:libraries:read::0,fff")
    check("libraries", "coredll.elf" in libraries.lower(), libraries[:200])
    data = bytes(range(256)) * 32
    full = "M080380000,%x:%s" % (len(data) - 8, data[:-8].hex())
    check("full packet size", len(full) == PACKET_SIZE, str(len(full)))
    check("full packet write", client.request(full) == "OK")
    check("full packet read", client.request("m80380000,%x" % (len(data) - 8)) == data[:-8].hex())
    client.send("k")


main()
