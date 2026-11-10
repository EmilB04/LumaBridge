#!/usr/bin/env python3
"""Full-app exit/restart/recovery checks in an isolated Wine prefix and X display.
Usage: python3 tools/dev/check-lifecycle.py --app /path/LumaBridge.exe --work /tmp/luma-qa --display :97
Requires Wine, an existing Xvfb display, xdotool and the MinGW x64 compiler.
"""
import argparse, configparser, json, os, pathlib, subprocess, time
p = argparse.ArgumentParser()
p.add_argument('--app', required=True); p.add_argument('--work', required=True); p.add_argument('--display', default=':97')
a = p.parse_args(); work = pathlib.Path(a.work).resolve(); work.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, WINEPREFIX=str(work/'wineprefix'), WINEDEBUG='-all', DISPLAY=a.display)
app = str(pathlib.Path(a.app).resolve()); processes = []; report = {}
users = work/'wineprefix/drive_c/users'
if not users.exists():
    subprocess.run(['wineboot', '-u'], env=env, check=True, timeout=60)
profile = next(x for x in users.iterdir() if x.is_dir() and x.name not in ('Public', 'All Users'))
folder = profile/'AppData/Local/LumaBridge'; folder.mkdir(parents=True, exist_ok=True)
ini = folder/'LumaBridge.ini'; original = ini.read_bytes() if ini.exists() else None
helper_source = work/'close-app.cpp'; helper = work/'close-app.exe'
helper_source.write_text('#include <windows.h>\nint main(){auto w=FindWindowW(L"LumaBridgeMainWnd",nullptr); if(!w)return 1;return PostThreadMessageW(GetWindowThreadProcessId(w,nullptr),WM_QUIT,0,0)?0:2;}')
subprocess.run(['x86_64-w64-mingw32-g++','-static',str(helper_source),'-o',str(helper)],check=True)
def configure(handoff=False):
    c=configparser.ConfigParser(interpolation=None, strict=False); c.optionxform=str
    if ini.exists(): c.read(ini, encoding='utf-8-sig')
    if not c.has_section('App'): c.add_section('App')
    for key,value in {'Running':'0','SetupDone':'1','StartMinimized':'0','CloseToTray':'0','HandoffColorEnabled':str(int(handoff)),'HandoffColor':'123456'}.items(): c['App'][key]=value
    with ini.open('w') as f: c.write(f, space_around_delimiters=False)
def launch():
    log=(work/f'run-{len(processes)}.log').open('wb')
    proc=subprocess.Popen(['wine',app],env=env,stdout=log,stderr=subprocess.STDOUT); log.close(); processes.append(proc)
    until=time.monotonic()+25
    while time.monotonic()<until:
        found=subprocess.run(['xdotool','search','--name','^LumaBridge$'],env=env,capture_output=True,timeout=2)
        if found.returncode==0: return proc
        if proc.poll() is not None: raise RuntimeError(f'app exited early: {proc.returncode}')
        time.sleep(.1)
    raise RuntimeError('app window did not appear')
def close(proc):
    start=time.monotonic()
    subprocess.run(['wine',str(helper)],env=env,check=True,timeout=5,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    proc.wait(timeout=10)
    elapsed=time.monotonic()-start
    assert elapsed<8, f'normal shutdown took {elapsed:.2f}s'
    assert 'Running=0' in ini.read_text(), 'normal exit retained the crash marker'
    return elapsed
def logs(): return (folder/'lumabridge-app.log').read_text(errors='replace') if (folder/'lumabridge-app.log').exists() else ''
try:
    configure(); before=logs().count('previous run did not exit cleanly'); before_watchdog=logs().count('shutdown took too long')
    first=launch(); report['normal_exit_seconds']=close(first)
    second=launch(); assert logs().count('previous run did not exit cleanly')==before
    report['restart_exit_seconds']=close(second)
    assert logs().count('shutdown took too long')==before_watchdog
    configure(True)
    recovery=folder/'lumabridge.log-handoff.log'
    previous=recovery.read_text(errors='replace').count('Applying handoff color') if recovery.exists() else 0
    third=launch(); third.kill(); third.wait(timeout=5)
    until=time.monotonic()+8
    while time.monotonic()<until:
        if recovery.exists() and recovery.read_text(errors='replace').count('Applying handoff color')>previous: break
        time.sleep(.1)
    else: raise RuntimeError('forced exit did not start recovery')
    start=time.monotonic(); fourth=launch(); report['restart_during_recovery_seconds']=time.monotonic()-start
    report['final_exit_seconds']=close(fourth)
    report['forced_recovery_started']=True
    (work/'lifecycle-results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
finally:
    for proc in processes:
        if proc.poll() is None:
            try: close(proc)
            except Exception: proc.kill()
    if original is None: ini.unlink(missing_ok=True)
    else: ini.write_bytes(original)
