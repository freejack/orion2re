#pragma once

#include <cstdint>

// LOOK TEST (local patch, not upstream): a camera over the tactical battlefield.
//
// The combat code keeps drawing its flat, top-down field exactly as before. Just before a finished frame is shown,
// this module treats the battlefield part of that frame as a floor, projects it through an orbiting camera, and
// stands the splat ships on it in 3D. Mouse positions over the battlefield are carried back through the camera
// before the game sees them, so every click and hover still lands on the flat field the game knows about.
namespace battle_cam {
    constexpr int32_t kFieldWidth = 640;
    constexpr int32_t kFieldHeight = 351; // the battlefield is rows 0 to 350 of the screen
    constexpr float kDragThreshold = 4.0f; // pixels the mouse must move with the right button down to count as a drag

    // Held for the length of a tactical battle; the camera only acts while one exists.
    struct Scope {
        Scope();
        ~Scope();
    };
    bool Active();

    // Held while the game shows a window over the battlefield (the scan result, the boarding prompt). The window is
    // part of the screen, not of the field: while one exists the battlefield stays as it last was behind it, the
    // window is shown flat in front, and the mouse is not carried through the camera.
    struct Popup_Scope {
        Popup_Scope();
        ~Popup_Scope();
    };
    bool Is_Default_View();

    // A full redraw of the battlefield is starting: forget the ships of the previous frame.
    void Begin_Field_Frame();
    // The combat code would draw this splat ship here. Returns true when the camera takes it over, in which case
    // the caller must not draw the flat sprite. centre_x/centre_y are flat field pixels.
    bool Add_Ship(int16_t ship_idx, int16_t centre_x, int16_t centre_y, int16_t engine_step, float heading, float roll);
    // Game thread. Slide, and zoom out if that is what it takes, so that two points of the whole battlefield (in
    // pixels) are both comfortably in view. The view stays framed until End_Framing; following a ship then resumes
    // and the zoom goes back to what the player last chose.
    void Frame_Points(float ax, float ay, float bx, float by);
    void End_Framing();
    // True once the shown view has caught up with where it was sent.
    bool Settled();

    // The game is drawing a beam between these two points of its own window (which may lie outside it). It draws
    // only the part inside; the camera draws the rest, in the same colours.
    void Add_Beam(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t palette_base, int16_t palette_span, bool wide);

    // The movement highlight is on or off; when on, the active ship covers a square `box_pixels` across and the
    // highlight is that palette colour. The camera lays it on the field itself, for the whole battlefield.
    void Set_Move_Highlight(bool shown, int32_t box_pixels, uint8_t color);
    // The square of the field, in the game window's pixels, that the active ship would take up at the mouse.
    void Set_Floor_Cursor(int32_t window_x, int32_t window_y, int32_t size_pixels);
    // Called with the frame about to be shown (8-bit, kFieldWidth wide).
    void Compose(uint8_t* pixels, int32_t stride);

    // Screen to flat field and back, in pixels.
    void Screen_To_Field(float screen_x, float screen_y, float* field_x, float* field_y);
    // True while positions on the battlefield and positions on screen differ.
    bool Remaps_Pointer(int16_t field_y);
    // Where a flat field position appears on screen.
    void Field_To_Screen(int16_t* x, int16_t* y);
    // For the mouse: carries a screen position over the battlefield to the field position under it.
    void Map_Pointer(int16_t* x, int16_t* y);

    // Game thread, from the combat input loop: true when the camera has been moved far enough that the game's own
    // window should follow, with the cell that window should be centred on.
    bool Wanted_Window(int16_t* centre_cell_x, int16_t* centre_cell_y);

    // Input, from the window's event loop. Screen positions are game pixels.
    void Orbit(float delta_x, float delta_y);
    void Zoom(float wheel_steps, float screen_x, float screen_y);
    void Pan(float delta_x, float delta_y);
    void Set_Pan_Keys(int32_t right_minus_left, int32_t down_minus_up);
    // The minimap: whether a screen position is on it, and sending the camera to the place it stands for.
    bool On_Minimap(float screen_x, float screen_y);
    void Centre_From_Minimap(float screen_x, float screen_y);
    void Reset();
} // namespace battle_cam
