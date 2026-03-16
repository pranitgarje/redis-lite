import socket
import struct

class ApheloError(Exception):
    pass

class ApheloClient:
    def __init__(self, host='127.0.0.1', port=1234):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))

    def _recvall(self, n):
        """Helper to ensure we receive exactly n bytes."""
        data = bytearray()
        while len(data) < n:
            packet = self.sock.recv(n - len(data))
            if not packet:
                raise ConnectionError("Connection closed by server")
            data.extend(packet)
        return bytes(data)

    def _execute(self, *args):
        """Packs a command, sends it, and unpacks the response."""
        # --- 1. PACK THE COMMAND ---
        # Calculate sizes: 4 bytes for nstr, plus (4 + string_length) for each arg
        args_bytes = [str(arg).encode('utf-8') for arg in args]
        body_len = 4 + sum(4 + len(b) for b in args_bytes)
        
        # Pack Total Length and Number of Strings (Little-Endian 32-bit Unsigned Ints)
        msg = struct.pack('<II', body_len, len(args_bytes))
        
        # Pack each string length and the string itself
        for b in args_bytes:
            msg += struct.pack('<I', len(b)) + b
            
        self.sock.sendall(msg)

        # --- 2. UNPACK THE RESPONSE ---
        # Read the 4-byte header to get the payload length
        header = self._recvall(4)
        payload_len = struct.unpack('<I', header)[0]
        
        # Read the exact payload
        payload = self._recvall(payload_len)
        tag = payload[0]
        
        # TAG_NIL = 0
        if tag == 0:
            return None
            
        # TAG_ERR = 1
        elif tag == 1:
            code = struct.unpack('<I', payload[1:5])[0]
            msg_len = struct.unpack('<I', payload[5:9])[0]
            err_msg = payload[9:9+msg_len].decode('utf-8')
            raise ApheloError(f"[{code}] {err_msg}")
            
        # TAG_STR = 2
        elif tag == 2:
            str_len = struct.unpack('<I', payload[1:5])[0]
            return payload[5:5+str_len].decode('utf-8')
            
        # TAG_INT = 3
        elif tag == 3:
            return struct.unpack('<q', payload[1:9])[0] # 'q' is 64-bit signed int
            
        # TAG_DBL = 4
        elif tag == 4:
            return struct.unpack('<d', payload[1:9])[0] # 'd' is 64-bit float
            
        else:
            raise ApheloError(f"Unknown response tag: {tag}")

    # --- 3. CONVENIENCE METHODS ---
    def get(self, key):
        return self._execute("get", key)

    def set(self, key, value):
        return self._execute("set", key, value)

    def delete(self, key):
        return self._execute("del", key)
        
    def pexpire(self, key, ttl_ms):
        return self._execute("pexpire", key, ttl_ms)
        
    def zadd(self, zset_name, score, value):
        return self._execute("zadd", zset_name, score, value)