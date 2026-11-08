"""Build and run real UDP tests with two clients and a full-host rejection.
Run build.ps1 first, then set LLVM_MINGW and run this script on Windows.
UPnP is disabled here: tests never change a real router's configuration.
"""
from pathlib import Path
import os, shutil, socket, subprocess, time

root = Path(__file__).resolve().parents[1]
os.chdir(root)
tc = Path(os.environ['LLVM_MINGW']) / 'bin/x86_64-w64-mingw32-clang++.exe'
objects = list(Path('build/obj').glob('third_party_enet_*.o')) + list(Path('build/obj').glob('third_party_miniupnpc_*.o'))
assert len(objects) == 22, 'Run build.ps1 first'
juice = 'third_party/libjuice'
ice_sources = ['src/net/ice_room.cpp', 'src/net/ice_crypto.cpp',
               *map(str, Path('build/obj').glob('*libjuice_src_*.o'))]
assert len(ice_sources) > 2, 'Build the native ICE objects first'
ice_flags = [f'-I{juice}/include', '-DJUICE_STATIC']
ice_libs = ['-lwinhttp', '-lbcrypt', '-lcrypt32', '-ladvapi32']
out = Path('build/direct-tests'); out.mkdir(exist_ok=True)
base = [str(tc), '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00', '-DMINIUPNP_STATICLIB',
        '-Isrc', '-I../../shared', '-I../../shared/protocol', '-Ithird_party/enet/include', '-Ithird_party/miniupnpc/include']
for name, sources, libs in [
    ('protocol', ['tests/direct_protocol.cpp', 'src/net/address_service.cpp', 'src/net/http_client.cpp', *ice_sources, *map(str, Path('build/obj').glob('third_party_enet_*.o'))], ['-lws2_32','-lwinmm','-luser32',*ice_libs]),
    ('upnp', ['tests/upnp_mapping.cpp', 'src/net/upnp_igd.cpp'], [])
]:
    exe = out/(name+'.exe')
    subprocess.run([*base, *ice_flags, *sources, '-static', *libs, '-o', str(exe)], check=True)
    subprocess.run([str(exe.resolve())], check=True)
subprocess.run([str(tc), '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00', '-DMINIUPNP_STATICLIB',
    '-Isrc', '-I../../shared', '-I../../shared/protocol', '-Ithird_party/enet/include', '-Ithird_party/miniupnpc/include',
    *ice_flags, *ice_sources, 'tests/direct_network.cpp', 'src/net/direct_api.cpp', 'src/net/upnp_igd.cpp', 'src/net/address_service.cpp', 'src/net/http_client.cpp', *map(str,objects),
    '-static', '-lws2_32', '-lwinmm', '-liphlpapi', *ice_libs, '-o', str(out/'network.exe')], check=True)
# Enable the vendored local TURN server in the test executable only.
juice_objects = list(Path('build/obj').glob('third_party_libjuice_src_*.o'))
for name in ('juice', 'server'):
    obj = out / (name + '_with_server.o')
    subprocess.run([str(tc.with_name('x86_64-w64-mingw32-clang.exe')),
                    '-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WINNT=0x0A00', *ice_flags,
                    f'-I{juice}/include/juice', f'-I{juice}/src',
                    '-c', f'{juice}/src/{name}.c', '-o', str(obj)], check=True)
    juice_objects.remove(Path(f'build/obj/third_party_libjuice_src_{name}_c.o'))
    juice_objects.append(obj)
relay_test = out / 'relay_network.exe'
subprocess.run([*base, *ice_flags, 'tests/relay_network.cpp', 'src/net/ice_crypto.cpp',
                *map(str, juice_objects), '-static', '-lws2_32', '-liphlpapi',
                *ice_libs, '-o', str(relay_test)], check=True)
subprocess.run([str(relay_test.resolve())], timeout=50, check=True)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
    s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
def run_group(group):
    processes = []
    try:
      for label,mode in group:
        folder = out/label; folder.mkdir(exist_ok=True)
        shutil.copy2(out/'network.exe', folder/'network.exe')
        (folder/'HSMMod.ini').write_text(f'[net]\nname={label}\nupnp=0\npublic_ip_lookup=0\nport={port}\nconnect=127.0.0.1:{port}\n')
        p = subprocess.Popen([str((folder/'network.exe').resolve()), mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        processes.append((label,p))
        time.sleep(0.35 if label == 'client2' else 0.15)
      results=[]
      for label,p in processes:
        output=p.communicate(timeout=25)[0].decode(errors='replace')
        print(f'{label}: exit={p.returncode}\n{output}')
        results.append(p.returncode==0 and 'PASS' in output)
      assert all(results), 'UDP integration failed'
    finally:
      for _,p in processes:
        if p.poll() is None: p.kill(); p.wait()

run_group([('host','host'), ('bindfail','bindfail'), ('client1','client'), ('client2','client'), ('full','reject')])
run_group([('lifecycle-host','lifecycle-host'), ('lifecycle-client','lifecycle-client')])
run_group([('timeout','timeout')])
