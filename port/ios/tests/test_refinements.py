"""Portable regression checks for TV output, FOV hotplug and short multi-touch taps.
UIKit/Metal/HDMI latency must also be checked on an iPhone built with Xcode.
"""
from pathlib import Path
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
CC = shutil.which('cc')

@unittest.skipUnless(CC, 'requires a C compiler')
class RefinementChecks(unittest.TestCase):
    def compile_run(self, code, *args):
        with tempfile.TemporaryDirectory() as td:
            source, executable = Path(td)/'check.c', Path(td)/'check'
            source.write_text(code)
            gc_flag = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
            subprocess.run([CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-ffunction-sections', '-fdata-sections', '-I'+str(ROOT/'port/hle'), str(source), *args,
                            gc_flag, '-lm', '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=20)

    def test_tv_fits_16_9_caps_output_and_uses_supported_cadence(self):
        self.compile_run(r'''
#include "display_math.h"
#include <assert.h>
int main(void) {
 double w,h;
 ios_display_limit(3840,2160,1080,true,&w,&h); assert(w==1920 && h==1080);
 ios_display_limit(3840,2160,0,true,&w,&h); assert(w==3840 && h==2160);
 ios_display_limit(2560,1080,720,true,&w,&h); assert(w==1280 && h==720);
 ios_display_limit(3440,1440,0,true,&w,&h); assert(w==2560 && h==1440);
 ios_display_limit(1280,720,1080,true,&w,&h); assert(w==1280 && h==720);
 ios_display_limit(1024,768,720,true,&w,&h); assert(w==1024 && h==576);
 ios_display_limit(2868,1320,720,false,&w,&h); assert(w==1564 && h==720);
 ios_display_limit(0,1080,720,true,&w,&h); assert(w==0 && h==0);
 ios_display_limit(NAN,1080,720,true,&w,&h); assert(w==0 && h==0);
 for(int x=320;x<8000;x+=79)for(int y=240;y<4400;y+=83) {
  ios_display_limit(x,y,720,true,&w,&h);
  assert(h<=720 && w<=1280 && w<=x && h<=y);
  assert(fabs(w/h-16.0/9)<(16.0/9+1)/h);
 }
 assert(ios_display_target_fps(120,60,true,false)==60);
 assert(ios_display_target_fps(60,120,true,false)==120);
 assert(ios_display_target_fps(120,120,true,true)==60);
 assert(ios_display_target_fps(60,120,false,false)==60);
 assert(ios_display_target_fps(120,120,false,false)==120);
 return 0;
}
''', '-I'+str(ROOT/'port/ios/render'))

    def test_touch_down_release_short_taps_and_simultaneous_controls(self):
        # Compile the actual input handlers and merge code, substituting only UIKit haptics
        # and the OS lock. Layout persistence (Objective-C JSON) isn't part of this fixture.
        source = (ROOT/'port/ios/input/touch_input.m').read_text()
        source = source[:source.index('const TouchConfig *touch_input_get_config(void)')]
        source = re.sub(r'^#import .*\n|^#include <os/lock.h>\n', '', source, flags=re.M)
        source = re.sub(r'^static UIImpactFeedbackGenerator .*\n', '', source, flags=re.M)
        a = source.index('static void trigger_haptic(')
        b = source.index('\nvoid touch_input_restore_defaults', a)
        source = source[:a] + 'static void trigger_haptic(bool heavy) { (void)heavy; }\n' + source[b:]
        gamepad = (ROOT/'port/hle/gamepad.c').read_text()
        pressure = gamepad[gamepad.index('static const uint16_t pressure_order'):gamepad.index('static uint8_t trigger_byte')]
        gamepad_input_ids = gamepad[gamepad.index('static int input_id('):gamepad.index('static int input_id(')+gamepad[gamepad.index('static int input_id('):].index('\n}')+3] + '\n'
        mapping = gamepad[gamepad.index('int gamepad_place('):gamepad.index('/* What its buttons say:')]
        # Exercise the iOS-specific analog filter without importing the Apple SDK on Linux.
        mapping = re.sub(r'#if defined\(__APPLE__\)\n#include <TargetConditionals.h>\n#endif\n', '', mapping)
        mapping = mapping.replace('defined(__APPLE__) && TARGET_OS_IPHONE', '1')
        keyboard = (ROOT/'port/hle/keyboard.c').read_text()
        decode = keyboard[keyboard.index('int keyboard_game_axis('):keyboard.index('/* The raw byte for a fraction')]
        raw_header = (ROOT/'port/hle/hle.h').read_text()
        raw_type = re.search(r'typedef struct \{[^{}]*\} GamepadRaw;', raw_header).group()
        code = r'''
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
typedef int os_unfair_lock;
#define OS_UNFAIR_LOCK_INIT 0
static void os_unfair_lock_lock(os_unfair_lock *p){(void)p;}
static void os_unfair_lock_unlock(os_unfair_lock *p){(void)p;}
static int applied_deadzone = -1, deadzone_writes, layout_writes;
static char pad1_value[512], pad2_value[512];
void host_putenv_s(const char *name, const char *value) {
 if (!strcmp(name,"DHD_PAD1") || !strcmp(name,"DHD_PAD2")) {
  snprintf(!strcmp(name,"DHD_PAD1") ? pad1_value : pad2_value,512,"%s",value); layout_writes++;
 } else { applied_deadzone = atoi(value); deadzone_writes++; }
}
''' + source + raw_type + '\nenum { PRESSED = 30 };\n' + '\n#include \"controls_table.h\"\n' + gamepad_input_ids + pressure + mapping + decode + r'''
#include <assert.h>
static HostPad poll(void) {
 HostPad p={0}; p.left_x=p.left_y=p.right_x=p.right_y=128; touch_pad_merge(&p); return p;
}
int main(void) {
 HostPad p;
 touch_input_restore_defaults(); touch_input_reset(); s_gamepad_connected=true;
 poll(); assert(applied_deadzone==5 && deadzone_writes==1);
 poll(); assert(deadzone_writes==1);
 touch_input_set_gamepad_deadzone(.20f); assert(applied_deadzone==5);
 poll(); assert(applied_deadzone==20 && deadzone_writes==2);
 touch_input_set_gamepad_deadzone(.20f); poll(); assert(deadzone_writes==2);
 touch_input_set_gamepad_deadzone(NAN); poll(); assert(applied_deadzone==5);
 /* The raw-coordinate fixed stick also applies the configured zone on touch-down. */
 s_config.stick_floating=false; s_config.stick_deadzone=.15f;
 touch_input_update_viewport(1000,1000,0,0,0,0);
 touch_input_begin(1,145,720); assert(poll().left_x==128);
 touch_input_end(1,145,720);
 touch_input_begin(2,160,720); p=poll(); assert(p.left_x>=178 && p.left_x<200);
 touch_input_cancel(2); assert(poll().left_x==128); s_config.stick_deadzone=.05f;
 /* Both native touch routes keep small off-axis motion neutral. */
 touch_input_begin(3,140,720);
 touch_input_move(3,198,738); p=poll(); assert(p.left_x>200 && p.left_y==128);
 touch_input_cancel(3); assert(poll().left_x==128 && poll().left_y==128);
 s_config.stick_floating=true;
 touch_input_begin(4,200,650);
 touch_input_move(4,142,632); p=poll(); assert(p.left_x<50 && p.left_y==128);
 touch_input_end(4,142,632); assert(poll().left_x==128 && poll().left_y==128);
 s_config.stick_floating=false;

 /* SwiftUI supplies normalized deltas here. Keep all deliberate diagonals. */
 for (int horizontal=-1; horizontal<=1; horizontal+=2) {
  for (int vertical=-1; vertical<=1; vertical+=2) {
   touch_input_set_stick(.90f*horizontal,.30f*vertical);
   p=poll(); assert((horizontal<0 ? p.left_x<50 : p.left_x>200) && p.left_y==128);
   float visual_x, visual_y;
   touch_input_get_stick(&visual_x,&visual_y);
   assert(visual_x*horizontal>.8f && visual_y==0);
   touch_input_set_stick(.30f*horizontal,.90f*vertical);
   p=poll(); assert(p.left_x==128 && (vertical<0 ? p.left_y<50 : p.left_y>200));
   touch_input_set_stick(.90f*horizontal,.90f*vertical);
   p=poll();
   assert(horizontal<0 ? p.left_x<50 : p.left_x>200);
   assert(vertical<0 ? p.left_y<50 : p.left_y>200);
  }
 }
 touch_input_set_stick(0,0); assert(poll().left_x==128 && poll().left_y==128);
 touch_input_set_button(TOUCH_CTRL_CROSS,true);
 p=poll(); assert(p.buttons==PAD_CROSS && p.pressure[6]==255);
 p=poll(); assert(p.buttons==PAD_CROSS);
 touch_input_set_button(TOUCH_CTRL_CROSS,false); assert(poll().buttons==0);
 touch_input_set_button(TOUCH_CTRL_CROSS,true); touch_input_set_button(TOUCH_CTRL_CROSS,false);
 assert(poll().buttons==PAD_CROSS); assert(poll().buttons==0);
 for(int i=0;i<3;i++) {
  touch_input_set_button(TOUCH_CTRL_CROSS,true); assert(poll().buttons==PAD_CROSS);
  touch_input_set_button(TOUCH_CTRL_CROSS,false); assert(poll().buttons==0);
 }
 touch_input_set_button(TOUCH_CTRL_L1,true); touch_input_set_button(TOUCH_CTRL_R2,true);
 touch_input_set_stick(1,-1); p=poll(); assert(p.buttons==(PAD_L1|PAD_R2) && p.left_x>200 && p.left_y<50);
 touch_input_reset(); p=poll(); assert(p.buttons==0 && p.left_x==128 && p.left_y==128);
 /* This range reproduced the bug before the fix: a light axis encoded as 174..177. */
 for (int direction=-1; direction<=1; direction+=2) {
  int previous_touch=0, previous_pad=0;
  for (int i=0; i<=32767; i++) {
   float deflection=direction*(i/32767.f);
   touch_input_set_stick(deflection,0); p=poll();
   int value=abs(keyboard_game_axis(p.left_x));
   int steering=value*value/127;
   assert(steering!=1 && steering>=previous_touch);
   assert(p.buttons==0 && p.left_y==128);
   if (i<1600) assert(steering==0);
   if (i==2400) assert(steering>0 && steering<10);
   previous_touch=steering;
   touch_input_set_stick(0,0);
   GamepadRaw raw={0}; raw.axis[0]=direction*i;
   memset(&p,0,sizeof p); p.left_x=p.left_y=p.right_x=p.right_y=128;
   gamepad_apply(&control_defaults[CONTROL_PAD1],&raw,applied_deadzone,&p);
   touch_pad_merge(&p);
   value=abs(keyboard_game_axis(p.left_x)); steering=value*value/127;
   assert(steering!=1 && steering>=previous_pad && p.buttons==0);
   if (i<1600) assert(steering==0);
   if (i==2400) assert(steering>0 && steering<10);
   previous_pad=steering;
  }
  assert(previous_touch==127 && previous_pad==127);
 }
 /* A neutral touch must leave the physical device's axes and digital d-pad intact. */
 touch_input_set_stick(0,0);
 p=(HostPad){.left_x=222,.left_y=128,.right_x=12,.right_y=244,.buttons=PAD_RIGHT};
 touch_pad_merge(&p); assert(p.left_x==222 && p.right_x==12 && p.right_y==244 && p.buttons==PAD_RIGHT);

 /* Cross-axial deadzone on physical controller: steer left with minor vertical thumb arc */
 GamepadRaw steer_left = {.axis = {-30000, -5000, 0, 0}};
 memset(&p, 0, sizeof p);
 gamepad_apply(&control_defaults[CONTROL_PAD1], &steer_left, applied_deadzone, &p);
 assert(p.left_x < 50 && p.left_y == 128);

 /* Steer right with minor vertical thumb arc downward */
 GamepadRaw steer_right = {.axis = {30000, 5000, 0, 0}};
 memset(&p, 0, sizeof p);
 gamepad_apply(&control_defaults[CONTROL_PAD1], &steer_right, applied_deadzone, &p);
 assert(p.left_x > 200 && p.left_y == 128);

 /* Intentional 45-degree diagonal trick must remain active on both axes */
 GamepadRaw diag = {.axis = {-25000, -25000, 0, 0}};
 memset(&p, 0, sizeof p);
 gamepad_apply(&control_defaults[CONTROL_PAD1], &diag, applied_deadzone, &p);
 assert(p.left_x < 50 && p.left_y < 50);

 touch_input_set_button((TouchControlID)-1,true); assert(poll().buttons==0);
 touch_input_set_button(TOUCH_CTRL_CROSS,true); touch_input_set_stick(1,0);
 touch_input_set_editing(true);
 p=(HostPad){.buttons=PAD_CROSS|PAD_RIGHT,.left_x=255,.left_y=0,.right_x=255,.right_y=0};
 touch_pad_merge(&p); assert(p.buttons==0 && p.left_x==128 && p.right_y==128);
 touch_input_set_button(TOUCH_CTRL_R1,true); touch_input_set_stick(1,1);
 touch_input_queue_controller_layout(1,"cross:east circle:south");
 touch_input_queue_controller_layout(1,"cross:north triangle:south");
 touch_input_queue_controller_layout(2,"cross:west square:south");
 assert(layout_writes==0);
 p=poll(); assert(p.buttons==0 && p.left_x==128 && layout_writes==2);
 assert(!strcmp(pad1_value,"cross:north triangle:south") && !strcmp(pad2_value,"cross:west square:south"));
 touch_input_reset(); poll(); assert(layout_writes==2);
 touch_input_set_editing(false); assert(poll().buttons==0 && poll().left_x==128);
 touch_input_set_button(TOUCH_CTRL_CROSS,true); assert(poll().buttons==PAD_CROSS);
 touch_input_reset();

 return 0;
}
'''
        # Unused screen-coordinate helper functions remain intentionally compiled.
        self.compile_run(code, '-Wno-unused-variable', '-I'+str(ROOT/'port/ios/input'))

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux mapped-memory fixture')
    def test_widescreen_hotplug_restores_4_3_without_drift(self):
        code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <fenv.h>
#include <sys/mman.h>
#include "widescreen.c"
uintptr_t guest_base=0;
unsigned hle_env_epoch;
WsEntry D_0075A9D0[WS_VIEW_COUNT];
unsigned char D_0029C470;
int D_0029DED8, D_0029E454 = 3;
static float current_y[WS_VIEW_COUNT];
static int rebuilt, combined;
static double output=16.0/9;
static int updated;
const char *host_getenv(const char *name){return getenv(name);}
void host_putenv_s(const char *name,const char *value){setenv(name,value,1);}
void host_dpi_aware(void){}
double host_monitor_aspect(void){return output;}
void hle_log(const char *fmt,...){(void)fmt;}
void host_service_graphics_wait(void){updated++;}
void func_0023D898(int i,float *x,float *y){*x=D_0075A9D0[i].view->x0;*y=current_y[i];}
void func_0023D8D8(int i,float x,float y){
 D_0075A9D0[i].view->x0=port_ws_menu_focal(x);
 current_y[i]=y;
 rebuilt++;
}
void func_0023CB00(int i){D_0029E454=i;combined++;}
int main(void) {
 void *memory=mmap((void*)0x08000000,0x02000000,PROT_READ|PROT_WRITE,
   MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(memory!=MAP_FAILED);
 uint32_t *x=(uint32_t*)(uintptr_t)(PS2_RAM_BASE+0x27ECC0);
 uint32_t *y=(uint32_t*)(uintptr_t)(PS2_RAM_BASE+0x27ECE0);
 *x=0x438A8000; *y=0x3F15551D;
 for (unsigned i=0;i<sizeof ws_sprite_widths/sizeof *ws_sprite_widths;i++) {
  uint32_t *at=(uint32_t*)(uintptr_t)(PS2_RAM_BASE+ws_sprite_widths[i].ee);
  at[0]=ws_sprite_widths[i].retail; at[1]=ws_sprite_widths[i].upper;
 }
 setenv("DHD_IOS_DISPLAY_AUTO","1",1);
 host_widescreen_init(); assert(fabs(host_widescreen_aspect()-16.0/9)<1e-6);
 assert(*x==float_bits(bits_float(0x438A8000)*.75f));
 WsView view={688*.75f,640,448};
 D_0075A9D0[0]=(WsEntry){&view,&view,5,{0}};
 D_0075A9D0[1]=(WsEntry){&view,&view,5,{0}};
 current_y[0]=current_y[1]=.5f/.75f;
 for(int i=0;i<1000;i++) {
  output=21.0/9;host_widescreen_vsync();assert(fabs(host_widescreen_aspect()-output)<1e-6);
  assert(fabs(view.x0-688*port_fov_x)<.02);
  assert(fabs(view.x0*current_y[0]-344)<.02 && fabs(view.x0*current_y[1]-344)<.02);
  output=4.0/3;host_widescreen_vsync();assert(host_widescreen_aspect()==0);
  assert(*x==0x438A8000 && *y==0x3F15551D && fabs(view.x0-688)<.02);
  output=16.0/9;host_widescreen_vsync();assert(fabs(view.x0-516)<.02);
 }
 assert(updated==3000);host_widescreen_vsync();assert(updated==3000);
 output=NAN;host_widescreen_vsync();assert(updated==3000);
 assert(rebuilt==6000 && combined==6000 && D_0029E454==3);
 output=21.0/9;D_0029DED8=18;D_0029C470=1;port_ws_inventory_enter();
 assert(fabs(host_widescreen_frame_aspect()-16.0/9)<1e-6);
 /* The shell can still be in the shop transition when the modal is closed. */
 D_0029C470=0;host_widescreen_vsync();
 assert(fabs(host_widescreen_frame_aspect()-21.0/9)<1e-6);
 assert(fabs(view.x0-688*port_fov_x)<.02);
 D_0029DED8=9;host_widescreen_vsync();
 assert(fabs(host_widescreen_frame_aspect()-21.0/9)<1e-6);
 for (unsigned i=0;i<sizeof ws_sprite_widths/sizeof *ws_sprite_widths;i++) {
  const uint32_t *at=(uint32_t*)(uintptr_t)(PS2_RAM_BASE+ws_sprite_widths[i].ee);
  assert(at[0]==float_bits(277*port_fov_x) && at[1]==ws_sprite_widths[i].upper);
 }
 output=-1;host_widescreen_vsync();
 assert(host_widescreen_frame_aspect()==-1 && fabs(view.x0-688)<.02);
 output=4.0/3;host_widescreen_vsync();
 assert(host_widescreen_frame_aspect()==0 && fabs(view.x0-688)<.02);
 /* Boot in IEEE round-to-nearest (1260x709) then switch on the EE fiber (round-toward-zero, 2346x1080). */
 fesetround(FE_TONEAREST);
 output=1260.0/709.0;host_widescreen_init();
 WsView *race_view=(WsView*)(uintptr_t)(PS2_RAM_BASE+0x27ECC0);
 race_view->width=512; race_view->height=448;
 D_0075A9D0[0]=(WsEntry){race_view,race_view,5,{0}};
 D_0075A9D0[1]=(WsEntry){0};
 current_y[0]=bits_float(*y);
 fesetround(FE_TOWARDZERO);
 output=2346.0/1080.0;host_widescreen_vsync();
 assert(*y==float_bits(bits_float(0x3F15551D)/port_fov_x));
 assert(fabs(race_view->x0*current_y[0]-277.0f*bits_float(0x3F15551D))<.02);
 output=1260.0/709.0;host_widescreen_vsync();
 assert(*y==float_bits(bits_float(0x3F15551D)/port_fov_x));
 output=2346.0/1080.0;host_widescreen_vsync();
 assert(*x==float_bits(277.0f*port_fov_x) && *y==float_bits(bits_float(0x3F15551D)/port_fov_x));
 assert(fabs(race_view->x0*current_y[0]-277.0f*bits_float(0x3F15551D))<.02);
 fesetround(FE_TONEAREST);
 return 0;
}
'''
        self.compile_run(code, '-I'+str(ROOT/'port/hle'), '-I'+str(ROOT/'port/runtime'),
                         '-I'+str(ROOT/'port/include'))

    def test_ios_race_fields_do_not_inherit_loading_card(self):
        source = (ROOT/'port/gs/native_core.inc').read_text()
        start = source.index('static void native_note_flat(')
        end = source.index('/* After a present:', start)
        fixture = r'''
#define DHD_IOS_RENDER 1
#define NATIVE_FLAT_FIELDS 8
#include <assert.h>
#include <stdio.h>
typedef struct { int wide, flat_fields, card_shown; } Native;
typedef struct { unsigned path1, flat_limit; int card_2d; unsigned long long field; } NativeFrame;
static int native_card_now;
'''
        self.compile_run(fixture + source[start:end] + r'''
int main(void) {
 Native n={1,0,0};
 NativeFrame f={150,1000,1,1};
 for (int i=0; i<8; i++) native_note_flat(&n,&f);
 assert(native_card_now && n.card_shown);
 f.path1=0;
 f.flat_limit=0;
 for (int i=0; i<12; i++) {
  native_note_flat(&n,&f);
  assert(!native_card_now && !n.card_shown);
 }
 return 0;
}
''')

    def test_existing_cached_wasm_is_rebuilt(self):
        spec=importlib.util.spec_from_file_location('ios_refine_build',ROOT/'port/ios/build_ios.py')
        build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            for name in ('assets/vutext.bin','build/wasm/dhd_core.wasm','build/host64-ios/libdhd_ios.a'):
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'fixture')
            with patch.object(build,'ROOT',root),patch.object(build,'setup_env'),patch.object(build.subprocess,'run') as run:
                build.prepare_native_core()
            self.assertEqual(len(run.call_args_list),2)
            self.assertTrue(run.call_args_list[0].args[0][1].endswith('build_wasm.py'))

if __name__=='__main__': unittest.main()
