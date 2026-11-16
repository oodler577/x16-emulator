#!/usr/bin/env python3
"""Native modem protocol contracts; fixtures never use public Internet services."""
import ctypes, http.server, os, socket, ssl, subprocess, sys, tempfile
import threading, time, unittest
from pathlib import Path
from test_serialcard import Card, Fixture
import paramiko

LIBRARY = sys.argv.pop(1)

class Server:
    def __init__(self, handle, udp=False):
        self.sock=socket.socket(type=socket.SOCK_DGRAM if udp else socket.SOCK_STREAM)
        self.sock.bind(('127.0.0.1',0)); self.port=self.sock.getsockname()[1]
        self.sock.settimeout(5)
        if not udp: self.sock.listen()
        self.error=None
        def run():
            try:
                if udp: handle(self.sock)
                else:
                    conn,_=self.sock.accept()
                    with conn: conn.settimeout(5); handle(conn)
            except Exception as exc: self.error=exc
        self.thread=threading.Thread(target=run,daemon=True); self.thread.start()
    def __enter__(self): return self
    def __exit__(self,*args):
        self.sock.close(); self.thread.join(6)
        if not args[0]:
            if self.thread.is_alive(): raise AssertionError('server did not finish')
            if self.error: raise self.error

def recv_exact(conn,n):
    data=b''
    while len(data)<n:
        part=conn.recv(n-len(data))
        if not part: raise AssertionError(f'early EOF at {len(data)}/{n}')
        data+=part
    return data

