#!/usr/bin/env python3
"""Run real C UART and TCP-bridge behavior tests using loopback servers only."""
import ctypes, socket, threading, time, unittest, sys, traceback
from pathlib import Path

class Card:
    def __init__(self,path):
        self.lib=ctypes.CDLL(str(Path(path).resolve()))
        for name in ("netcard_enable","netcard_matches"):
            fn=getattr(self.lib,name); fn.argtypes=[ctypes.c_uint16]; fn.restype=ctypes.c_bool
        self.lib.netcard_read.argtypes=[ctypes.c_uint8,ctypes.c_bool]
        self.lib.netcard_read.restype=ctypes.c_uint8
        self.lib.netcard_write.argtypes=[ctypes.c_uint8,ctypes.c_uint8]
        self.lib.netcard_step.argtypes=[ctypes.c_uint,ctypes.c_uint32]
        self.lib.netcard_irq.restype=ctypes.c_bool
    def enable(self,base=0x9fe0):
        self.eol=1
        return self.lib.netcard_enable(base)
    def reset(self):
        self.eol=1
        self.lib.netcard_reset()
    def close(self): self.lib.netcard_close()
    def read(self,reg,debug=False): return self.lib.netcard_read(reg,debug)
    def write(self,reg,value): self.lib.netcard_write(reg,value)
    def step(self,clocks=8000): self.lib.netcard_step(8,clocks)
    def init(self,port=0):
        b=port*8
        self.write(b+3,0x80); self.write(b,8); self.write(b+1,0)
        self.write(b+3,3); self.write(b+2,0x87); self.write(b+4,0x27); self.write(b+1,0)
    def wait(self,predicate,timeout=3):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            self.step()
            if predicate(): return
            time.sleep(.001)
        raise AssertionError("device wait exceeded deadline")
    def send(self,data):
        for value in data:
            self.wait(lambda: self.read(5)&0x20)
            self.write(0,value); self.step()
    def receive(self,length):
        result=bytearray()
        for _ in range(length):
            try:
                self.wait(lambda: self.read(5)&1)
            except AssertionError as exc:
                raise AssertionError(f"RX stalled after {len(result)}/{length} bytes; LSR={self.read(5):02x}, MSR={self.read(6):02x}") from exc
            result.append(self.read(0))
        return bytes(result)
    def command(self,data):
        if b"R0" in data.upper(): self.eol=0
        if b"R1" in data.upper(): self.eol=1
        self.send(data+b"\r"); result=bytearray()
        deadline=time.monotonic()+3
        while time.monotonic()<deadline:
            self.step()
            while self.read(5)&1: result.append(self.read(0))
            suffix=b"\r\n" if self.eol else b"\r"
            if any(result.endswith(s+suffix) for s in (b"OK",b"ERROR")):
                return bytes(result)
            time.sleep(.001)
        raise AssertionError(f"no command result: {result!r}")
    def prepare(self):
        self.init()
        answer=self.command(b"ATE0V1Q0X0F0R0&P0S62=0S63=2S46=0S48=0S50=0S54=0N0H")
        assert answer.endswith(b"OK\r"),answer
    def dial(self,port,quiet=True):
        self.send(f'ATQ{int(quiet)}D"127.0.0.1:{port}"\r'.encode())
        self.wait(lambda: self.read(6)&0x80)

class Fixture:
    def __init__(self,payload=b"",echo=True,close=False):
        self.listener=socket.socket()
        self.listener.bind(("127.0.0.1",0)); self.listener.listen(); self.listener.settimeout(.1)
        self.port=self.listener.getsockname()[1]
        self.payload,self.echo,self.close_after=payload,echo,close
        self.received=bytearray(); self.stop=False; self.failure=None
        self.thread=threading.Thread(target=self.run,daemon=True); self.thread.start()
    def run(self):
        try:
            while not self.stop:
                try: peer,_=self.listener.accept()
                except socket.timeout: continue
                with peer:
                    peer.settimeout(.5)
                    if self.payload: peer.sendall(self.payload)
                    if self.close_after: return
                    while not self.stop:
                        try: data=peer.recv(4096)
                        except socket.timeout: continue
                        if not data: break
                        self.received.extend(data)
                        if self.echo: peer.sendall(data)
                return
        except OSError as exc:
            if not self.stop: self.failure=exc
    def __enter__(self): return self
    def __exit__(self,*args):
        self.stop=True; self.thread.join(2); self.listener.close()
        if self.failure: raise self.failure

