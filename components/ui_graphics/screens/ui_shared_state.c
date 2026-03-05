#include "ui_shared_state.h"

extern void mesh_get_state(ui_mesh_state_t* out);
extern void mesh_get_location_state(location_manager_t* out);

const ui_mesh_state_t* ui_shared_mesh_state(void) {
    static ui_mesh_state_t s_state;
    mesh_get_state(&s_state);
    return &s_state;
}

const location_manager_t* ui_shared_location_state(void) {
    static location_manager_t s_state;
    mesh_get_location_state(&s_state);
    return &s_state;
}
