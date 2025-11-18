import socket

PORT = 8080
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", PORT))
s.listen(1)
print(f"Mock server listening on port {PORT}...")
conn, addr = s.accept()
print(f"Connected by {addr}")
while True:
    data = conn.recv(1024)
    print("Received:", data.decode())
