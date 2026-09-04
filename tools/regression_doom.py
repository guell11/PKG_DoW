#!/usr/bin/env python3
"""Small boot regression runner for a local DOOM/crupsti test folder.

A surviving process is not called "100% compatible"; it is only a smoke-test signal that the
core crossed the early-boot window without a fatal exit.
"""
from __future__ import annotations
import argparse, json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import pkg_dow_server as core

TOKENS = ('doom', 'crupsti', 'crispy')
FATAL = ('exit_not_implemented', 'not implemented', 'access violation', '0xc0000005',
         'vk_error_device_lost', 'segmentation fault', 'fatal')


def find_target() -> Path | None:
    candidates = []
    for p in ROOT.rglob('*'):
        try:
            if p.is_dir() and any(t in p.name.lower() for t in TOKENS):
                if (p / 'eboot.bin').is_file() or next(p.rglob('eboot.bin'), None):
                    candidates.append(p)
        except (OSError, PermissionError):
            pass
    return sorted(candidates, key=lambda p: len(p.parts))[0] if candidates else None


def engines() -> dict[str,str]:
    ps5 = [ROOT/'_Build/windows/kyty_emulator.exe', ROOT/'_Build/windows/install/kyty_emulator.exe', ROOT/'engines/ps5/kyty_emulator.exe', ROOT/'kyty_emulator.exe']
    ps4 = [ROOT/'engines/ps4/shadPS4.exe', ROOT/'shadPS4.exe']
    def first(xs):
        return str(next((p.resolve() for p in xs if p.is_file()), ''))
    return {'ps5': first(ps5), 'ps4': first(ps4)}


def main() -> int:
    ap=argparse.ArgumentParser(); ap.add_argument('--timeout', type=int, default=30); ns=ap.parse_args()
    target=find_target(); out={'time':int(time.time()),'target':'','result':'SKIP','reason':'','exit_code':None,'survived_seconds':0}
    if target is None:
        out['reason']='Nenhuma pasta DOOM/crupsti com eboot.bin encontrada'; print(json.dumps(out,ensure_ascii=False,indent=2)); return 2
    out['target']=str(target)
    item=core.describe_import(str(target)); platform_name=str(item.get('platform','Auto')).upper(); es=engines()
    if platform_name=='PS4':
        exe=es['ps4']; eboot=target/'eboot.bin'
        if not eboot.is_file(): eboot=next(target.rglob('eboot.bin'))
        args=[exe,str(eboot)] if exe else []
    else:
        exe=es['ps5']; args=[exe,'--game',str(target),'--present-mode','Fifo','--shader-optimization-type','None','--printf-direction','Console','--shader-log-direction','Console','--redzone'] if exe else []
    if not exe:
        out['reason']=f"Core {'PS4' if platform_name=='PS4' else 'PS5'} ausente"; print(json.dumps(out,ensure_ascii=False,indent=2)); return 3
    log_dir=ROOT/'logs'; log_dir.mkdir(exist_ok=True); log_path=log_dir/f"doom-smoke-{int(time.time())}.log"
    start=time.time()
    with log_path.open('w',encoding='utf-8',errors='replace') as log:
        log.write('CMD: '+subprocess.list2cmdline(args)+'\n'); log.flush()
        proc=subprocess.Popen(args,cwd=str(Path(exe).parent),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace',bufsize=1)
        lines=[]
        try:
            while True:
                if proc.stdout:
                    line=proc.stdout.readline()
                    if line:
                        print(line,end=''); log.write(line); log.flush(); lines.append(line)
                code=proc.poll()
                elapsed=time.time()-start
                if code is not None:
                    out['exit_code']=int(code); out['survived_seconds']=round(elapsed,2); break
                if elapsed>=ns.timeout:
                    out['survived_seconds']=round(elapsed,2); proc.terminate()
                    try: proc.wait(3)
                    except subprocess.TimeoutExpired: proc.kill()
                    break
        finally:
            if proc.poll() is None: proc.kill()
    text=''.join(lines).lower(); signature=next((x for x in FATAL if x in text),'')
    if signature:
        out['result']='FAIL'; out['reason']=f'Assinatura fatal: {signature}'
    elif out['exit_code'] not in (None,0):
        out['result']='FAIL'; out['reason']=f'Engine encerrou com código {out["exit_code"]}'
    elif out['survived_seconds']>=ns.timeout:
        out['result']='BOOT_SURVIVED'; out['reason']=f'Sobreviveu à janela de smoke test de {ns.timeout}s'
    else:
        out['result']='EXITED'; out['reason']='Encerrou sem assinatura fatal durante smoke test'
    out['log']=str(log_path)
    report=ROOT/'logs'/'doom-smoke-latest.json'; report.write_text(json.dumps(out,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(out,ensure_ascii=False,indent=2)); return 0 if out['result']=='BOOT_SURVIVED' else 1

if __name__=='__main__': raise SystemExit(main())