class Protocols(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory()
        cls.root=Path(cls.tmp.name)
        cls.cert=cls.root/'ca.pem'; cls.key=cls.root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
            '-keyout',str(cls.key),'-out',str(cls.cert),'-subj','/CN=localhost',
            '-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],check=True,capture_output=True)
        os.environ['X16_SERIAL_CA_FILE']=str(cls.cert)
        cls.tls=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.tls.load_cert_chain(cls.cert,cls.key)
    @classmethod
    def tearDownClass(cls):
        os.environ.pop('X16_SERIAL_CA_FILE',None); os.environ.pop('X16_SERIAL_KNOWN_HOSTS',None)
        cls.tmp.cleanup()
    def setUp(self):
        self.c=Card(LIBRARY); self.assertTrue(self.c.enable()); self.c.prepare()
    def tearDown(self): self.c.close()
    def until(self,ending,timeout=5):
        result=bytearray(); deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            self.c.step()
            while self.c.read(5)&1: result.append(self.c.read(0))
            if result.endswith(ending): return bytes(result)
            time.sleep(.001)
        self.fail(f'expected suffix {ending!r}, got {result[-300:]!r}')
    def dial(self,port,modifier='',host='127.0.0.1'):
        self.c.send(f'ATQ1D{modifier}"{host}:{port}"\r'.encode())
        self.c.wait(lambda:self.c.read(6)&128)
    def escape(self):
        self.c.step(8_100_000); self.c.send(b'+++'); self.c.step(8_100_000)
    def test_wifi_mock_and_status(self):
        self.assertIn(b'MOCK',self.c.command(b'ATW'))
        result=self.c.command(b'ATW"MY TEST,never-stored-password"')
        self.assertIn(b'MY TEST',result); self.assertNotIn(b'never-stored-password',result)
        self.assertIn(b'MOCK Wi-Fi SSID: MY TEST',self.c.command(b'ATI3'))
        self.c.send(b'AT+CONFIG\r'); self.assertIn(b'MOCK',self.until(b'Choice: '))
        self.c.send(b'2\r'); self.until(b'Mock SSID label: ')
        self.c.send(b'NEW LABEL\r'); self.until(b'Choice: ')
        self.c.send(b'Q\r'); self.until(b'OK\r')
        self.assertIn(b'NEW LABEL',self.c.command(b'ATI3'))
        self.assertIn(b'DISCONNECTED',self.c.command(b'ATC'))
    def test_escape_resume_and_raw_pluses(self):
        with Fixture() as f:
            self.dial(f.port)
            payload=b'raw+++data\x00\xff'
            self.c.send(payload); self.assertEqual(self.c.receive(len(payload)),payload)
            self.escape()
            self.c.send(b'ATQ0C\r'); self.assertIn(b'CONNECTED 1',self.until(b'OK\r'))
            self.c.send(b'ATO\r'); self.until(b'CONNECT\r')
            self.c.send(b'resumed'); self.assertEqual(self.c.receive(7),b'resumed')
            self.c.write(4,0x26)
    def test_escape_partial_sequence_is_not_lost(self):
        with Fixture() as f:
            self.dial(f.port); self.c.step(8_100_000); self.c.send(b'++')
            self.c.step(8_100_000); self.assertEqual(self.c.receive(2),b'++')
            self.c.write(4,0x26)
    def test_telnet_negotiation_fragmentation_and_ff(self):
        expected=bytes([255,253,0,255,253,1,255,251,24,255,254,99])
        def serve(conn):
            for b in bytes([255,251,0,255,251,1,255,253,24,255,251,99]): conn.sendall(bytes([b]))
            self.assertEqual(recv_exact(conn,len(expected)),expected)
            conn.sendall(bytes([255,250,24,1,255,240]))
            self.assertEqual(recv_exact(conn,11),b'\xff\xfa\x18\x00VT100\xff\xf0')
            conn.sendall(b'READY\xff\xff!')
            self.assertEqual(recv_exact(conn,4),b'A\xff\xffB')
        with Server(serve) as server:
            self.dial(server.port,'T'); self.assertEqual(self.c.receive(7),b'READY\xff!')
            self.c.send(b'A\xffB'); self.c.wait(lambda:not self.c.read(6)&128)
    def test_tls_binary_echo(self):
        payload=bytes(range(256))*4
        def serve(conn):
            with self.tls.wrap_socket(conn,server_side=True) as secure:
                data=recv_exact(secure,len(payload)); self.assertEqual(data,payload); secure.sendall(data)
        with Server(serve) as server:
            self.dial(server.port,'S'); self.c.send(payload)
            self.assertEqual(self.c.receive(len(payload)),payload)
            self.c.wait(lambda:not self.c.read(6)&128)
    def test_tls_rejects_untrusted_server(self):
        def serve(conn):
            try:
                with self.tls.wrap_socket(conn,server_side=True): pass
            # Rejected handshakes can close with a Winsock abort/reset instead
            # of a TLS alert. Guest-side failure and carrier are checked below.
            except (ssl.SSLError, ConnectionAbortedError, ConnectionResetError): pass
        with Server(serve) as server:
            os.environ['X16_SERIAL_CA_FILE']=str(self.root/'missing-ca.pem')
            try:
                self.c.send(f'ATDS"127.0.0.1:{server.port}"\r'.encode())
                self.assertIn(b'NO ANSWER',self.until(b'NO ANSWER\r'))
                self.assertFalse(self.c.read(6)&128)
            finally: os.environ['X16_SERIAL_CA_FILE']=str(self.cert)
    def download(self,payload,mode=0,tls=False):
        self.c.command(f'ATS45={mode}'.encode())
        def handle(conn):
            if tls: conn=self.tls.wrap_socket(conn,server_side=True)
            with conn:
                request=b''
                while not request.endswith(b'\r\n\r\n'): request+=conn.recv(1)
                self.assertIn(b'GET /file.bin ',request)
                conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: '+str(len(payload)).encode()+b'\r\nConnection: close\r\n\r\n'+payload)
        with Server(handle) as server:
            scheme='https' if tls else 'http'
            self.c.send(f'AT&G"{scheme}://127.0.0.1:{server.port}/file.bin"\r'.encode())
            return self.until(b'\rOK\r')
    def test_http_download_checksum_and_exact_binary(self):
        payload=bytes(range(256))*3
        result=self.download(payload)
        self.assertEqual(result,f'[ 0 {len(payload)} {sum(payload)%256} ]\r'.encode()+payload+b'\rOK\r')
    def test_https_download_without_checksum(self):
        payload=b'\x00\xffHTTPS\r\n'
        self.assertEqual(self.download(payload,3,True),b'[ 0 9 ]\r'+payload+b'\rOK\r')
    def test_download_decimal_and_hex(self):
        self.assertEqual(self.download(b'\x00\xff',2),b'[\r0\r2\r255\r]\r0\r255\r\rOK\r')
        self.assertEqual(self.download(b'\x00\xff',1),b'[ 00 02 FF ]\r00FF\r\rOK\r')
    def test_download_empty_and_truncation(self):
        self.assertEqual(self.download(b''),b'[ 0 0 0 ]\r\rOK\r')
        def serve(conn):
            conn.recv(4096); conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort')
        with Server(serve) as server:
            self.c.send(f'AT&G"127.0.0.1:{server.port}/x"\r'.encode())
            self.assertEqual(self.until(b'ERROR\r'),b'\rERROR\r')
    def test_udp_preserves_datagram_boundaries_and_empty_packets(self):
        packets=[b'',b'\x00\xffdata',bytes(range(256))*8]
        def serve(sock):
            for expected in packets:
                data,peer=sock.recvfrom(8192); self.assertEqual(data,expected); sock.sendto(data,peer)
        with Server(serve,udp=True) as server:
            self.dial(server.port,'U')
            for payload in packets:
                frame=len(payload).to_bytes(2,'big')+payload
                self.c.send(frame); self.assertEqual(self.c.receive(len(frame)),frame)
            self.c.write(4,0x26)
    def test_listener_manual_and_autoanswer(self):
        for auto in (0,1):
            self.c.command(f'ATS0={auto}S41=1'.encode())
            with socket.socket() as probe:
                probe.bind(('127.0.0.1',0)); port=probe.getsockname()[1]
            self.assertIn(b'OK',self.c.command(f'ATA{port}'.encode()))
            peer=socket.socket(); peer.settimeout(2)
            deadline=time.monotonic()+3
            while True:
                try: peer.connect(('127.0.0.1',port)); break
                except ConnectionRefusedError:
                    if time.monotonic()>deadline: raise
                    time.sleep(.01)
            with peer:
                self.until(b'CONNECT\r' if auto else b'RING\r')
                if not auto:
                    self.assertFalse(self.c.read(6)&128)
                    self.c.send(b'ATA\r'); self.until(b'CONNECT\r')
                peer.sendall(b'IN'); self.assertEqual(self.c.receive(2),b'IN')
                self.c.send(b'OUT'); self.assertEqual(recv_exact(peer,3),b'OUT')
                self.c.write(4,0x26)
            self.c.command(b'ATN0'); self.c.write(4,0x27)
    def test_second_uart_tcp_bridge(self):
        self.c.lib.netcard_attach_serial.argtypes=[ctypes.c_char_p]
        self.c.lib.netcard_attach_serial.restype=ctypes.c_bool
        with Fixture() as f:
            self.assertTrue(self.c.lib.netcard_attach_serial(f'127.0.0.1:{f.port}'.encode()))
            self.c.init(1); self.c.wait(lambda:self.c.read(14)&128)
            payload=b'SERIAL TWO\x00\xff'
            for byte in payload:
                self.c.wait(lambda:self.c.read(13)&32); self.c.write(8,byte); self.c.step()
                self.c.wait(lambda:self.c.read(13)&1); self.assertEqual(self.c.read(8),byte)
    def test_listener_port_conflict_is_an_error(self):
        with socket.socket() as busy:
            busy.bind(('127.0.0.1',0)); busy.listen()
            self.assertIn(b'ERROR',self.c.command(f'ATA{busy.getsockname()[1]}'.encode()))
    def test_udp_oversize_frame_drops_carrier(self):
        with socket.socket(type=socket.SOCK_DGRAM) as peer:
            peer.bind(('127.0.0.1',0))
            self.dial(peer.getsockname()[1],'U')
            self.c.send(b'\xff\xff')
            self.c.wait(lambda:not self.c.read(6)&128)
    def test_download_cancel_and_reset(self):
        def serve(conn):
            conn.recv(4096)
            conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\nstart')
            try:
                while conn.recv(1): pass
            except ConnectionError: pass
        with Server(serve) as server:
            self.c.send(f'AT&G"127.0.0.1:{server.port}/slow"\r'.encode())
            # Give the host worker an opportunity to issue the HTTP request.
            self.c.wait(lambda:server.thread.is_alive())
            time.sleep(.05)
            self.c.send(b'\x03'); self.assertIn(b'OK',self.until(b'OK\r'))
        self.assertEqual(self.c.command(b'AT'),b'\rOK\r')
    def test_native_serial_pty(self):
        if os.name=='nt':
            # The native Windows build tests invalid-port failure; real COM
            # hardware is not attached to GitHub's Windows runner.
            self.c.lib.netcard_attach_device.argtypes=[ctypes.c_char_p]
            self.c.lib.netcard_attach_device.restype=ctypes.c_bool
            self.assertTrue(self.c.lib.netcard_attach_device(b'COM99999'))
            self.c.init(1)
            for _ in range(20): self.c.step(); time.sleep(.001)
            self.assertEqual(self.c.read(14)&0xf0,0)
            return
        import pty
        master,slave=pty.openpty()
        try:
            self.c.lib.netcard_attach_device.argtypes=[ctypes.c_char_p]
            self.c.lib.netcard_attach_device.restype=ctypes.c_bool
            self.assertTrue(self.c.lib.netcard_attach_device(os.ttyname(slave).encode()))
            self.c.init(1); self.c.wait(lambda:self.c.read(14)&128)
            os.set_blocking(master,False)
            os.write(master,b'\x00\xff')
            for byte in (0,255):
                self.c.wait(lambda:self.c.read(13)&1); self.assertEqual(self.c.read(8),byte)
            self.c.write(8,65)
            received=bytearray()
            def done():
                try: received.extend(os.read(master,1024))
                except BlockingIOError: pass
                return received==b'A'
            self.c.wait(done)
        finally:
            self.c.close(); os.close(master); os.close(slave)
    def test_ssh_rejects_changed_host_key_before_password(self):
        actual=paramiko.RSAKey.generate(2048)
        wrong=paramiko.RSAKey.generate(2048)
        passwords=[]
        class Login(paramiko.ServerInterface):
            def check_auth_password(self,u,p): passwords.append((u,p)); return paramiko.AUTH_FAILED
        def serve(conn):
            with paramiko.Transport(conn) as t:
                t.add_server_key(actual)
                try:
                    t.start_server(server=Login())
                    t.accept(3)
                except (EOFError,ConnectionError,paramiko.SSHException): pass
        with Server(serve) as server:
            known=self.root/'wrong_known_hosts'
            known.write_text(f'[127.0.0.1]:{server.port} {wrong.get_name()} {wrong.get_base64()}\n')
            os.environ['X16_SERIAL_KNOWN_HOSTS']=str(known)
            self.c.send(f'ATDS"guest:do-not-transmit@127.0.0.1:{server.port}"\r'.encode())
            self.assertIn(b'NO ANSWER',self.until(b'NO ANSWER\r'))
        self.assertEqual(passwords,[])
    def test_ssh_verified_host_shell(self):
        key=paramiko.RSAKey.generate(2048)
        class Login(paramiko.ServerInterface):
            def check_auth_password(self,u,p):
                return paramiko.AUTH_SUCCESSFUL if (u,p)==('guest','test-only') else paramiko.AUTH_FAILED
            def get_allowed_auths(self,u): return 'password'
            def check_channel_request(self,k,i): return paramiko.OPEN_SUCCEEDED if k=='session' else 1
            def check_channel_pty_request(self,*a): return True
            def check_channel_shell_request(self,*a): return True
        def serve(conn):
            with paramiko.Transport(conn) as t:
                t.add_server_key(key); t.start_server(server=Login())
                ch=t.accept(5); self.assertIsNotNone(ch)
                with ch:
                    ch.settimeout(5)
                    self.assertEqual(recv_exact(ch,4),b'PING'); ch.sendall(b'PONG')
                    # Keep the channel open until guest hangup, preserving the final data.
                    ch.recv(1)
        with Server(serve) as server:
            known=self.root/'known_hosts'
            known.write_text(f'[127.0.0.1]:{server.port} {key.get_name()} {key.get_base64()}\n')
            os.environ['X16_SERIAL_KNOWN_HOSTS']=str(known)
            self.dial(server.port,'S',host='guest:test-only@127.0.0.1')
            self.c.send(b'PING'); self.assertEqual(self.c.receive(4),b'PONG'); self.c.write(4,0x26)

if __name__=='__main__':
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(Protocols)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
