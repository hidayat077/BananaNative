#!/usr/bin/env python3
# Unreal shader archives on a PC builder with no Mac host: dm/third_party/ooz's unoodle worker answers the decode protocol of uepak.OodleHost (DM_OODLE_OOZ).
# shader_split.py calls decode() when DM_UE_DECODE names an output folder. The Mac NSP script's decode and the phone's own decoder stay the primary paths; this
# decoder is not the game's own, so pack_android.py accepts its split only when its shader keys are exactly the profile's.
import os, struct, sys, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../../tools')); sys.path.insert(0, HERE)
import uepak
from romfs import os as ros


class OozWorker(uepak.OodleHost):
    """OodleHost with the open-source ooz worker instead of the game's own decoder; a failure names the worker"""
    def __init__(self, worker):
        super().__init__(worker, '-')

    def __call__(self, comp, n):
        try: return super().__call__(comp, n)
        except ValueError as e: raise ValueError(str(e).replace("the game's Oodle in the Mac host", "the ooz worker (DM_OODLE_OOZ; not the game's own decoder)")) from None


def stamp(romfs):
    """The paks and containers under the RomFS with their sizes (the NSP script's ue_stamp)"""
    out = []
    for dp, _, fs in ros.walk(romfs):
        for f in fs:
            if f.endswith(('.pak', '.utoc')):
                p = os.path.join(dp, f); out.append(f'{os.path.relpath(p, romfs)}\t{ros.stat(p).st_size}\n')
    return ''.join(sorted(out))


def rmtree(p):
    """<p> and everything in it. A file already gone is no error: a Mac's AppleDouble metadata on an exFAT drive can be removed with its file mid-walk"""
    if os.path.isdir(p):
        for dp, ds, fs in os.walk(p, topdown=False):
            for f in fs:
                try: os.remove(os.path.join(dp, f))
                except FileNotFoundError: pass
            for d in ds: os.rmdir(os.path.join(dp, d))
        os.rmdir(p)


def decode(romfs, main_nso, out, worker):
    """<out> holds the decoded shader code archives of the RomFS's paks (made now, or kept from an earlier run with the same paks); → out"""
    st = stamp(romfs)
    sf = os.path.join(out, 'stamp')
    if os.path.isfile(sf) and open(sf).read() == st:
        print(f'ue: reusing the decoded shader archives in {out}', flush=True); return out
    t0 = time.time()
    work, part = out + '.job', out + '.partial'
    rmtree(work); rmtree(part)
    dec = None
    try:
        if uepak.io_shader_tocs(romfs):
            dec = OozWorker(worker)   # IoStore libraries need a decoder before uepak.job can list their shader groups
        listing, n = uepak.job(romfs, main_nso, work, decode=dec)
        if not listing: raise ValueError(f'the RomFS has Unreal paks, but none under {romfs} holds a shader code archive')
        print('ue: ' + '; '.join(f'{x["name"]} {x["shaders"]:,} shaders' + (f' ({x["compression"]})' if x.get('compression') else '') for x in listing)
              + (f'; decoding {n:,} with the ooz worker' if n else ''), flush=True)
        if n:
            dec = dec or OozWorker(worker)
            with open(os.path.join(work, 'oodle.job'), 'rb') as f, open(os.path.join(work, 'oodle.job.out.partial'), 'wb') as o:
                magic, count = struct.unpack('<II', f.read(8))
                if magic != uepak.JOB_MAGIC or count != n: raise ValueError('the Oodle job file does not match its listing')
                o.write(struct.pack('<II', uepak.OUT_MAGIC, n))
                for k in range(n):
                    size, raw = struct.unpack('<II', f.read(8))
                    d = dec(f.read(size), raw)
                    o.write(struct.pack('<I', len(d))); o.write(d)
                    if k % 2000 == 0: print(f'ue: decoded {k:,} of {n:,}', flush=True)
            os.replace(os.path.join(work, 'oodle.job.out.partial'), os.path.join(work, 'oodle.job.out'))
    finally:
        if dec: dec.close()
    uepak.finish(work, part)
    open(os.path.join(part, 'stamp'), 'w').write(st)
    rmtree(out); os.replace(part, out); rmtree(work)
    print(f'ue: {len(listing)} archives decoded into {out} in {time.time() - t0:.1f} s', flush=True)
    return out
