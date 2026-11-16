#!/usr/bin/env python3
"""Bundle the PE import closure, using exact DLL filenames from objdump."""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SYSTEM_DLLS = set('advapi32 bcrypt cfgmgr32 comctl32 comdlg32 crypt32 d3d9 d3d11 dbghelp dinput8 dnsapi dsound dwmapi dxgi gdi32 hid imm32 iphlpapi kernel32 kernelbase msvcrt ntdll ole32 oleaut32 opengl32 powrprof propsys psapi rpcrt4 secur32 setupapi shell32 shlwapi user32 userenv usp10 uxtheme version winhttp wininet winmm winspool wintrust ws2_32 wtsapi32 ucrtbase normaliz ncrypt shcore'.split())

def main():
    executable = Path(sys.argv[1]).resolve()
    runtime = Path(sys.argv[2])
    available = {p.name.lower(): p for p in runtime.glob('*.dll')}
    # Windows supplies imports such as WLDAP32.dll. Resolve from the actual
    # system directory as well as the baseline list, while always preferring
    # bundled runtime dependencies over similarly named system files.
    system_root = os.environ.get('SystemRoot')
    system_files = {p.name.lower() for p in (Path(system_root)/'System32').glob('*.dll')} if system_root else set()
    pending = [executable]
    seen = set()
    manifest = []
    while pending:
        path = pending.pop()
        if path.name.lower() in seen:
            continue
        seen.add(path.name.lower())
        imports = subprocess.check_output(['objdump', '-p', str(path)], text=True)
        for name in re.findall(r'DLL Name:\s*(\S+)', imports):
            key = name.lower()
            if key.startswith(('api-ms-win-', 'ext-ms-win-')) or key.removesuffix('.dll') in SYSTEM_DLLS:
                continue
            if key not in available and key in system_files:
                manifest.append(f'{path.name} -> {name} (Windows system)')
                continue
            if key not in available:
                raise RuntimeError(f'{path.name}: unresolved non-system dependency {name}')
            target = executable.parent / available[key].name
            if not target.exists():
                shutil.copy2(available[key], target)
            pending.append(target)
            manifest.append(f'{path.name} -> {target.name}')
    (executable.parent / 'runtime-dependencies.txt').write_text('\n'.join(sorted(set(manifest)))+'\n', encoding='utf-8')
    prefix = runtime.parent
    candidates = [prefix/'etc/ssl/certs/ca-bundle.crt', prefix/'ssl/certs/ca-bundle.crt', runtime/'curl-ca-bundle.crt']
    bundle = next((p for p in candidates if p.is_file()), None)
    if bundle is None:
        raise RuntimeError('Missing CA bundle for portable TLS/HTTPS')
    import ssl
    assert ssl.create_default_context(cafile=str(bundle)).cert_store_stats()["x509_ca"] > 0
    shutil.copy2(bundle, executable.parent/'curl-ca-bundle.crt')
    licenses = prefix/'share/licenses'
    if not licenses.is_dir():
        raise RuntimeError('Missing runtime library license directory')
    shutil.copytree(licenses, executable.parent/'third-party-licenses', dirs_exist_ok=True)
    print('\n'.join(sorted(set(manifest))))

if __name__ == '__main__':
    main()
