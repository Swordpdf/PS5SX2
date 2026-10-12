# Send Elevate.elf to the console ELF loader (port 9021). The payload stays running.
import socket
import sys
from pathlib import Path

PORT = 9021
ELF = Path(__file__).resolve().parent / "Elevate.elf"


def main():
	if len(sys.argv) < 2:
		raise SystemExit("kullanim: py -3 send.py <konsolun adresi>")
	host = sys.argv[1]
	data = ELF.read_bytes()
	if data[:4] != b"\x7fELF":
		raise SystemExit(f"ELF degil: {ELF}")
	print(f"Gonderiliyor: {ELF.name} ({len(data)} bayt) -> {host}:{PORT}")
	s = socket.create_connection((host, PORT), timeout=8)
	try:
		s.sendall(data)
		s.shutdown(socket.SHUT_WR)
		s.settimeout(3)
		try:
			reply = s.recv(400)
		except socket.timeout:
			reply = b""
	finally:
		s.close()
	if reply:
		print(reply.decode("utf-8", "replace").strip())
	print("Yuklendi. Payload acik kaliyor; PS5SX2'yi yeniden ac.")


if __name__ == "__main__":
	try:
		main()
	except OSError as exc:
		raise SystemExit(f"ELF loader'a baglanilamadi ({PORT}): {exc}")