class SerialCardTests(unittest.TestCase):
    def setUp(self):
        self.c=CARD; self.c.close(); self.assertTrue(self.c.enable())
    def tearDown(self): self.c.close()
    def test_disabled_and_address_validation(self):
        self.c.close(); self.assertFalse(self.c.lib.netcard_matches(0x9fe0))
        for base in (0,0x9f20,0x9fe8,0x9fb0,0x9ff8): self.assertFalse(self.c.enable(base))
        self.assertTrue(self.c.enable(0x9fc0))
        self.assertTrue(self.c.lib.netcard_matches(0x9fcf))
        self.assertFalse(self.c.lib.netcard_matches(0x9fe0))
    def test_independent_scratch_registers_and_reset(self):
        for i in range(256):
            self.c.write(7,i); self.c.write(15,255-i)
            self.assertEqual(self.c.read(7),i); self.assertEqual(self.c.read(15),255-i)
        self.c.reset(); self.assertEqual(self.c.read(7),0); self.assertEqual(self.c.read(15),0)
        self.assertEqual(self.c.read(5),0x60)
    def test_dlab_and_register_masks(self):
        for b in (0,8):
            self.c.write(b+3,0x80); self.c.write(b,96); self.c.write(b+1,0x12)
            self.assertEqual(self.c.read(b),96); self.assertEqual(self.c.read(b+1),0x12)
            self.c.write(b+3,3); self.c.write(b+1,255); self.c.write(b+4,255)
            self.assertEqual(self.c.read(b+1),15); self.assertEqual(self.c.read(b+4),0x3f)
            self.assertFalse(self.c.read(b+5)&1)
    def test_tx_shift_register_thre_and_temt(self):
        self.c.init(1); self.c.write(12,0x10); self.c.write(8,42)
        self.assertFalse(self.c.read(13)&0x20)
        self.c.step(1)
        self.assertTrue(self.c.read(13)&0x20); self.assertFalse(self.c.read(13)&0x40)
        self.c.step(1000)
        self.assertEqual(self.c.read(8),42); self.assertTrue(self.c.read(13)&0x40)
    def test_both_loopbacks_all_byte_values(self):
        for port in (0,1):
            self.c.init(port); b=port*8; self.c.write(b+4,0x10)
            for value in range(256):
                self.c.write(b,value); self.c.step(1000)
                self.assertTrue(self.c.read(b+5)&1); self.assertEqual(self.c.read(b),value)
    def test_fifo_capacity_overrun_and_clear(self):
        self.c.init(1); self.c.write(12,0x10)
        for v in range(17): self.c.write(8,v); self.c.step(1000)
        self.assertTrue(self.c.read(13,True)&2); self.assertTrue(self.c.read(13)&2)
        self.assertFalse(self.c.read(13)&2)
        self.assertEqual(bytes(self.c.read(8) for _ in range(16)),bytes(range(16)))
        self.c.write(8,42); self.c.step(1000); self.c.write(10,0x83)
        self.assertFalse(self.c.read(13)&1); self.assertEqual(self.c.read(10)&0xc0,0xc0)
    def test_nonfifo_holds_one_byte(self):
        self.c.write(11,3); self.c.write(12,0x10)
        for v in (1,2): self.c.write(8,v); self.c.step(1000)
        self.assertTrue(self.c.read(13)&2); self.assertEqual(self.c.read(8),1)
        self.assertFalse(self.c.read(13)&1)
    def test_debug_reads_do_not_pop_receive_fifo(self):
        self.c.init(1); self.c.write(12,0x10); self.c.write(8,42); self.c.step(1000)
        for _ in range(8): self.assertEqual(self.c.read(8,True),42)
        self.assertEqual(self.c.read(8),42); self.assertFalse(self.c.read(13)&1)
    def test_thre_interrupt_ack_and_mask(self):
        self.c.write(9,2); self.assertTrue(self.c.lib.netcard_irq())
        self.assertEqual(self.c.read(10,True)&15,2); self.assertTrue(self.c.lib.netcard_irq())
        self.assertEqual(self.c.read(10)&15,2); self.assertFalse(self.c.lib.netcard_irq())
        self.c.write(9,0); self.assertFalse(self.c.lib.netcard_irq())
    def test_receive_trigger_and_timeout_interrupt(self):
        self.c.init(1); self.c.write(12,0x10); self.c.write(9,1)
        self.c.write(8,42); self.c.step(1000); self.assertFalse(self.c.lib.netcard_irq())
        self.c.step(4000); self.assertTrue(self.c.lib.netcard_irq()); self.assertEqual(self.c.read(10)&15,12)
        self.c.read(8); self.assertFalse(self.c.lib.netcard_irq())
        for v in range(8): self.c.write(8,v); self.c.step(1000)
        self.assertEqual(self.c.read(10)&15,4)
    def test_line_status_interrupt_priority(self):
        self.c.init(1); self.c.write(12,0x10); self.c.write(9,7); self.c.write(11,0x43)
        self.c.write(8,42); self.c.step(1000)
        self.assertEqual(self.c.read(10)&15,6); self.assertTrue(self.c.read(13)&0x10)
        self.assertEqual(self.c.read(8),0)
    def test_modem_loopback_delta_bits(self):
        self.c.write(12,0x10); self.c.read(14); self.c.write(12,0x1f)
        self.assertEqual(self.c.read(14,True)&0xf0,0xf0)
        self.assertEqual(self.c.read(14)&0x0b,0x0b); self.assertEqual(self.c.read(14)&15,0)
        self.c.write(12,0x10); self.assertTrue(self.c.read(14)&4)
    def test_profile_echo_ascii_and_unsupported_commands(self):
        self.c.init(); self.assertIn(b"X16 host TCP modem",self.c.command(b"ATI"))
        self.c.prepare(); self.assertEqual(self.c.command(b"AT"),b"\rOK\r")
        for cmd in (b"AT&G",b"ATS62=1",b"AT+CONFIG",b"AT&",b"ATS",b'ATD"host:0"',b'ATD"host:65536"'):
            self.assertEqual(self.c.command(cmd),b"\rERROR\r",cmd)
    def test_command_overflow_recovers(self):
        self.c.prepare()
        self.assertEqual(self.c.command(b"AT"+b"x"*300),b"\rERROR\r")
        self.assertEqual(self.c.command(b"AT"),b"\rOK\r")
    def test_numeric_results(self):
        self.c.prepare(); self.c.send(b"ATV0\r"); self.assertEqual(self.c.receive(3),b"\r0\r")
    def test_raw_tcp_all_bytes_and_modem_like_payload(self):
        self.c.prepare()
        with Fixture() as server:
            self.c.dial(server.port)
            payload=bytes(range(256))+b"+++\r\nNO CARRIER\0\xff"
            self.c.send(payload); self.assertEqual(self.c.receive(len(payload)),payload)
            self.assertEqual(server.received,payload)
    def test_immediate_payload_and_final_bytes_before_eof(self):
        self.c.prepare(); payload=bytes(range(256))*4
        with Fixture(payload,close=True) as server:
            self.c.dial(server.port); self.assertEqual(self.c.receive(len(payload)),payload)
            self.c.wait(lambda: not self.c.read(6)&0x80)
            self.assertFalse(self.c.read(5)&1)
    def test_rts_backpressure_large_payload(self):
        self.c.prepare(); payload=bytes(range(256))*256
        with Fixture(payload,close=True) as server:
            self.c.dial(server.port)
            for _ in range(200): self.c.step(1000)
            self.assertFalse(self.c.read(5,True)&2)
            self.assertEqual(self.c.receive(len(payload)),payload)
    def test_successful_empty_connection_is_observable(self):
        self.c.prepare()
        with Fixture(close=True) as server:
            self.c.dial(server.port)
            self.c.wait(lambda: not self.c.read(6)&0x80)
            self.assertFalse(self.c.read(5)&1)
    def test_verbose_connect_result(self):
        self.c.prepare()
        with Fixture() as server:
            self.c.dial(server.port,quiet=False)
            self.assertEqual(self.c.receive(9),b"\rCONNECT\r")
    def test_dtr_disconnect_and_repeated_reconnect(self):
        self.c.prepare()
        for _ in range(3):
            with Fixture() as server:
                self.c.dial(server.port); self.c.send(b"test")
                self.assertEqual(self.c.receive(4),b"test")
                self.c.write(4,0x26); self.c.wait(lambda: not self.c.read(6)&0x80); self.c.write(4,0x27)
    def test_reset_discards_previous_connection_generation(self):
        self.c.prepare()
        with Fixture(b"old payload") as server:
            self.c.dial(server.port); self.c.reset(); self.c.init()
            self.assertFalse(self.c.read(6)&0x80)
            self.assertEqual(self.c.command(b"ATE0"),b"ATE0\r\r\nOK\r\n")
    def test_refused_connection(self):
        self.c.prepare()
        listener=socket.socket(); listener.bind(("127.0.0.1",0)); port=listener.getsockname()[1]; listener.close()
        self.c.send(f'ATD"127.0.0.1:{port}"\r'.encode())
        self.assertEqual(self.c.receive(11),b"\rNO ANSWER\r")

class TAPResult(unittest.TestResult):
    def addSuccess(self,test):
        super().addSuccess(test); print(f"ok {self.testsRun} - {test._testMethodName}")
    def addFailure(self,test,err):
        super().addFailure(test,err); self.fail(test,err)
    def addError(self,test,err):
        super().addError(test,err); self.fail(test,err)
    def fail(self,test,err):
        print(f"not ok {self.testsRun} - {test._testMethodName}")
        for line in "".join(traceback.format_exception(*err)).splitlines(): print("# "+line)

if __name__=="__main__":
    CARD=Card(sys.argv[1]); result=TAPResult()
    unittest.defaultTestLoader.loadTestsFromTestCase(SerialCardTests).run(result)
    print(f"1..{result.testsRun}")
    sys.exit(not result.wasSuccessful())
