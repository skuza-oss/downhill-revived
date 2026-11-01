/* controls.c (finding 141): the defaults are the shipped layout, the bindings text round-trips, a
   rebind and an unbind are read back, the catalogs map names to keys and SDL sources both ways, an
   invalid text is skipped, and a profile's dhd.ini key is named the way host.c looks it up.
   Built and run by port/tools/test_controls.py. */
#include "controls.h"
#include "hle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int key(const char *id) { return control_key_index(id); }
static int pad(const char *id) { return control_pad_index(id); }

int main(void)
{
    int i;
    char text[CONTROL_TEXT];
    controls_default();

    /* The tables: the shipped keyboard layout and pad layout. */
    assert(control_key_count() == 19);
    assert(control_pad_count() == 20);
    assert(key("menu_left") == 0 && key("powerslide") == 18 && key("nope") == -1);
    assert(pad("south") == 0 && pad("right_y") == 19 && pad("nope") == -1);
    assert(control_key_vk(key("accelerate")) == 0x20);   /* Space */
    assert(control_key_vk(key("accelerate_x")) == 'X');
    assert(control_key_vk(key("menu_left")) == 0x25 && control_key_axis(key("menu_left")) == CONTROL_AXIS_LEFT_X &&
           control_key_dir(key("menu_left")) == -1);
    assert(control_key_menu(key("pause")) == PAD_TRIANGLE && control_key_riding(key("pause")) == PAD_START);
    assert(control_pad_button(pad("south")) == PAD_CROSS && control_pad_axis(pad("south")) == 0);
    assert(control_pad_source(pad("south")) == PADSRC_SOUTH);
    assert(control_pad_button(pad("left_x")) == 0 && control_pad_axis(pad("left_x")) == CONTROL_AXIS_LEFT_X);
    assert(control_pad_source(pad("left_x")) == PADSRC_LEFT_X);

    /* The catalogs: every name maps to its key or source and back. */
    for (i = 0; i < control_key_catalog_count(); i++) {
        unsigned vk = control_key_catalog_vk(i);
        assert(vk && control_key_catalog_index(vk) == i);
        assert(control_key_vk_of_name(control_key_catalog_name(i)) == vk);
        assert(!strcmp(control_key_name_of_vk(vk), control_key_catalog_name(i)));
    }
    assert(control_key_vk_of_name("space") == 0x20 && control_key_vk_of_name("f7") == 0);
    for (i = 0; i < control_pad_catalog_count(); i++) {
        unsigned source = control_pad_catalog_source(i);
        assert(source && control_pad_catalog_index(source) == i);
        assert(control_pad_source_of_name(control_pad_catalog_name(i)) == source);
        assert(!strcmp(control_pad_source_name(source), control_pad_catalog_name(i)));
    }
    assert(control_pad_source_of_name("nope") == 0 && control_pad_source_name(0)[0] == 0);

    /* A stick control takes an axis, a button control a button or trigger; unbound is always fine. */
    assert(control_pad_accepts(pad("south"), PADSRC_SOUTH) && control_pad_accepts(pad("south"), PADSRC_LEFT_TRIGGER));
    assert(!control_pad_accepts(pad("south"), PADSRC_LEFT_X));
    assert(control_pad_accepts(pad("left_x"), PADSRC_RIGHT_X) && !control_pad_accepts(pad("left_x"), PADSRC_NORTH));
    assert(control_pad_accepts(pad("south"), 0) && control_pad_accepts(pad("left_x"), 0));

    /* The defaults format, and parse back to themselves. */
    assert(!controls_customised());
    assert(controls_format(text, sizeof text) > 0);
    assert(strstr(text, "key.accelerate=space") && strstr(text, "key.pause=escape") &&
           strstr(text, "pad.south=south") && strstr(text, "pad.left_x=left_x"));
    assert(text[0] == 'k' && !strchr(text, ' ') && !strchr(text, ';'));
    controls_parse(text);
    assert(!controls_customised());

    /* A rebind and an unbind are read back by a fresh parse. */
    control_key_set(key("accelerate"), control_key_vk_of_name("j"));
    control_pad_set(pad("south"), PADSRC_WEST);
    control_key_set(key("punch"), 0);                      /* unbound */
    assert(controls_customised());
    assert(controls_format(text, sizeof text) > 0);
    assert(strstr(text, "key.accelerate=j") && strstr(text, "key.punch=") && strstr(text, "pad.south=west"));
    controls_default();
    assert(!controls_customised());
    assert(controls_parse(text) > 0);
    assert(control_key_vk(key("accelerate")) == control_key_vk_of_name("j"));
    assert(control_pad_source(pad("south")) == PADSRC_WEST);
    assert(control_key_vk(key("punch")) == 0);
    assert(controls_customised());

    /* An invalid text is skipped, never fatal: unknown ids, keys and sources, and a bad axis. */
    controls_default();
    assert(controls_parse("key.nope=x,key.accelerate=notakey,pad.nope=north,pad.south=left_x,garbage,=x") == 0);
    assert(!controls_customised());
    /* Spaces, semicolons and the DHD_ spelling are accepted; a stick may take an axis, not a button. */
    controls_default();
    assert(controls_parse(" key.accelerate=X ; pad.left_x = right_x ,\nkey.brake = up\n") == 3);
    assert(control_key_vk(key("accelerate")) == 'X');
    assert(control_pad_source(pad("left_x")) == PADSRC_RIGHT_X);
    assert(control_key_vk(key("brake")) == 0x26);          /* up */
    assert(controls_parse("pad.south=left_x") == 0);       /* a button control can't take an axis */

    /* A profile's dhd.ini key. */
    control_profile_key("default", text, sizeof text);
    assert(!strcmp(text, "bindings"));
    control_profile_key("", text, sizeof text);
    assert(!strcmp(text, "bindings"));
    control_profile_key("fast", text, sizeof text);
    assert(!strcmp(text, "bindings_fast"));
    control_profile_key("a b!c-d", text, sizeof text);
    assert(!strcmp(text, "bindings_abc-d"));
    control_profile_key("!!", text, sizeof text);
    assert(!strcmp(text, "bindings"));

    controls_default();
    puts("test_controls: ok (defaults, catalogs, round trip, rebinds, profile keys)");
    return 0;
}
