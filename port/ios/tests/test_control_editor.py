"""Controller bridge regression checks. The shared Rust parser is mocked here.
SwiftUI, drag gestures and persistence need Xcode/device validation.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CC = shutil.which('cc')


@unittest.skipUnless(CC, 'requires a C compiler')
class ControllerEditorChecks(unittest.TestCase):
    def test_bridge_preserves_slots_flags_and_applies_only_saved_changes(self):
        code = r'''
#include "ios_main.h"
#include "dhd_settings.h"
#include <assert.h>
#include <string.h>
static int parser_result=1, can_save=1, saves, queued, queued_player;
static int seen_player,seen_edit,seen_target,seen_slot,seen_input;
static char saved_key[16],queued_value[512];
int ios_setting_read(const char *key,char *out,int size) {
 (void)key;(void)size;out[0]=0;return 1;
}
int ios_setting_save(const char *key,const char *value) {
 (void)value;saves++;strcpy(saved_key,key);return can_save;
}
void touch_input_queue_controller_layout(int player,const char *value) {
 queued++;queued_player=player;strcpy(queued_value,value);
}
void dhd_controls_layouts(const char *k,const char *a,const char *b,DhdControlsLayout out[3],DhdSettingsUnknown problem) {
 (void)k;(void)a;(void)b;(void)problem;memset(out,0,sizeof(*out)*3);
 for(int p=1;p<=2;p++)for(int t=0;t<16;t++)for(int s=0;s<2;s++)out[p].input[t][s]=(unsigned char)(p+t+s);
 out[1].swap_sticks=1;out[2].invert=9;
}
int dhd_controls_edit(const char *k,const char *a,const char *b,int player,int edit,int target,int slot,int input,
 int kind,char *value,size_t size,char *message,size_t message_size) {
 (void)k;(void)a;(void)b;(void)kind;(void)size;(void)message_size;
 seen_player=player;seen_edit=edit;seen_target=target;seen_slot=slot;seen_input=input;
 strcpy(value,"cross:east circle:south");
 if(message)strcpy(message,parser_result<0 ? "Essential input required" : "");
 return parser_result;
}
int main(void) {
 unsigned char layout[34];char error[256];
 assert(!ios_controller_layout(0,layout,34));assert(!ios_controller_layout(1,layout,33));
 assert(ios_controller_layout(1,layout,34));
 assert(layout[0]==1 && layout[31]==17 && layout[32]==1 && layout[33]==0);
 assert(ios_controller_layout(2,layout,34));assert(layout[0]==2 && layout[33]==9);
 assert(ios_controller_edit(2,0,3,1,22,error,sizeof error)==1);
 assert(seen_player==2 && seen_edit==0 && seen_target==3 && seen_slot==1 && seen_input==22);
 assert(saves==1 && queued==1 && queued_player==2 && !strcmp(saved_key,"pad2"));
 assert(!strcmp(queued_value,"cross:east circle:south"));
 parser_result=-1;assert(ios_controller_edit(1,1,0,0,0,error,sizeof error)==-1);
 assert(saves==1 && queued==1 && !strcmp(error,"Essential input required"));
 parser_result=0;assert(ios_controller_edit(1,2,0,0,0,error,sizeof error)==0);
 assert(saves==1 && queued==1);
 parser_result=1;can_save=0;assert(ios_controller_edit(1,3,0,0,1,error,sizeof error)==-1);
 assert(saves==2 && queued==1 && error[0]);
 can_save=1;assert(ios_controller_edit(1,4,8,0,1,error,sizeof error)==1);
 assert(seen_edit==4 && seen_target==8 && !strcmp(saved_key,"pad1") && queued==2);
 assert(ios_controller_edit(3,0,0,0,1,error,sizeof error)==-1 && queued==2);
 assert(ios_controller_input_count()==22 && ios_controller_input_id(21)==22);
 assert(!strcmp(ios_controller_input_label(0,true),"Cross"));
 assert(!strcmp(ios_controller_input_label(0,false),"A"));
 assert(!strcmp(ios_controller_target_label(0),"Cross"));
 assert(!strcmp(ios_controller_target_label(20),""));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source, binary = Path(directory) / 'bridge.c', Path(directory) / 'bridge'
            source.write_text(code)
            subprocess.run([CC, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'port/ios/host'), '-I' + str(ROOT / 'port/ios/input'),
                            '-I' + str(ROOT / 'port/render/include'), '-I' + str(ROOT / 'port/hle'),
                            str(source), str(ROOT / 'port/ios/host/controller_settings.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
