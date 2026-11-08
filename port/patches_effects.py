"""The optional effects of REVIVED SETTINGS > Graphics > Performance (finding 183): each is switched
off where the game decides to draw it, by asking port/hle/pc_settings.c (port_fx_*). With a switch
on, the patched code does what the retail code does; off, the effect's draw data is not made or
not linked into the frame's chain. Nothing the game logic reads changes. Merged into
port/patches.py's PATCHES (same entry kinds, same checks).

- Shadows and sun glow: the riders' shadows (0021D250: func_0021D950 marks a shadow for the frame
  in scratchpad 0x70000208; every later step of it, the riders' silhouettes, the blur, the second
  scene walk and the ground's darkening, runs only for a marked one) and the sun's glow chain
  (00228328: the sun's request to func_00238740, the only place that sets the chain's pending flag
  D_0029E3A0, which func_002388F0 reads). The sun disc is the flare's first pass and stays.
- Lens flare: the flare object's header gives VU1 program 0x26D8 its pass count (00225DB0 writes
  18, one per row of its tables): pass 0 the source's disc, pass 1 its halo, passes 2-17 the
  ghosts along the line through the screen's centre. Off: 2 passes. The flare's Z probe
  (func_00228328) is not touched.
- Particles: func_0022E560 adds each visible emitter's packet to the frame's chain (from the asm
  func_0022E9A0); off, it adds none. The heat haze's two 64 x 64 textures (VU1 program 0x29C0,
  packets made once by func_00237918) are drawn only for those emitters: off, the program's MSCAL
  and MSCNT words are VIF NOPs.
"""

FX = 'extern int port_fx_shadows(void), port_fx_lens_flare(void), port_fx_particles(void);\n'


def add(PATCHES):
    def more(unit, *entries):
        PATCHES['unsorted/' + unit] = PATCHES.get('unsorted/' + unit, []) + list(entries)

    # Shadows: no shadow is marked for the frame.
    more('0021D250.c',
         ('text', 'extern void func_0021E1B0(float *, float *);', 'extern void func_0021E1B0(float *, float *);\n' + FX),
         ('text', 'if ((particle->flags >> buffer) & 1) {', 'if (((particle->flags >> buffer) & 1) && port_fx_shadows()) {'))
    # Sun glow: the sun asks for no glow chain.
    more('00228328.c',
         ('text', 'extern void func_00238740(int, int, int);', 'extern void func_00238740(int, int, int);\n' + FX),
         ('text', '                func_00238740((int)(proj.x * 16.0f), (int)(proj.y * 16.0f), index);',
          '                if (port_fx_shadows()) func_00238740((int)(proj.x * 16.0f), (int)(proj.y * 16.0f), index);'))
    # Lens flare: the disc and its halo only.
    more('00226588.c',
         ('text', 'extern char D_26D8[];', 'extern char D_26D8[];\n' + FX),
         ('text', 'base[6]=count;', 'base[6]=count; base[5]=port_fx_lens_flare() ? 18 : 2;'))
    # Particles: no emitter's packet in the chain, and the haze textures' program not started.
    more('0022E560.c',
         ('text', 'extern int func_00219EE8(int, Entry *, int *);',
          'extern int func_00219EE8(int, Entry *, int *);\n' + FX + 'extern void port_fx_haze(int on);\n'),
         ('text', '    Entry *entry = &D_0067DF90[obj->x2814];\n    int n;\n',
          '    Entry *entry = &D_0067DF90[obj->x2814];\n    int n;\n\n'
          '    port_fx_haze(port_fx_particles());\n'
          '    if (!port_fx_particles()) return p;\n'))
    # The program runs in two parts: MSCAL 0x29C0 into the first target, then MSCNT (continue at
    # the address it stopped at) into the second. Both words become NOPs together: an MSCNT alone
    # would continue the program before it.
    more('002370E0.c',
         ('text', '    p3->x7C=((unsigned int)D_29C0>>3)|0x14000000;',
          '    p3->x7C=((unsigned int)D_29C0>>3)|0x14000000;\n'
          '    { extern void port_fx_haze_word(unsigned int *); port_fx_haze_word((unsigned int *)&p3->x7C); }'),
         ('text', '    p6->x0=0x11000000; p6->x4=0x17000000; p6->x8=0; p6->xC=0x11000000;',
          '    p6->x0=0x11000000; p6->x4=0x17000000; p6->x8=0; p6->xC=0x11000000;\n'
          '    { extern void port_fx_haze_word(unsigned int *); port_fx_haze_word((unsigned int *)&p6->x4); }'))
