#ifndef BRAMBLE_UI_CONFIRM_H
#define BRAMBLE_UI_CONFIRM_H

typedef void (*ui_confirm_cb_t)(void* user_data);

/* Modal yes/no. Confirm button carries confirm_label and danger color;
 * Cancel (focused by default) just closes. on_confirm runs after the
 * modal closes. One modal at a time; a second call replaces the first. */
void ui_confirm_show(const char* text, const char* confirm_label, ui_confirm_cb_t on_confirm,
                     void* user_data);

#endif
