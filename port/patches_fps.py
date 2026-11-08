"""Port-only real-step corrections. Retail sources/checks remain untouched.

Every edit is guarded at runtime by port_delta_clock(): the delta-time clock (paced play at
every rate, finding 139) takes these paths; only a deterministic run at 60 takes the original
arithmetic. Timestamp storage stays integer: on the delta-time clock animation timestamps use
1/1024-field units.
"""
from pathlib import Path
import re


def add(patches):
    root = Path(__file__).resolve().parents[1] / 'src' / 'unsorted'

    def edit(unit, old, new):
        paths = list(root.glob(unit + '.*'))
        assert len(paths) == 1, unit
        patches.setdefault('unsorted/' + paths[0].name, []).append(('text', old, new))

    def entry(unit, name, code):
        path = next(root.glob(unit + '.*'))
        text = path.read_text()
        m = re.search(r'\b' + name + r'\([^;{}]*\)\s*\{', text)
        assert m, name
        edit(unit, m[0], m[0] + '\n' + code + '\n')

    # Input edges must expire after each real update, even when the integer
    # legacy clock is unchanged. Keep timers/cooldowns in their original units.
    patches.setdefault('unsorted/001909A8.c', []).append(('re',
        (r'func_00238F48\(\)',2),'port_input_stamp(func_00238F48())'))
    edit('0021F8E0','int next=n-1;',
         'int next=port_delta_clock() ? port_count_down(n) : n-1;')
    edit('0021F8E0','++D_00660D60[k]',
         '(D_00660D60[k]+=port_delta_clock() ? port_whole_step() : 1)')
    edit('0017EB00','obj->counts[index]--;',
         'obj->counts[index]=port_delta_clock() ? port_count_down(obj->counts[index]) : obj->counts[index]-1;')
    edit('0017E440','else obj->x3EC++;',
         'else obj->x3EC+=port_delta_clock() ? port_whole_step() : 1;')

    # RIDER SELECTED and the shop flight gate their skip on elapsed 60 Hz
    # fields, not updates: a skip before the camera blend ends gets reset.
    # From n0buholic (PR #13).
    for suffix in ('__c62', '__c16'):
        edit('001EB1D8', 'D_0029DF70%s++;' % suffix,
             'D_0029DF70%s += port_delta_clock() ? port_whole_step() : 1;' % suffix)

    # Reset must take place after the original initializer too.
    edit('00238A68', '*(volatile int *)0x10000800 = 0;',
         '*(volatile int *)0x10000800 = 0;\n    if (port_delta_clock()) port_clock_reset();')
    edit('00238C00', 'extern int func_00243CE8(int);',
         'extern int func_00243CE8(int);\nextern void rate_audit_update(void);')
    entry('00238C00', 'func_00238C00',
          '    rate_audit_update();\n    if (port_delta_clock()) { port_clock_wait(result); return 1; }')
    entry('00238D98', 'func_00238D98',
          '    if (port_delta_clock()) { port_clock_step(); return; }')
    entry('0011A100', 'func_0011A100','    port_fps_race_step();')
    edit('00135A30','func_0012DDC0(o->x158);',
         'func_0012DDC0(o->x158); port_fps_contact_recovery(o);')
    edit('001336F8','func_00231C10(obj);','port_fps_force_window(obj,1); func_00231C10(obj);')

    # AI's rotating expensive-query slot and seven-second leader refresh are
    # field schedules, not integration work. A held slot must not run its
    # queries repeatedly on the intervening fractional updates.
    # The rotating slot moves one rider per update that ends a field (as retail moves it once a
    # frame): a two-field step moving it by two would skip every other rider (finding 139).
    edit('0011A100', '++race->x501B4',
         '(race->x501B4 += port_delta_clock() ? port_whole_step()>0 : 1)')
    edit('0011A100', '--race->x501C0',
         '(race->x501C0 -= port_delta_clock() ? port_whole_step() : 1)')

    # FPS1 (rate audit): two per-update counters that are time in disguise.
    # D_0029DBAC is the particle/effects frame clock: the dust, snow and spray waves
    # (001DF920 Wave), the glow phases (0022E9A0) and the effect matrix wobble
    # (0022E3D0) read it as a 60 Hz frame count, so at 240 they ran four times too
    # fast. Whole fields keep the retail 60 Hz phase and the 60 arithmetic identical.
    edit('00228BE8', 'D_0029DBAC += 1;',
         'D_0029DBAC += port_delta_clock() ? port_whole_step() : 1;')
    # obj->x60C is the sound/skat queue owner's age; func_001BC250 returns it as
    # milliseconds with *1000/60, so per-update counting at 240 reports 4x the time.
    edit('001BBF30', 'obj->x60C++;',
         'obj->x60C += port_delta_clock() ? port_whole_step() : 1;')
    for unit,expression in (
            ('0010EC80','*(int *)(obj->x0 + 0x797C) == D_002AF2C0.x501B4'),
            ('0010F0D8','obj->x0->x797C == D_002AF2C0.x501B4'),
            ('0010FB00','world->x797C==D_002AF2C0.x501B4')):
        edit(unit,expression,'((!port_delta_clock() || port_whole_step()) && ('+expression+'))')
    edit('00112360','o->x0->x797C != D_002AF2C0.x501B4',
         '(port_delta_clock() && !port_whole_step()) || o->x0->x797C != D_002AF2C0.x501B4')
    # The track's creatures (func_00125E38, type 6: ids 0x1018-0x1022) pick their mode once a
    # field. When the player has finished and stopped (speed < 0.1), one more than 12.2 units
    # from the rider (xD8 > 150, squared) turns to walk to a point 3 units ahead of it (mode 2),
    # and mode 2 ends at once (mode 1) within 10 units of that point. A creature that came from
    # ahead stops between the two, so every pick is two mode changes, each a 250 ms animation
    # blend started on top of the one still running (func_00126B20 -> func_00202E38). Retail
    # picks 60 times a second, about 30 blends deep; at 240 every update picked, about 120 deep,
    # and evaluating the chain (00204CE0 -> 00204F38 -> 00205D10 per level) ran out of call stack
    # 2 s after the finish (online, where the race then waits for the others). Picks stay on
    # whole fields: the chain is as deep as in retail.
    edit('00125E38', '        rider = D_0043EEC0[0];\n        if (rider != 0',
         '        rider = D_0043EEC0[0];\n        if (port_delta_clock() && !port_whole_step()) {\n'
         '        } else if (rider != 0')
    # Failed trick selections can be retried immediately: the cooldown is
    # zero until a choice succeeds. Polling that RNG on fractional updates
    # raises the probability of starting a trick and shifts later AI rolls.
    entry('00110870','func_00110870',
          '    if (port_delta_clock() && !port_whole_step()) return;')
    edit('0010C940','else if(func_00222BD8(0,1))',
         'else if(port_delta_clock() && !port_whole_step()) return;\n else if(func_00222BD8(0,1))')

    for unit in ('00136790', '00136B40', '00136C20', '00136F80'):
        path = next(root.glob(unit + '.*'))
        text = path.read_text()
        # Do not rewrite the accessor declaration.
        calls = list(re.finditer(r'func_00238F38\(\)', text))
        assert len(calls) == 1, (unit, len(calls))
        edit(unit, 'func_00238F38()', '(port_delta_clock() ? 60.0f : func_00238F38())')
    edit('00136038', 'SCALE(o->x70,o->x70,0.9f);',
         'SCALE(o->x70,o->x70,port_decay(0.9f)); port_fps_force_window(o,0);')
    # 00231CC0 applies these tire vectors directly to linear/angular impulse,
    # not the force accumulator. A fixed Coulomb cap per contact update would
    # give N/60 times the friction per second. Scale the cap, not the
    # unconstrained collision impulse needed to cancel relative motion.
    edit('00132CE0','pos = &a->x50;',
         'if (port_delta_clock()) friction*=port_step();\n    pos = &a->x50;')
    edit('00161B10','bike->x434 > D_0029C7D0',
         'bike->x434 > (port_delta_clock() ? D_0029C7D0*port_step() : D_0029C7D0)')
    # Sliding tire modes bypass the Coulomb cap above: these are sustained
    # per-contact linear/angular impulses, not impact restitution.
    edit('0016AFC0','VecScale(out, &sum, D_0029C864);',
         'VecScale(out, &sum, port_delta_clock() ? D_0029C864*port_step() : D_0029C864);')
    edit('0016ABC8','scale=func_0016AD98(D_0026EBE8,obj->xC);',
         'scale=func_0016AD98(D_0026EBE8,obj->xC); if (port_delta_clock()) scale*=port_step();')
    edit('0016ACE8','"f"(-func_0016AD98(D_0026EC18, obj->xC))',
         '"f"(-func_0016AD98(D_0026EC18, obj->xC)*(port_delta_clock() ? port_step() : 1.0f))')
    edit('00136F80','n = obj->x158->x7B00 - 1;',
         'n = obj->x158->x7B00 - (port_delta_clock() ? port_whole_step() : 1);')
    # The yaw/pitch velocities are applied with dt downstream (00137C78/E68),
    # but their acceleration and release were still per-update constants.
    edit('00137950', 'float step = accel * input;',
         'float step = accel * input; if (port_delta_clock()) step*=port_step();')
    edit('00137950', 'float next = vel * decay;', 'float next = vel * port_decay(decay);')
    edit('00137B70', 'obj->x454 * pitchInput * scale',
         'obj->x454 * pitchInput * scale * (port_delta_clock() ? port_step() : 1.0f)')
    edit('00137B70', 'obj->x240 * obj->x470', 'obj->x240 * port_decay(obj->x470)')
    edit('00137300', 'VecScale(specialDir, specialDir, D_0029C3B0);',
         'VecScale(specialDir, specialDir, port_decay(D_0029C3B0));')
    edit('00137300', 'specialScale = specialOne - D_0029C3B0;',
         'specialScale = specialOne - port_decay(D_0029C3B0);')
    edit('00137300', 'dir = &obj->x270;',
         'if (port_delta_clock()) scale=port_decay(scale);\n    dir = &obj->x270;')
    edit('001AF5E8', 'amount = amount * 0.05f + old * 0.95f;',
         'amount = port_delta_clock() ? old+(amount-old)*port_blend(0.05f) : amount * 0.05f + old * 0.95f;')
    # Remaining manual object/UI motion found outside the accessor callers.
    for unit,expression in [('001926E0','obj->x8->xA0+=0.015f;'),
                            ('001926E0','obj->x8->xA8+=0.003f;'),
                            ('0017C168','(*item)->x4c += 0.05f;')]:
        value=expression.rstrip(';').split('+=')[1].strip()
        edit(unit,expression,expression.replace(value,'(port_delta_clock() ? '+value+'*port_step() : '+value+')'))
    edit('001926E0','v.v[1]=speed;', 'v.v[1]=port_delta_clock() ? speed*port_step() : speed;')
    edit('0017C130','child->x10 += obj->x1288;',
         'child->x10 += port_delta_clock() ? obj->x1288*port_step() : obj->x1288;')
    edit('0017C130','obj->x1314--;','obj->x1314-=port_delta_clock() ? port_whole_step() : 1;')
    edit('0017C168','--obj->x1318','(obj->x1318=port_delta_clock() ? port_count_down(obj->x1318) : obj->x1318-1)')
    edit('0016FC00','obj->x224 += 0.03f;',
         'obj->x224 += port_delta_clock() ? 0.03f*port_step() : 0.03f;')
    edit('0016FC00','diff < obj->x224',
         'diff < (port_delta_clock() ? obj->x224*port_step() : obj->x224)')
    edit('0016FC00','obj->x1F0->x10.z -= obj->x224;',
         'obj->x1F0->x10.z -= port_delta_clock() ? obj->x224*port_step() : obj->x224;')
    edit('00142940','w = length * 0.9992f + D_0029DD3C * 1.2f - obj->pos[2] * -0.008f / length;',
         'w = port_delta_clock() ? length*port_decay(0.9992f)+(D_0029DD3C*1.2f-obj->pos[2]*-0.008f/length)*(port_particle_sum(0.9992f)/0.9992f) : length * 0.9992f + D_0029DD3C * 1.2f - obj->pos[2] * -0.008f / length;')
    edit('001B0410','obj->x20++;','obj->x20+=port_delta_clock() ? port_whole_step() : 1;')
    for unit,expression in [('001B4688','--obj->x8'),('001B4718','--obj->timer')]:
        edit(unit,expression,'('+expression[2:]+'-=port_delta_clock() ? port_whole_step() : 1)')
    edit('001B4900','D_00437340.x508--;',
         'D_00437340.x508=port_delta_clock() ? port_count_down(D_00437340.x508) : D_00437340.x508-1;')
    for field in ('x14','x18'):
        edit('001634E0','drain += gauge->'+field+';',
             'drain += port_delta_clock() ? gauge->'+field+'*port_step() : gauge->'+field+';')
    # Despite its provisional decomp name "dt", component +2110 is 1.0
    # in both 60 and 144 live state, not seconds/fields per real step.
    # Recovery and base depletion therefore need the same time conversion.
    edit('001634E0','gauge->fill * gauge->x24 * gauge->x28 * gauge->game->dt',
         'gauge->fill * gauge->x24 * gauge->x28 * gauge->game->dt * (port_delta_clock() ? port_step() : 1.0f)')
    edit('001634E0','gauge->drain * gauge->game->dt;',
         'gauge->drain * gauge->game->dt * (port_delta_clock() ? port_step() : 1.0f);')
    for unit,expression in [('001737F0','obj->xC->pos.z += 0.5f;'),
                            ('00173CB0','obj->xC->pos[2]+=0.5f;'),
                            ('00173CB0','obj->xC->pos[2]-=0.5f;')]:
        edit(unit,expression,expression.replace('0.5f','(port_delta_clock() ? 0.5f*port_step() : 0.5f)'))
    # All seven retail callers apply a partial delta quaternion every update
    # (camera attitude / ground-following actors). This is the shared nlerp
    # toward identity, rather than a one-shot asset-space quaternion scale.
    edit('00223578','{\n\n    __asm__ volatile ("lqc2 $vf11, 0(%0)"',
         '{\n    if (port_delta_clock()) scale=port_blend(scale);\n    __asm__ volatile ("lqc2 $vf11, 0(%0)"')
    edit('00165B90','obj->x1A8++',
         '(port_delta_clock() ? (obj->x1A8+=port_whole_step())-port_whole_step() : obj->x1A8++)')
    edit('00148F60','progress = obj->x2F0 + obj->x2F4;',
         'progress = obj->x2F0 + (port_delta_clock() ? obj->x2F4*port_step() : obj->x2F4);')
    edit('00148F60','obj->x2F0 += obj->x2F4;',
         'obj->x2F0 += port_delta_clock() ? obj->x2F4*port_step() : obj->x2F4;')
    patches['unsorted/00148F60.cpp'].append(('re',(r'obj->x2F0 -= obj->x2F8',2),
        'obj->x2F0 -= port_delta_clock() ? obj->x2F8*port_step() : obj->x2F8'))
    patches['unsorted/00148F60.cpp'].append(('re',(r'obj->xA4 \* obj->x300',3),
        '(obj->xA4 * obj->x300 * (port_delta_clock() ? port_step() : 1.0f))'))
    # Two held directions, one release decay.
    patches.setdefault('unsorted/00138658.cpp', []).append(
        ('re', (r'rate\*0\.03f', 2), 'rate*(port_delta_clock() ? 0.03f*port_step() : 0.03f)'))
    edit('00138658', 'o->x488*=0.95f;', 'o->x488*=port_decay(0.95f);')
    edit('00135A30', '++o->x4A4;',
         'if (port_delta_clock()) o->x4A4=port_air_tick(o,o->x4A4); else ++o->x4A4;')
    edit('00135A30', 'if (o->x4A4 == 1)',
         'if (port_delta_clock() ? port_air_first(o,o->x4A4) : o->x4A4 == 1)')
    edit('00135A30', 'o->x48C *= D_0029C30C;', 'o->x48C *= port_decay(D_0029C30C);')
    for unit, expression, body in [('00136038','(float)o->x4a4','o'),
                                    ('00138658','(float)o->x4A4','o'),
                                    ('00137E68','(float)obj->x4a4','obj')]:
        path=next(root.glob(unit+'.*'))
        count=path.read_text().count(expression)
        patches.setdefault('unsorted/'+path.name,[]).append(('re',(re.escape(expression),count),
            '(port_delta_clock() ? port_air_age(%s,%s) : %s)' %
            (body,expression.removeprefix('(float)'),expression)))
    edit('00148F60', 'angle=(float)obj->x364*0.017453294f;',
         'angle=port_counter_fields(obj->x364)*0.017453294f;')
    # The pedal kick is a per-update impulse: spread it over the real steps.
    edit('00136790', 'len=strength*D_0029C31C;',
         'len=strength*D_0029C31C; if (port_delta_clock()) len*=port_step();')
    edit('0012F6A0', '--obj->x7A74', '(obj->x7A74 -= port_delta_clock() ? port_whole_step() : 1)')
    edit('0012F5D0', 'obj->x7a3c += speed;',
         'obj->x7a3c += port_delta_clock() ? speed*port_step() : speed;')
    edit('001AD2B8', 'ctrl->x34 += 0.05f;',
         'ctrl->x34 += port_delta_clock() ? 0.05f*port_step() : 0.05f;')
    for unit in ('001AD2B8', '00157950', '00239778', '002398C0'):
        path = next(root.glob(unit + '.*'))
        text = path.read_text()
        # Accessor calls on executable lines, never declarations.
        for line in text.splitlines():
            if 'func_00238F60()' in line and not line.lstrip().startswith('extern'):
                edit(unit, line, line.replace('func_00238F60()',
                     '(port_delta_clock() ? port_step() : (float)func_00238F60())'))
    for unit, args in [('00235EA8', ('rate', 'minimum')), ('00235F38', ('rate', 'minStep'))]:
        entry(unit, 'func_'+unit,
              '    if (port_delta_clock()) { %s=port_blend(%s); %s*=port_step(); }' % (args[0],args[0],args[1]))
    # Raw helpers now do the correction once; avoid the rate-aware helper
    # correcting it a second time, while keeping its original 30/60 paths.
    for unit, callee in [('00236088','00235EA8'),('00236130','00235F38')]:
        entry(unit, 'func_'+unit,
              '    if (port_delta_clock()) return func_%s(fparg0,fparg1,fparg2,fparg3);' % callee)

    # True fractional animation evaluation without changing any retail layout.
    for unit in ('00202D78', '002065C0'):
        edit(unit, 'D_0029E160 += func_00238F60();',
             'D_0029E160 += port_animation_scale()>1 ? port_animation_step() : func_00238F60();')
    edit('00202E00', 'p->x4->x1C * (float)(D_0029E160[0] - p->x4->x14)',
         'p->x4->x1C * port_animation_fields((float)(D_0029E160[0] - p->x4->x14))')
    edit('00202E00', '(float)p->x4->x14 * p->x4->x1C',
         'port_animation_fields((float)p->x4->x14) * p->x4->x1C')
    edit('00202E00', 'elapsed = (float)(now - (unsigned int)p->x4->x14);',
         'elapsed = port_animation_fields((float)(now - (unsigned int)p->x4->x14));')
    # the frame-time constants of the compiled timers; the sixth, in func_00204F38 (the pose
    # blend), is NONMATCHING C: its recompiled asm is scaled in recomp.py (finding 165)
    for timer in ('D_0029E160__c38 - fade->start', 'D_0029E160__c142 - obj->x48', 'D_0029E160__c43 - obj->x48',
                  'D_0029E160__c39 - obj->x48', 'D_0029E160__c42 - obj->x48'):
        edit('00202E00', '((float)%s) * 16.66667f' % timer,
             '((float)%s) * (port_animation_scale()>1 ? 16.66667f/1024 : 16.66667f)' % timer)
    edit('00206440', 'frame = D_0029E160;', 'frame = D_0029E160 / port_animation_scale();')
    # Execute all skeleton updates at high rates (the original scheduler skips
    # expensive animation updates by field period for distant models).
    edit('00206440', 'int apply = state == current;', 'int apply = port_delta_clock() || state == current;')
    # Existing EE-float patch in patches.py runs first.
    edit('00208708', 'ps2_fdiv((float)(func_00202DA8() - obj->startTick), (float)obj->duration)',
         'ps2_fdiv(port_animation_fields((float)(func_00202DA8() - obj->startTick)), (float)obj->duration)')
    # Tween callbacks otherwise remain scheduled on integer field boundaries,
    # even with a fractional clock. Only the float-tween trampoline runs on
    # every real step; ordinary gameplay callbacks retain their 60-Hz timers.
    edit('00236C60', 'extern unsigned int func_00238F48(void);',
         'extern unsigned int func_00238F48(void);\nextern unsigned int func_00263AC8(void *);')
    edit('00236C60', 'if (now >= timer->due)',
         'if (now >= timer->due || (port_delta_clock() && timer->callback == func_00263AC8))')
    # Two per-field callbacks also queue a 2D draw for the frame, in lists the game empties
    # every frame (func_002102A8): the screen fade (func_001F7840, started by func_001F79D8:
    # the title's fade-in, menu changes, the bike shop) and the training hint box
    # (func_002626B8 -> func_001E41A0, game mode 11). Run on whole fields only, they left
    # the updates in between without the fade (pictures flashing at full brightness, about
    # every other picture) or without the hint (finding 086, "Flicker at 144 (second look)").
    # On an update that didn't advance the field clock, port_timer_hold queues the same draw
    # again, as the callback's last run left it, and changes no state: the fade still moves
    # once a field. port_between is never set at 60.
    # The two nodes are told apart by what they do, not by their callback's address: the
    # fade's starter (func_001F79D8) is recompiled MIPS, so the pointer it stores is its own
    # ABI's (the wasm core's ()->void thunk of func_001F7840), not the one C code would take.
    # The fade's callback is the one whose run queued the fade sprite (D_004C67C0) in view 0's
    # list; the hint's node is the one with the hint object (D_004C5640) as its context.
    edit('00236C60', 'extern unsigned int func_00263AC8(void *);',
         'extern unsigned int func_00263AC8(void *);\n'
         'extern int func_001E41A0(void *);\n'
         'extern unsigned char D_0029D7F4;\n'
         'extern unsigned int *D_0029DED4;\n'
         'extern char D_00513B00[], D_004C67C0[], D_004C5640[];\n'
         'extern void func_002103D8(void *, int, void *);\n'
         'int port_timer_redraw = 0;  /* func_001E41A0 (port patch): draw as its last run did */\n'
         'static unsigned int port_timer_now = 0xFFFFFFFFu;\n'
         'static TimerCallback port_fade_callback;  /* the screen fade\'s, once a run queued its sprite */\n'
         '/* view 0\'s list end: func_002103D8 leaves its end tag there */\n'
         'static unsigned int *port_list_end(void) { return *(unsigned int **)D_00513B00; }\n'
         'static void port_timer_learn(TimerNode *timer, unsigned int *before)\n'
         '{\n'
         '    unsigned int *end = port_list_end(), *q;\n'
         '    for (q = before; q < end && q < before + 64 * 4; q += 4)\n'
         '        if (q[0] == 0x50000000u &&\n'
         '            !((q[1] ^ (unsigned int)(unsigned long)D_004C67C0) & 0x01FFFFFFu))\n'
         '            port_fade_callback = timer->callback;\n'
         '}\n'
         'static void port_timer_hold(TimerNode *timer)\n'
         '{\n'
         '    if (port_fade_callback && timer->callback == port_fade_callback) {\n'
         '        /* as func_001F7840 queues it (no run yet: as func_001F79D8 did) */\n'
         '        if (D_0029D7F4 && D_0029DED4 && *D_0029DED4) func_002103D8(D_00513B00, 0, D_004C67C0);\n'
         '    } else if (timer->context == (void *)D_004C5640) {\n'
         '        port_timer_redraw = 1;\n'
         '        func_001E41A0(timer->context);\n'
         '        port_timer_redraw = 0;\n'
         '    }\n'
         '}')
    edit('00236C60', '    now = func_00238F48();\n',
         '    now = func_00238F48();\n'
         '    int port_between = port_delta_clock() && now == port_timer_now;\n'
         '    port_timer_now = now;\n')
    edit('00236C60', '            if (timer->context != 0) {\n',
         '            unsigned int *port_before = port_delta_clock() && !port_fade_callback ? port_list_end() : 0;\n'
         '            if (timer->context != 0) {\n')
    edit('00236C60', '                timer->due = ((TimerCallbackNoArg)timer->callback)();\n            }\n',
         '                timer->due = ((TimerCallbackNoArg)timer->callback)();\n            }\n'
         '            if (port_before) port_timer_learn(timer, port_before);\n')
    edit('00236C60', '        } else {\n            timer = timer->next;\n        }',
         '        } else {\n            if (port_between && timer->due == now + 1) port_timer_hold(timer);\n'
         '            timer = timer->next;\n        }')
    # The hint box's draw-only pass: what its last run drew (none if that run ended the hint
    # or ran in an earlier field), with no counter or flag changed.
    edit('001E41A0', 'extern void func_0020F9F0(int, int, int, int, char *);',
         'extern void func_0020F9F0(int, int, int, int, char *);\n'
         'extern int func_00238F48(void);\n'
         'extern int port_timer_redraw;\n'
         'static unsigned int port_hint_field = 0xFFFFFFFFu;  /* the field its last run drew in */')
    edit('001E41A0', '    struct Rider *rider;\n\n',
         '    struct Rider *rider;\n\n'
         '    if (port_timer_redraw) {\n'
         '        if (port_hint_field != (unsigned int)func_00238F48()) return 1;\n'
         '        goto port_draw;\n'
         '    }\n')
    edit('001E41A0', '            p->x510 = -1;\n            return 1;',
         '            p->x510 = -1;\n            port_hint_field = 0xFFFFFFFFu;\n            return 1;')
    edit('001E41A0', '    func_0023D650(0, &width, &height);\n    half = width / 2;',
         '    port_hint_field = (unsigned int)func_00238F48();\n'
         'port_draw:\n'
         '    func_0023D650(0, &width, &height);\n    half = width / 2;')
    # Race replays on the delta-time clock (finding 142, port/hle/replay.c): the recorder keeps
    # one sample per field of race clock and a replay plays them with the recorded steps (the
    # race's field grid, fps.c). 0019EE98 asks for one-field steps from the race load to GO;
    # 001A9640, between the clock's wait and its step, stores or forces the step; the attract
    # demo's camera cuts and its two minutes count fields.
    entry('001A8DE8', 'func_001A8DE8',
          '    if (port_delta_clock()) return port_replay_race(obj);')
    entry('001A9640', 'func_001A9640',
          '    if (port_delta_clock()) { port_replay_frame(arg0); return; }')
    entry('0019EE98', 'func_0019EE98',
          '    if (port_delta_clock()) port_replay_race_phase(race->state);')
    # The time trial's ghost (mode 18) samples its rider every D_0029C3C4 (8) fields of race
    # clock and draws the best run between its samples. A two-field step past the mark keeps
    # its extra field (retail starts the count again from 0), so a race second holds the same
    # samples at every rate; the drawing takes the part of a field the clock is past too.
    edit('00138E08', 'obj->x316ec = 0;',
         'obj->x316ec = port_delta_clock() ? obj->x316ec - D_0029C3C4 : 0;')
    edit('00138E08', 'func_00138868(obj,(float)obj->x316ec/(float)D_0029C3C4);',
         'func_00138868(obj,(port_delta_clock() ? port_counter_fields(obj->x316ec) : (float)obj->x316ec)'
         '/(float)D_0029C3C4);')
    edit('0020CDF0', 'p->rise*(float)dt', 'p->rise*(port_delta_clock() ? port_step() : (float)dt)')
    edit('0020CDF0', 'p->fall*(float)dt', 'p->fall*(port_delta_clock() ? port_step() : (float)dt)')
    edit('00154058', 'obj->x30.z -= fvar;',
         'obj->x30.z -= port_delta_clock() ? fvar*port_step()*port_step() : fvar;')
    edit('0021F210', 'func_00238F30() == 0x3C', '(port_delta_clock() || func_00238F30() == 0x3C)')
    # Every other reader of the steps a second (func_00238F30) takes a time step from it: 1/rate
    # (the rigid-body integrator 002327B0, the contact solver, 00209248's callers) or the rate
    # itself. On the delta-time clock they read the exact step, not the integer rate
    # (port_step_seconds / port_step_rate; fps.c says why). The original path keeps retail's own
    # expression in the unit (its float division rounds as the EE's does).
    # The rate reads first (each is a "(float)func_00238F30()" not after "1.0f /"), then the 1/rate
    # ones, so neither patch matches the retail expression the other keeps.
    seconds = r'1\.0f\s*/\s*(?:\(float\)\s*)?func_00238F30\(\)'
    rate = r'(?<!1\.0f / )(?<!1\.0f/)\(float\)func_00238F30\(\)'
    readers = 0
    for path in sorted(root.glob('*.*')):
        if path.stem in ('00238F30', '0021F210'):
            continue
        text = path.read_text()
        for pattern, value in ((rate, 'port_step_rate'), (seconds, 'port_step_seconds')):
            count = len(re.findall(pattern, text))
            if count:
                readers += count
                patches.setdefault('unsorted/' + path.name, []).append(
                    ('re', (pattern, count), r'(port_delta_clock() ? %s() : \g<0>)' % value))
        assert len(re.findall(r'func_00238F30\(\)', text)) == len(re.findall(seconds, text)) + len(re.findall(rate, text)), path
    assert readers == 22, readers  # a new reader needs a look: is it a time step?
    entry('00239A80', 'func_00239CE0',
          '    if (port_delta_clock()) { port_ragdoll_step(sys->particles,sys->count,sizeof(Particle),D_0029DC74); return; }')
    edit('00239A80', 'scale = -(dist * 0.99f);', 'scale = -(dist * port_blend(0.99f));')
    patches['unsorted/00239A80.c'].append(('re',(r'D_0029DC84 \* 0\.5f',2),'port_blend(D_0029DC84) * 0.5f'))
    for field in ('xDD8','xDD0','xDE4'):
        edit('001A4F70', 'obj->%s++;' % field,
             'obj->%s+=port_delta_clock() ? port_whole_step() : 1;' % field)
    edit('001A4F70', 'obj->xDF8 = VLength(&normal);',
         'obj->xDF8 = VLength(&normal); if (port_delta_clock()) obj->xDF8/=port_step();')
    edit('001A4F70', 'Scale__c4(&lateral, &lateral, D_0029D34C);',
         'Scale__c4(&lateral, &lateral, port_decay(D_0029D34C));')
    for constant in ('D_0029D3A8','D_0029D3AC'):
        edit('001A4F70', '"f"(%s)' % constant,
             '"f"(port_delta_clock() ? %s*port_step()*port_step() : %s)' % (constant,constant))
    edit('00161B10', 'VSCALE(delta,&delta,1.0f/count);',
         'VSCALE(delta,&delta,1.0f/(port_delta_clock() ? port_step() : (float)count));')
    edit('00161B10', '} else effect->x2724--;',
         '} else effect->x2724=port_delta_clock() ? port_count_down(effect->x2724) : effect->x2724-1;')
    # Further source audit: timers next to code which never queried the clock. Countdowns
    # stop at 0 (port_count_down): retail took one off a frame and several test for exactly 0,
    # which a two-field step would jump over (finding 139).
    # Allocation counts, loop cursors, item counts and RNG search indexes are
    # deliberately not included in this table.
    for unit, field in [('001100D0','obj->x3BC'),('001103B0','obj->x3BC'),
                         ('0012CF68','obj->x7bc8'),('00155208','p->xB4')]:
        edit(unit, field+'++;', field+'+=port_delta_clock() ? port_whole_step() : 1;')
    for unit, field in [('0012CF68','obj->x7bc0'),('00155380','obj->timer'),
                         ('00142508','obj->timer'),('001425E0','o->x64'),
                         ('00147BD8','obj->timer'),('0012AD60','obj->x180'),
                         ('0013DF28','obj->xCE0'),('00161B10','effect->x27F8'),
                         ('0011FF38','o->x25E')]:
        edit(unit, field+'--;', field+'=port_delta_clock() ? port_count_down('+field+') : '+field+'-1;')
    for unit,field in [('001207F8','obj->x262'),('0015B018','obj->x1E8'),
                        ('001517D0','obj->xA3C')]:
        edit(unit, '--'+field, '('+field+'=port_delta_clock() ? port_count_down('+field+') : '+field+'-1)')
    edit('00161B10', 'if (++effect->x276E == 0x79)',
         'if (port_delta_clock() ? port_count_reached(effect->x276E+=port_whole_step(),0x79) : ++effect->x276E == 0x79)')
    edit('00161B10', 'if (effect->x276E == 0xF0) {', 'if (port_delta_clock() ? port_count_reached(effect->x276E,0xF0) : effect->x276E == 0xF0) {')
    edit('00154058', 'obj->x2C++;', 'obj->x2C+=port_delta_clock() ? port_whole_step() : 1;')
    edit('001425E0', '(target-value)*0.125f', '(target-value)*port_blend(0.125f)')
    edit('00147BD8', 'obj->phase += obj->step;',
         'obj->phase += port_delta_clock() ? obj->step*port_step() : obj->step;')
    for sign in ('+','-'):
        edit('00147BD8', 'obj->amount'+sign+'0.01f',
             'obj->amount'+sign+'(port_delta_clock() ? 0.01f*port_step() : 0.01f)')
    # Keep integer lifetime storage and special asm ABIs; make all floating
    # motion/emission calculations fractional at the producer entry points.
    edit('00228BE8', 'pt.f[3] = count;', 'pt.f[3] = port_delta_clock() ? port_step() : (float)count;')
    edit('00228BE8', 'invCount = 1.0f / (float)count;',
         'invCount = 1.0f / (port_delta_clock() ? port_step() : (float)count);')
    edit('00228BE8', 'obj->x2768 += obj->x272C / (minSize + sizeRange * (baseAlpha + spread * 0.5f));',
         'obj->x2768 += obj->x272C * (port_delta_clock() ? port_step() : 1.0f) / (minSize + sizeRange * (baseAlpha + spread * 0.5f));')
    edit('00228BE8', 'left=*life-1;', 'left=*life-(port_delta_clock() ? port_whole_step() : 1);')
    # The specialized float-step particle path already receives f20 from the
    # generated dispatcher. Convert its frame damping to fractional decay.
    edit('00228BE8', 'values[3]=1.0f-obj->x2778;', 'values[3]=port_decay(1.0f-obj->x2778);')
    # SP5b (finding 090): the remaining per-update constants found for 165/240.
    # Rider-pair overlap (00134688): each call moves both riders apart by a tenth of the
    # overlap (00231A48 translates the body), a relaxation, not an impulse.
    edit('00134688', 'length * (push - 1.0f) * 0.1f', 'length * (push - 1.0f) * port_blend(0.1f)')
    # The jump wind sound's pitch/volume walk (001D1528): a slope per 60 Hz field
    # (x40/(x70*60)), added on every call of the rider's sound update (001C8380).
    for lane, slopes in (('x58', 's->x4C+s->x50'), ('x54', 's->x44+s->x48')):
        edit('001C5040', 'value=s->%s+(%s);' % (lane, slopes),
             'value=s->%s+(port_delta_clock() ? (%s)*port_step() : (%s));' % (lane, slopes, slopes))
    # Particle rise after half the burst's life (0022A168, called from 0022B708 once per update):
    # x2630.x134 is 0x2764, the life per field, which 0022B708 already advances by f20.
    edit('00228BE8', 'v = obj->x2630.x68 + obj->x2630.x134 * 60.0f;',
         'v = obj->x2630.x68 + obj->x2630.x134 * (port_delta_clock() ? 60.0f*port_step() : 60.0f);')
    # The human rider's trick button sequencer (001E6970: chord window x724 > 2, trick after
    # x720 > 3 quiet fields) and the HUD's code sequencer (001E6F58: x8 > 2, x4 > 20): field
    # counters, compared with field budgets. The samples stay per update.
    for unit, fields in (('001E6970', ('x724', 'x720')), ('001E6F58', ('x8', 'x4'))):
        for field in fields:
            patches.setdefault('unsorted/%s.c' % unit, []).append(('re', (r'obj->%s\+\+' % field, 2),
                'obj->%s+=port_delta_clock() ? port_whole_step() : 1' % field))
    # OPTIONS volume sliders (0015E750): one step per field while a direction is held.
    for button in ('0x20', '0x80'):
        edit('0015E750', '    if ((buttons & %s) || func_002209A8(' % button,
             '    if ((!port_delta_clock() || port_whole_step()) && ((buttons & %s) || func_002209A8(' % button)
        edit('0015E750', 'func_002209A8(%s, func_002205A0(D_003F52D0.x130))) {' % button,
             'func_002209A8(%s, func_002205A0(D_003F52D0.x130)))) {' % button)
    # HUD timers counted per draw: trick banner (x12BC, 120 fields) and score popup (x12C4,
    # 90/120 fields with its fades) in 00178730; pickup icon blink (x132C, 120 fields, 32-field
    # blink; whole steps never pass 0) and two 75-field fades in 00179A60, whose panels slide
    # in 20 steps (0017A1B0); the message popup (x12F0, 180 fields) and its 16-field blink
    # (x1308) in 0017B3C0; mode 10's 240-field banner cycle (x1324) and the item list's
    # 300-field timer (x1310) in 00176ED8, whose 60-field event (0017C638) fires once.
    patches.setdefault('unsorted/00178730.c', []).append(('re', (r'obj->x12BC\+\+', 2),
        'obj->x12BC+=port_delta_clock() ? port_whole_step() : 1'))
    edit('00178730', 'obj->x12C4++;', 'obj->x12C4+=port_delta_clock() ? port_whole_step() : 1;')
    edit('00179A60', '        obj->x132C--;',
         '        if (port_delta_clock()) { obj->x132C -= port_whole_step(); if (obj->x132C < 0) obj->x132C = 0; }\n'
         '        else obj->x132C--;')
    for field in ('x135D', 'x135E'):
        edit('00179A60', 'obj->%s += 2;' % field,
             'if (!port_delta_clock() || port_whole_step()) obj->%s += 2;' % field)
    entry('0017A1B0', 'func_0017A1B0', ' if (port_delta_clock() && !port_whole_step()) return;')
    edit('0017B3C0', 'obj->x12F0--;', 'obj->x12F0-=port_delta_clock() ? port_whole_step() : 1;')
    edit('0017B3C0', 'obj->x1308++;', 'obj->x1308+=port_delta_clock() ? port_whole_step() : 1;')
    edit('00176ED8', '--obj->x1324;', 'obj->x1324-=port_delta_clock() ? port_whole_step() : 1;')
    # x1310 counts down only while > 0 (retail's per-field call is inside that test), so a
    # two-field step from 1 rests at 0 as retail's two fields do, never at -1.
    edit('00176ED8', '--obj->x1310;',
         'if (port_delta_clock()) obj->x1310=port_count_down(obj->x1310); else --obj->x1310;')
    # 0017C638 runs before 00176ED8 takes this update's whole fields off x1310, so an update
    # covers the values x1310 down to x1310-whole+1: its marks (== 1 clears the item flag, == 60
    # starts x1318) are reached when one of those is the mark. x1318 then holds the last value
    # covered, as retail's copy of 60 counted down with x1310 for the fields after it.
    edit('0017C638', 'if (*(int *)(obj+0x1310)==1)',
         'if (port_delta_clock() ? port_count_reached(*(int *)(obj+0x1310),1) : *(int *)(obj+0x1310)==1)')
    edit('0017C638', 'if (state==60) {', 'if (port_delta_clock() ? port_count_reached(state,60) : state==60) {')
    edit('0017C638', '*(int *)(obj+0x1318)=state;',
         '*(int *)(obj+0x1318)=port_delta_clock() ? state-port_whole_step()+1 : state;')
    # NPC event rolls consume RNG only on whole fields. Preserve their original
    # probability and RNG draw frequency, rather than doubling ambient events.
    for path in root.glob('*.*'):
        text=path.read_text()
        pattern=r'func_00222BD8\(0,\s*100\)\s*==\s*42'
        count=len(re.findall(pattern,text))
        if count:
            patches.setdefault('unsorted/'+path.name, []).append(('re',(pattern,count),
                '( (!port_delta_clock() || port_whole_step()) && func_00222BD8(0,100)==42 )'))
