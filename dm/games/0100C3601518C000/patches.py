# Sid Meier's Civilization VII (0100C3601518C000): the rewriter's patches of this game's own builds.
# dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID.
# CUTS, OBS and FRAME_STEP as described in docs/reference/rewriter.md ("Registered patches").

CUTS = {
    # 1.5.1 (v1703936), main e656e23d5435311081509ba2a2ae81d3. The menu sorts a container whose element count
    # grows with play time (measured live: 0x800000 and 0x1400000 elements of 0x98 bytes). Its temp buffer is
    # allocated at main+0x33b9dc; when the Firaxis allocator refuses the ~1.2-2.5 GiB request the game writes
    # to the returned null at main+0x33ba08 and crashes. The two nops at +0x33b9f8 become `cbz x27, +0x3cc`:
    # a null buffer skips the copy/sort and the whole container-update block (ending at +0x33bdc4), leaving the
    # original (still valid) container in place. A first cut to +0x33bb3c was too short: the game then crashed
    # at +0x33bb80 writing to the same null buffer minus 0x64. This is a deliberate cut of the game's own code,
    # not a host fix: the oversized request cannot be satisfied by the heap (global region is 0x7f000000).
    # The same pattern exists at main+0x344334 (the same allocator wrapper, another 0x98-byte copy loop); with
    # the first guard in place the phone crashed there next (main+0x34417c, null write), so that site returns early too.
    # The guard sits on the second of two mov x22,x20 instructions: replacing the first broke the empty-range path
    # (a phone run then crashed at main+0x3426c0 with x22=0).
    'e656e23d5435311081509ba2a2ae81d3': [
        (0x33b9f8, '1f2003d51f2003d5', '7b1e00b41f2003d5',
         'menu sort: cbz x27 to main+0x33bdc4 when the temp buffer is null; skip the whole sort/container-update block and keep the original container'),
        (0x34434c, 'f60314aa', 'b60300b4',
         'menu sort (second site): cbz x22 to the epilogue main+0x3443c0 when its temp buffer is null; the first mov x22,x20 stays so the empty-range path keeps a valid x22'),
    ],
}

# How general each patch is and could be: what, its binding now, the loosest it could have (goal), purpose, why,
# blocked or kept: why the gap stays open; checked: what was checked on game files.
BINDING = [
    {'what': 'menu sort with an unsatisfiable temp-buffer allocation on Civilization VII 1.5.1',
     'now': 'game', 'goal': 'game', 'purpose': 'runtime',
     'why': 'the oversized request and the unchecked null write are in the game\'s own main of this build; no Horizon or NVN semantics the host could implement are on this path. The cut changes the game\'s behaviour on purpose: it leaves the sort un-applied instead of crashing. The underlying growth of the sorted container is not fixed',
     'checked': 'the original bytes were read from the registered build\'s main; the replacement cbz was disassembled to main+0x33bdc4 (the next phase, after the whole container-update block). A phone run with an earlier, shorter branch (+0x33bb3c) proved the guard fired but the game then crashed at +0x33bb80 on the same null buffer, hence the wider skip; this version is not yet checked on a phone. Open: this is a workaround; why the container grows to millions of 0x98-byte records at the menu, and why the game asks for a buffer no allocator can satisfy, is still unknown'},
]
