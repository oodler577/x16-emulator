#!/usr/bin/env python3
"""Full emulator CPU/IO/network echo integration, using a local server only."""
import argparse, os, subprocess
from pathlib import Path
from test_serialcard import Fixture

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--emulator",required=True)
    p.add_argument("--rom",required=True)
    p.add_argument("--guest",default=str(Path(__file__).with_name("serialcard_guest.bin")))
    p.add_argument("--x16net",help="optional existing x16net.bin; guest must be built with USING_LIBRARY=1")
    p.add_argument("--clean-path",action="store_true",help="exclude build-tool DLLs from the child Windows PATH")
    args=p.parse_args()
    child_env=dict(os.environ,SDL_VIDEODRIVER="dummy",SDL_AUDIODRIVER="dummy")
    if args.clean_path:
        if os.name != "nt": raise RuntimeError("--clean-path requires Windows")
        root=os.environ["SystemRoot"]
        child_env["PATH"]=os.pathsep.join([str(Path(root)/"System32"),root])
        child_env.pop("SDL_DYNAMIC_API",None)
    with Fixture() as server:
        image=Path(args.guest).read_bytes()
        if image.count(b":00000") != 1: raise RuntimeError("guest endpoint marker is missing/ambiguous")
        image=image.replace(b":00000",f":{server.port:05}".encode())
        writes=[(0x800,image)]
        if args.x16net: writes.append((0x8000,Path(args.x16net).read_bytes()))
        commands=[]
        for base,data in writes:
            commands.extend(f"STM {base+i:04X} {v:02X}" for i,v in enumerate(data))
        commands.append("RUN 0800")
        run=subprocess.run([str(Path(args.emulator).resolve()),"-rom",str(Path(args.rom).resolve()),"-serialcard","-testbench",
                            "-echo","raw","-sound","none","-warp"],
             input="\n".join(commands)+"\n",text=True,encoding="latin1",
             stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90,
             env=child_env)
        output=run.stdout.replace("\r","")
        lines=[line for line in output.splitlines()
               if line.startswith(("ok ","not ok","1..","User debug"))]
        print("\n".join(lines))
        if run.returncode or "not ok" in output or len([l for l in lines if l.startswith("ok ")]) != 3:
            raise RuntimeError("guest test failed: "+output[-2000:])
        expected=bytes(range(256))*16
        if server.received != expected: raise RuntimeError("host-side transfer bytes differ")
if __name__=="__main__":
    main()
