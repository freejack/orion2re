// LOOK TEST (local patch, not upstream): see battle_cam.h.
#include "pch.h"

#include <atomic>

#include "game/battle_cam.h"
#include "game/splat_ship.h"

namespace battle_cam {
    namespace {
        constexpr float kPi = 3.14159265358979f;
        constexpr float kMaxTilt = 84.0f * kPi / 180.0f; // from straight down; 90 would be edge-on, where the field has no area left to point at
        constexpr float kMinZoom = 0.36f, kMaxZoom = 4.0f; // 0.36 fits the whole 81 by 68 cell field on screen
        constexpr float kCentreX = kFieldWidth * 0.5f, kCentreY = kFieldHeight * 0.5f; // the middle of the battlefield on screen
        constexpr float kEaseSeconds = 0.09f;  // how quickly the view catches up with where it was told to go
        constexpr float kPanSpeed = 520.0f;    // screen pixels a second while a pan key is held
        constexpr float kIconZoom = 0.7f;      // below this zoom, ships get a marker so they can still be told apart

        // The camera has a view it is heading for (set from the window's event thread) and the view it is showing
        // (eased toward the first by the game thread, once a frame). Angles are radians: yaw about the vertical,
        // tilt from straight down. The centre is the point of the whole battlefield, in pixels, that sits in the
        // middle of the screen; the game's own window covers only part of that field.
        struct s_pose {
            std::atomic<float> yaw{0.0f}, tilt{0.0f}, zoom{1.0f}, centre_x{0.0f}, centre_y{0.0f};
        };
        s_pose g_target, g_shown;
        std::atomic<int32_t> g_active{0};
        std::atomic<int32_t> g_popups{0};          // windows the game is showing over the battlefield
        std::atomic<int32_t> g_centre_known{0};
        // Where the game's window starts on the whole field, and how big that field is, in pixels (game thread writes).
        std::atomic<float> g_window_x{0.0f}, g_window_y{0.0f};
        std::atomic<float> g_field_width{81.0f * 20.0f}, g_field_height{68.0f * 20.0f};
        // Where the mouse is on the flat field, for the floor cursor; -1 while it is off the battlefield.
        std::atomic<int32_t> g_pointer_field_x{-1};
        std::atomic<int32_t> g_pointer_field_y{-1};
        // Pan keys held: right minus left, and down minus up.
        std::atomic<int32_t> g_pan_key_x{0}, g_pan_key_y{0};
        // Whether the player has slid the camera away since the turn passed to their current ship. Until they do,
        // the camera keeps following that ship as it moves.
        std::atomic<int32_t> g_player_moved_camera{0};
        // Whether the camera is currently keeping a ship of the player's in the middle of the view.
        std::atomic<int32_t> g_following_player{0};

        struct s_settings {
            float turn_x = 1.0f, turn_y = 1.0f, zoom = 1.0f; // multipliers; a negative one inverts that direction
        };
        s_settings g_settings;

        // A view to draw or measure with. px, py is the screen-centre point in the game window's own pixels.
        struct s_view {
            float yaw, tilt, zoom, cy, sy, ct, st, px, py;
        };
        s_view View_Of_(const s_pose& pose) {
            s_view v;
            v.yaw = pose.yaw.load();
            v.tilt = pose.tilt.load();
            v.zoom = pose.zoom.load();
            v.cy = cosf(v.yaw);
            v.sy = sinf(v.yaw);
            v.ct = cosf(v.tilt);
            v.st = sinf(v.tilt);
            v.px = pose.centre_x.load() - g_window_x.load();
            v.py = pose.centre_y.load() - g_window_y.load();
            return v;
        }
        s_view View_() {
            return View_Of_(g_shown);
        }
        // Window pixel (u, w) at height y above the field: X to the right, Y up, Z toward the bottom of the flat
        // field. The camera turns that about the vertical, tips it, and scales it.
        inline void Project_(const s_view& v, float u, float y, float w, float* sx, float* sy, float* depth) {
            const float x = u - v.px, z = w - v.py;
            const float xr = x * v.cy - z * v.sy;
            const float zr = x * v.sy + z * v.cy;
            *sx = kCentreX + v.zoom * xr;
            *sy = kCentreY + v.zoom * (zr * v.ct - y * v.st);
            *depth = y * v.ct + zr * v.st;
        }
        // How far across the field a step of (dx, dy) screen pixels reaches.
        inline void Field_Step_(const s_view& v, float dx, float dy, float* du, float* dw) {
            const float xr = dx / v.zoom;
            const float zr = dy / (v.zoom * v.ct);
            *du = xr * v.cy + zr * v.sy;
            *dw = -xr * v.sy + zr * v.cy;
        }
        inline void Unproject_(const s_view& v, float sx, float sy, float* u, float* w) {
            float du, dw;
            Field_Step_(v, sx - kCentreX, sy - kCentreY, &du, &dw);
            *u = v.px + du;
            *w = v.py + dw;
        }
        void Clamp_Target_Centre_() {
            g_target.centre_x = std::clamp(g_target.centre_x.load(), 0.0f, g_field_width.load());
            g_target.centre_y = std::clamp(g_target.centre_y.load(), 0.0f, g_field_height.load());
        }

        struct s_ship {
            int16_t ship_idx, centre_x, centre_y, engine_step;
            float heading, roll;
        };
        std::vector<s_ship> g_ships;      // game thread only
        std::vector<uint8_t> g_field;     // the flat battlefield of the frame being composed
        std::vector<uint8_t> g_space;     // the empty background behind it
        std::vector<uint8_t> g_ship_pixels;
        int32_t g_frames_composed = 0;
        int16_t g_followed_ship = -1;
        int16_t g_turn_ship = -1;          // the ship whose turn the camera last reacted to
        struct s_beam {
            float x1, y1, x2, y2; // in the game window's pixels
            int32_t palette_base, palette_span;
            bool wide;
        };
        std::vector<s_beam> g_beams;       // the beams of the frame being drawn (game thread)
        bool g_framing = false;            // holding a shot in view; ship following is paused
        float g_shot_ax = 0.0f, g_shot_ay = 0.0f, g_shot_bx = 0.0f, g_shot_by = 0.0f; // the shot being framed, whole-field pixels
        std::atomic<float> g_player_zoom{1.0f}; // the zoom the player last chose, to go back to after a framed shot
        std::vector<uint8_t> g_last_flat;  // the battlefield as the game last drew it with no window over it
        std::vector<uint8_t> g_popup;      // the frame as the game drew it while a window is up
        // The movement highlight and the floor cursor, as the game last described them (game thread only).
        int32_t g_highlight_cells = 0;     // the highlighted box is this many cells square; 0 while it is off
        uint8_t g_highlight_color = 0;
        std::vector<uint8_t> g_reachable;  // one entry per cell of the whole field
        int32_t g_cursor_x = 0, g_cursor_y = 0, g_cursor_size = 0; // in the game window's pixels; size 0 is no cursor
        float g_start_offset_x = 0.0f, g_start_offset_y = 0.0f; // test aid: where the centre starts, off the window's middle
        Uint64 g_last_update = 0;          // game thread
        int32_t g_last_window_cell_x = -1, g_last_window_cell_y = -1, g_asked_cell_x = -1, g_asked_cell_y = -1;
    } // namespace

    namespace {
        // The game's starfield is three tiled 640 by 480 layers that slide at different rates as the view scrolls.
        // This rebuilds the same picture with nothing drawn on it.
        struct s_star_layer {
            const s_animation_header* picture = nullptr;
            std::vector<uint8_t> bitmap; // header, then 640 * 480 palette indices
        };
        s_star_layer g_layers[3];
        int32_t g_space_key[6] = {-1, -1, -1, -1, -1, -1};

        const uint8_t* Layer_Pixels_(s_star_layer* layer, s_animation_header* picture) {
            if (picture == nullptr || picture->width != 640 || picture->height != 480) {
                return nullptr;
            }
            if (layer->picture != picture || layer->bitmap.empty()) {
                layer->picture = picture;
                layer->bitmap.assign(640 * 480 + 64, 0);
                bitmap::Draw_Picture_To_Bitmap((s_animation_header*)layer->bitmap.data(), picture);
                g_space_key[0] = -1; // force the starfield to be rebuilt
            }
            return layer->bitmap.data() + sizeof(s_animation_header) + sizeof(uint32_t);
        }

        void Build_Space_(const uint8_t* frame, int32_t stride) {
            const uint8_t* far_px = Layer_Pixels_(&g_layers[0], MOX::combat_far_star);
            const uint8_t* mid_px = Layer_Pixels_(&g_layers[1], MOX::combat_mid_star);
            const uint8_t* near_px = Layer_Pixels_(&g_layers[2], MOX::combat_near_star);
            g_space.resize((size_t)kFieldWidth * kFieldHeight);
            if (far_px == nullptr || mid_px == nullptr || near_px == nullptr) {
                // the layers are not loaded: fall back to treating the whole frame as backdrop
                for (int32_t y = 0; y < kFieldHeight; ++y) {
                    memcpy(&g_space[(size_t)y * kFieldWidth], frame + (size_t)y * stride, kFieldWidth);
                }
                return;
            }
            const int32_t key[6] = {MOX::combat_far_x, MOX::combat_far_y, MOX::combat_mid_x, MOX::combat_mid_y, MOX::combat_near_x, MOX::combat_near_y};
            if (memcmp(key, g_space_key, sizeof(key)) == 0) {
                return;
            }
            memcpy(g_space_key, key, sizeof(key));
            const auto wrap = [](int32_t value, int32_t size) { return ((value % size) + size) % size; };
            for (int32_t y = 0; y < kFieldHeight; ++y) {
                const int32_t fy = wrap(y - key[1], 480) * 640, my = wrap(y - key[3], 480) * 640, ny = wrap(y - key[5], 480) * 640;
                uint8_t* row = &g_space[(size_t)y * kFieldWidth];
                for (int32_t x = 0; x < kFieldWidth; ++x) {
                    uint8_t pixel = near_px[ny + wrap(x - key[4], 640)];
                    if (pixel == 0) {
                        pixel = mid_px[my + wrap(x - key[2], 640)];
                    }
                    if (pixel == 0) {
                        pixel = far_px[fy + wrap(x - key[0], 640)];
                    }
                    row[x] = pixel;
                }
            }
        }
    } // namespace

    Scope::Scope() {
        g_ships.clear();
        g_space_key[0] = -1;
        g_frames_composed = 0;
        g_last_update = 0;
        g_last_window_cell_x = g_last_window_cell_y = g_asked_cell_x = g_asked_cell_y = -1;
        g_centre_known = 0;
        g_turn_ship = -1;
        g_player_moved_camera = 0;
        g_following_player = 0;
        g_framing = false;
        g_player_zoom = 1.0f;
        g_pan_key_x = 0;
        g_pan_key_y = 0;
        for (s_pose* pose : {&g_target, &g_shown}) {
            pose->yaw = 0.0f;
            pose->tilt = 0.0f;
            pose->zoom = 1.0f;
        }
        // Settings: a negative speed inverts that direction.
        const auto number = [](const char* name, float fallback) {
            const char* text = SDL_getenv(name);
            return text != nullptr && text[0] != '\0' ? (float)SDL_atof(text) : fallback;
        };
        g_settings.turn_x = number("ORION2RE_CAM_TURN_SPEED", 1.0f) * (number("ORION2RE_CAM_INVERT_X", 0.0f) != 0.0f ? -1.0f : 1.0f);
        g_settings.turn_y = number("ORION2RE_CAM_TURN_SPEED", 1.0f) * (number("ORION2RE_CAM_INVERT_Y", 0.0f) != 0.0f ? -1.0f : 1.0f);
        g_settings.zoom = number("ORION2RE_CAM_ZOOM_SPEED", 1.0f);
        g_start_offset_x = g_start_offset_y = 0.0f;
        if (const char* start = SDL_getenv("ORION2RE_LOOK_TEST_CAMERA")) {
            // "yaw,tilt,zoom[,pointer_x,pointer_y[,offset_x,offset_y]]", angles in degrees: lets the staged battle
            // start from a chosen view, with the floor cursor placed and the centre moved off the game's window
            float yaw = 0.0f, tilt = 0.0f, zoom = 1.0f, offset_x = 0.0f, offset_y = 0.0f;
            int pointer_x = -1, pointer_y = -1;
            const int fields = sscanf(start, "%f,%f,%f,%d,%d,%f,%f", &yaw, &tilt, &zoom, &pointer_x, &pointer_y, &offset_x, &offset_y);
            if (fields >= 5) {
                g_pointer_field_x = pointer_x;
                g_pointer_field_y = pointer_y;
            }
            if (fields >= 7) {
                g_start_offset_x = offset_x;
                g_start_offset_y = offset_y;
            }
            if (fields >= 2) {
                for (s_pose* pose : {&g_target, &g_shown}) {
                    pose->yaw = yaw * kPi / 180.0f;
                    pose->tilt = std::clamp(tilt * kPi / 180.0f, 0.0f, kMaxTilt);
                    pose->zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
                }
                g_player_zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
            }
        }
        g_active = 1;
    }
    Scope::~Scope() {
        g_active = 0;
        g_last_flat.clear();
        g_pointer_field_x = -1;
        g_ships.clear();
    }
    Popup_Scope::Popup_Scope() {
        ++g_popups;
    }
    Popup_Scope::~Popup_Scope() {
        --g_popups;
    }

    bool Active() {
        return g_active.load() != 0;
    }
    bool Is_Default_View() {
        if (g_centre_known.load() == 0) {
            return true; // no frame of this battle has been composed yet
        }
        // straight down, unzoomed and centred on the game's own window: the frame can be shown as the game drew it
        return g_shown.yaw.load() == 0.0f && g_shown.tilt.load() == 0.0f && g_shown.zoom.load() == 1.0f &&
            g_shown.centre_x.load() - g_window_x.load() == kCentreX && g_shown.centre_y.load() - g_window_y.load() == kCentreY;
    }

    namespace {
        // Once a frame, on the game thread: note where the game's window is, apply held pan keys, and move the
        // shown view toward the target.
        void Update_() {
            const int32_t cell_x = MOX::cur_combat_x, cell_y = MOX::cur_combat_y;
            g_window_x = (float)cell_x * 20.0f;
            g_window_y = (float)cell_y * 20.0f;
            g_field_width = (float)MOX::combat_grid_width * 20.0f;
            g_field_height = (float)MOX::combat_grid_height * 20.0f;
            const float window_centre_x = g_window_x.load() + kCentreX, window_centre_y = g_window_y.load() + kCentreY;
            if (g_centre_known.load() == 0) {
                for (s_pose* pose : {&g_target, &g_shown}) {
                    pose->centre_x = window_centre_x + g_start_offset_x;
                    pose->centre_y = window_centre_y + g_start_offset_y;
                }
                g_centre_known = 1;
            } else if ((cell_x != g_last_window_cell_x || cell_y != g_last_window_cell_y) && !(cell_x == g_asked_cell_x && cell_y == g_asked_cell_y) &&
                !g_framing && MOX::cur_ship != g_turn_ship &&
                !(MOX::cur_ship > 0 && MOX::cur_ship == g_followed_ship && COMBAT1::Ship_Controlled_By_Computer(MOX::cur_ship) != 0)) {
                // the game moved its own window (to the next ship, to a target, from the minimap): head there too.
                // The shown centre is left where it is, so the view glides across instead of cutting.
                g_target.centre_x = window_centre_x;
                g_target.centre_y = window_centre_y;
            }
            g_last_window_cell_x = cell_x;
            g_last_window_cell_y = cell_y;

            const Uint64 now = SDL_GetTicks();
            const float dt = g_last_update == 0 ? 0.0f : std::min(0.1f, (float)(now - g_last_update) / 1000.0f);
            g_last_update = now;

            const int32_t key_x = g_pan_key_x.load(), key_y = g_pan_key_y.load();
            if ((key_x != 0 || key_y != 0) && dt > 0.0f) {
                float du, dw;
                Field_Step_(View_Of_(g_target), (float)key_x * kPanSpeed * dt, (float)key_y * kPanSpeed * dt, &du, &dw);
                g_target.centre_x = g_target.centre_x.load() + du;
                g_target.centre_y = g_target.centre_y.load() + dw;
                Clamp_Target_Centre_();
            }

            const float blend = dt <= 0.0f ? 1.0f : 1.0f - expf(-dt / kEaseSeconds);
            const auto ease = [blend](std::atomic<float>& shown, const std::atomic<float>& target, float close_enough) {
                const float to = target.load(), from = shown.load();
                shown = fabsf(to - from) <= close_enough ? to : from + (to - from) * blend;
            };
            ease(g_shown.yaw, g_target.yaw, 0.0005f);
            ease(g_shown.tilt, g_target.tilt, 0.0005f);
            ease(g_shown.zoom, g_target.zoom, 0.0005f);
            ease(g_shown.centre_x, g_target.centre_x, 0.05f);
            ease(g_shown.centre_y, g_target.centre_y, 0.05f);
        }
    } // namespace

    bool Wanted_Window(int16_t* centre_cell_x, int16_t* centre_cell_y) {
        if (!Active() || g_centre_known.load() == 0) {
            return false;
        }
        // the window, in whole cells, that would put the camera's target in its middle
        const int32_t max_x = std::max(0, MOX::combat_grid_width - COMBAT_GRID_VIEW_WIDTH), max_y = std::max(0, MOX::combat_grid_height - COMBAT_GRID_VIEW_HEIGHT);
        const int32_t cell_x = std::clamp((int32_t)lroundf((g_target.centre_x.load() - kCentreX) / 20.0f), 0, max_x);
        const int32_t cell_y = std::clamp((int32_t)lroundf((g_target.centre_y.load() - kCentreY) / 20.0f), 0, max_y);
        if (cell_x == MOX::cur_combat_x && cell_y == MOX::cur_combat_y) {
            return false;
        }
        g_asked_cell_x = cell_x;
        g_asked_cell_y = cell_y;
        *centre_cell_x = (int16_t)(cell_x + COMBAT_GRID_VIEW_WIDTH / 2);
        *centre_cell_y = (int16_t)(cell_y + COMBAT_GRID_VIEW_HEIGHT / 2);
        static int32_t logged = 0;
        if (logged < 6) {
            ++logged;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "battle cam: asking the game to move its window to cell %d,%d", (int)cell_x, (int)cell_y);
        }
        return true;
    }

    void Add_Beam(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t palette_base, int16_t palette_span, bool wide) {
        if (Active() && g_beams.size() < 256) {
            g_beams.push_back({(float)x1, (float)y1, (float)x2, (float)y2, palette_base, palette_span, wide});
        }
    }

    void Begin_Field_Frame() {
        g_ships.clear();
        g_beams.clear();
        g_cursor_size = 0;
    }

    void Set_Move_Highlight(bool shown, int32_t box_pixels, uint8_t color) {
        g_highlight_cells = shown ? std::max(1, box_pixels / 20) : 0;
        g_highlight_color = color;
    }

    void Set_Floor_Cursor(int32_t window_x, int32_t window_y, int32_t size_pixels) {
        g_cursor_x = window_x;
        g_cursor_y = window_y;
        g_cursor_size = size_pixels;
    }

    bool Add_Ship(int16_t ship_idx, int16_t centre_x, int16_t centre_y, int16_t engine_step, float heading, float roll) {
        if (!Active() || !splat_ship::Covers(ship_idx)) {
            return false;
        }
        // When the turn passes to one of the player's own ships, glide over to it once, then leave the camera alone.
        if (ship_idx == MOX::cur_ship && ship_idx != g_turn_ship && g_centre_known.load() != 0) {
            g_turn_ship = ship_idx;
            g_player_moved_camera = 0; // a new ship's turn: follow it again until the player takes the camera
            if (!g_framing && COMBAT1::Ship_Controlled_By_Computer(ship_idx) == 0) {
                g_target.centre_x = (float)MOX::cur_combat_x * 20.0f + (float)centre_x;
                g_target.centre_y = (float)MOX::cur_combat_y * 20.0f + (float)centre_y;
                g_target.zoom = g_player_zoom.load();
                Clamp_Target_Centre_();
            }
        }
        // The ship whose turn it is stays in the middle of the view while it moves: always for a computer-controlled
        // ship, and for one of the player's own ships until the player slides the camera away. Holding a pan key
        // leaves the camera to the player in either case.
        const bool computer_ship = COMBAT1::Ship_Controlled_By_Computer(ship_idx) != 0;
        if (ship_idx == MOX::cur_ship) {
            g_following_player = (!computer_ship && g_player_moved_camera.load() == 0) ? 1 : 0;
        }
        if (ship_idx == MOX::cur_ship && !g_framing && g_centre_known.load() != 0 && g_pan_key_x.load() == 0 && g_pan_key_y.load() == 0 &&
            (computer_ship || g_player_moved_camera.load() == 0)) {
            g_target.zoom = g_player_zoom.load();
            g_target.centre_x = (float)MOX::cur_combat_x * 20.0f + (float)centre_x;
            g_target.centre_y = (float)MOX::cur_combat_y * 20.0f + (float)centre_y;
            Clamp_Target_Centre_();
            static int32_t logged = 0;
            if (logged < 8 && (logged == 0 || ship_idx != g_followed_ship)) {
                ++logged;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "battle cam: following %s ship %d", computer_ship ? "computer" : "player's", (int)ship_idx);
            }
            g_followed_ship = ship_idx;
        }
        for (s_ship& ship : g_ships) {
            if (ship.ship_idx == ship_idx) {
                ship = {ship_idx, centre_x, centre_y, engine_step, heading, roll};
                return true;
            }
        }
        g_ships.push_back({ship_idx, centre_x, centre_y, engine_step, heading, roll});
        return true;
    }

    namespace {
        // A line between two screen points, `thickness` pixels wide, clipped to the battlefield.
        void Screen_Line_(uint8_t* pixels, int32_t stride, float x0, float y0, float x1, float y1, int32_t thickness, uint8_t color) {
            const int32_t steps = std::max(1, (int32_t)ceilf(std::max(fabsf(x1 - x0), fabsf(y1 - y0))));
            const bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
            for (int32_t i = 0; i <= steps; ++i) {
                const int32_t x = (int32_t)floorf(x0 + (x1 - x0) * (float)i / (float)steps);
                const int32_t y = (int32_t)floorf(y0 + (y1 - y0) * (float)i / (float)steps);
                for (int32_t t = 0; t < thickness; ++t) {
                    const int32_t px = steep ? x + t - thickness / 2 : x, py = steep ? y : y + t - thickness / 2;
                    if (px >= 0 && px < kFieldWidth && py >= 0 && py < kFieldHeight) {
                        pixels[(size_t)py * stride + px] = color;
                    }
                }
            }
        }

        // The game draws a beam only inside its own window, so a shot to or from a ship outside that window stops
        // at the window's edge. Each beam the game draws is noted with its full length; the parts of it that fall
        // outside the window are drawn here, taking their colour from the part the game did draw.
        void Draw_Beams_Beyond_Window_(uint8_t* pixels, int32_t stride, const s_view& v) {
            const auto inside = [](float x, float y) { return x >= 0.0f && x < (float)kFieldWidth && y >= 0.0f && y < (float)kFieldHeight; };
            int32_t carried = 0;
            for (const s_beam& beam : g_beams) {
                const float length = hypotf(beam.x2 - beam.x1, beam.y2 - beam.y1);
                if (length < 1.0f || (inside(beam.x1, beam.y1) && inside(beam.x2, beam.y2))) {
                    continue;
                }
                const float dx = (beam.x2 - beam.x1) / length, dy = (beam.y2 - beam.y1) / length;
                // the beam's colour: the brightest-indexed pixel the game drew across the line, somewhere inside the
                // window; failing that, the middle of the beam's own palette run
                uint8_t color = (uint8_t)std::clamp(beam.palette_base + beam.palette_span / 2, 1, 239);
                for (float t = 0.0f; t <= length; t += 4.0f) {
                    const float x = beam.x1 + dx * t, y = beam.y1 + dy * t;
                    if (!inside(x, y)) {
                        continue;
                    }
                    const size_t at = (size_t)y * kFieldWidth + (size_t)x;
                    if (g_field[at] != g_space[at] && g_field[at] >= beam.palette_base && g_field[at] <= beam.palette_base + beam.palette_span) {
                        color = g_field[at];
                        break;
                    }
                }
                const int32_t width = std::max(1, (int32_t)lroundf((beam.wide ? 3.0f : 1.0f) * v.zoom));
                // walk the beam and draw each stretch that lies outside the window
                float run_start = -1.0f;
                for (float t = 0.0f; t <= length + 1.0f; t += 1.0f) {
                    const float tt = std::min(t, length);
                    const bool out = !inside(beam.x1 + dx * tt, beam.y1 + dy * tt) && t <= length;
                    if (out && run_start < 0.0f) {
                        run_start = tt;
                    } else if (!out && run_start >= 0.0f) {
                        float sx0, sy0, sx1, sy1, depth_unused;
                        Project_(v, beam.x1 + dx * run_start, 0.0f, beam.y1 + dy * run_start, &sx0, &sy0, &depth_unused);
                        Project_(v, beam.x1 + dx * tt, 0.0f, beam.y1 + dy * tt, &sx1, &sy1, &depth_unused);
                        Screen_Line_(pixels, stride, sx0, sy0, sx1, sy1, width, color);
                        run_start = -1.0f;
                        ++carried;
                    }
                }
            }
            static int32_t logged = 0;
            if (carried > 0 && logged < 5) {
                ++logged;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "battle cam: drew %d beam stretches beyond the game's window (%d beams this frame)", (int)carried, (int)g_beams.size());
            }
        }

        // Missiles and fighters in flight outside the game's window are not drawn by the game at all. Mark each
        // with a small pale dot so that it can be seen coming.
        void Mark_Far_Flyers_(uint8_t* pixels, int32_t stride, const s_view& v) {
            const float window_x = (float)MOX::cur_combat_x * 20.0f, window_y = (float)MOX::cur_combat_y * 20.0f;
            const int32_t size = std::max(2, (int32_t)lroundf(2.0f * v.zoom));
            if (MOX::missile == nullptr || g_ships.empty()) {
                return; // the battle is still being set up: the flyer table is not ready to read
            }
            for (int32_t i = 0; i < MAX_COMBAT_FLYERS; ++i) {
                const s_missile& flyer = MOX::missile[i];
                if (flyer.type <= 0) {
                    continue;
                }
                const float u = (float)flyer.x - window_x, w = (float)flyer.y - window_y;
                if (u >= 0.0f && u < (float)kFieldWidth && w >= 0.0f && w < (float)kFieldHeight) {
                    continue; // the game draws this one itself
                }
                float sx, sy, depth_unused;
                Project_(v, u, 0.0f, w, &sx, &sy, &depth_unused);
                for (int32_t y = 0; y < size; ++y) {
                    for (int32_t x = 0; x < size; ++x) {
                        const int32_t px = (int32_t)floorf(sx) + x - size / 2, py = (int32_t)floorf(sy) + y - size / 2;
                        if (px >= 0 && px < kFieldWidth && py >= 0 && py < kFieldHeight) {
                            pixels[(size_t)py * stride + px] = 31;
                        }
                    }
                }
            }
        }
    } // namespace

    namespace {
        // The minimap sits in the panel below the battlefield: 1.5 pixels to a cell across, and the field's whole
        // height in 102 pixels down, from 499,363.
        constexpr float kMapLeft = 499.0f, kMapTop = 363.0f, kMapRight = 620.0f, kMapBottom = 465.0f;
        inline void World_To_Map_(float world_x, float world_y, float* map_x, float* map_y) {
            *map_x = kMapLeft + world_x * (1.5f / 20.0f);
            *map_y = kMapTop + world_y * (102.0f / std::max(1.0f, g_field_height.load()));
        }

        // What the camera sees, outlined on the minimap. Drawn on the shown copy of every frame, so it keeps up
        // with the camera as it follows a ship, and leaves nothing behind on the game's own page.
        void Draw_Minimap_Outline_(uint8_t* pixels, int32_t stride, const s_view& v) {
            const float corner[4][2] = {{0.0f, 0.0f}, {(float)kFieldWidth - 1.0f, 0.0f}, {(float)kFieldWidth - 1.0f, (float)kFieldHeight - 1.0f}, {0.0f, (float)kFieldHeight - 1.0f}};
            float map_x[4], map_y[4];
            for (int32_t i = 0; i < 4; ++i) {
                float u, w;
                Unproject_(v, corner[i][0], corner[i][1], &u, &w);
                World_To_Map_(u + g_window_x.load(), w + g_window_y.load(), &map_x[i], &map_y[i]);
                map_x[i] = std::clamp(map_x[i], kMapLeft, kMapRight);
                map_y[i] = std::clamp(map_y[i], kMapTop, kMapBottom);
            }
            for (int32_t i = 0; i < 4; ++i) {
                const float x0 = map_x[i], y0 = map_y[i], x1 = map_x[(i + 1) % 4], y1 = map_y[(i + 1) % 4];
                const int32_t steps = std::max(1, (int32_t)ceilf(std::max(fabsf(x1 - x0), fabsf(y1 - y0))));
                for (int32_t k = 0; k <= steps; ++k) {
                    const int32_t x = (int32_t)floorf(x0 + (x1 - x0) * (float)k / (float)steps), y = (int32_t)floorf(y0 + (y1 - y0) * (float)k / (float)steps);
                    pixels[(size_t)y * stride + x] = 138; // the colour the game uses for its own view box
                }
            }
        }
    } // namespace

    void Compose(uint8_t* pixels, int32_t stride) {
        if (!Active() || pixels == nullptr) {
            return;
        }
        Update_();
        const s_view v = View_();
        // A window over the battlefield: keep what the game drew, put the battlefield back to how it last was
        // without the window, compose that as usual, and lay the window on top at the end.
        const bool popup = g_popups.load() > 0 && g_last_flat.size() == (size_t)kFieldWidth * kFieldHeight;
        if (popup) {
            g_popup.resize(g_last_flat.size());
            for (int32_t y = 0; y < kFieldHeight; ++y) {
                memcpy(&g_popup[(size_t)y * kFieldWidth], pixels + (size_t)y * stride, kFieldWidth);
                memcpy(pixels + (size_t)y * stride, &g_last_flat[(size_t)y * kFieldWidth], kFieldWidth);
            }
        } else {
            g_last_flat.resize((size_t)kFieldWidth * kFieldHeight);
            for (int32_t y = 0; y < kFieldHeight; ++y) {
                memcpy(&g_last_flat[(size_t)y * kFieldWidth], pixels + (size_t)y * stride, kFieldWidth);
            }
        }
        struct s_lay_popup {
            bool wanted;
            uint8_t* pixels;
            int32_t stride;
            ~s_lay_popup() {
                if (!wanted) {
                    return;
                }
                // the window is the rectangle around everything that changed since the battlefield was last clear
                int32_t x0 = kFieldWidth, y0 = kFieldHeight, x1 = -1, y1 = -1;
                for (int32_t y = 0; y < kFieldHeight; ++y) {
                    for (int32_t x = 0; x < kFieldWidth; ++x) {
                        if (g_popup[(size_t)y * kFieldWidth + x] != g_last_flat[(size_t)y * kFieldWidth + x]) {
                            x0 = std::min(x0, x);
                            x1 = std::max(x1, x);
                            y0 = std::min(y0, y);
                            y1 = std::max(y1, y);
                        }
                    }
                }
                for (int32_t y = y0; y <= y1; ++y) {
                    memcpy(pixels + (size_t)y * stride + x0, &g_popup[(size_t)y * kFieldWidth + x0], (size_t)(x1 - x0 + 1));
                }
            }
        } lay_popup{popup, pixels, stride};
        // Space stays fixed behind everything; only what the game draws on top of it (beams, missiles, explosions,
        // markers, ships without a splat model) belongs to the battlefield and is carried through the camera.
        // Anything in the frame that differs from the empty starfield is taken to be on the field.
        Build_Space_(pixels, stride);
        // Where the active ship can move. The game marks, for the whole field, each cell the ship's top-left corner
        // may go to; the ship then covers a square of cells from there. Spread those marks into the cells covered.
        const int32_t grid_width = MOX::combat_grid_width, grid_height = MOX::combat_grid_height;
        const bool highlight = g_highlight_cells > 0 && grid_width > 0 && grid_height > 0;
        if (highlight) {
            g_reachable.assign((size_t)grid_width * grid_height, 0);
            for (int32_t cx = 0; cx < grid_width; ++cx) {
                for (int32_t cy = 0; cy < grid_height; ++cy) {
                    if (MOX::combat_grid[cx][cy] != 2000) {
                        continue;
                    }
                    for (int32_t ox = 0; ox < g_highlight_cells && cx + ox < grid_width; ++ox) {
                        for (int32_t oy = 0; oy < g_highlight_cells && cy + oy < grid_height; ++oy) {
                            g_reachable[(size_t)(cy + oy) * grid_width + cx + ox] = 1;
                        }
                    }
                }
            }
        }
        const int32_t window_cell_x = MOX::cur_combat_x, window_cell_y = MOX::cur_combat_y;
        if (!Is_Default_View() || highlight) {
            g_field.resize((size_t)kFieldWidth * kFieldHeight);
            size_t drawn_on = 0;
            for (int32_t y = 0; y < kFieldHeight; ++y) {
                memcpy(&g_field[(size_t)y * kFieldWidth], pixels + (size_t)y * stride, kFieldWidth);
                for (int32_t x = 0; x < kFieldWidth; ++x) {
                    drawn_on += g_field[(size_t)y * kFieldWidth + x] != g_space[(size_t)y * kFieldWidth + x];
                }
            }
            if (++g_frames_composed == 200) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "battle cam: %.2f%% of the battlefield differs from empty space at frame 200",
                    100.0f * (float)drawn_on / (float)(kFieldWidth * kFieldHeight));
            }
            for (int32_t y = 0; y < kFieldHeight; ++y) {
                uint8_t* row = pixels + (size_t)y * stride;
                // unprojection is linear across a row, so step it
                float u0, w0, u1, w1;
                Unproject_(v, 0.5f, y + 0.5f, &u0, &w0);
                Unproject_(v, 1.5f, y + 0.5f, &u1, &w1);
                const float du = u1 - u0, dw = w1 - w0;
                for (int32_t x = 0; x < kFieldWidth; ++x) {
                    const int32_t u = (int32_t)floorf(u0 + du * x), w = (int32_t)floorf(w0 + dw * x);
                    uint8_t pixel = g_space[(size_t)y * kFieldWidth + x];
                    if (highlight) {
                        // the highlight is laid on the field for the whole battlefield, not only the game's window
                        const int32_t cell_x = (int32_t)floorf((float)u / 20.0f) + window_cell_x, cell_y = (int32_t)floorf((float)w / 20.0f) + window_cell_y;
                        if (cell_x >= 0 && cell_x < grid_width && cell_y >= 0 && cell_y < grid_height && g_reachable[(size_t)cell_y * grid_width + cell_x] != 0) {
                            pixel = g_highlight_color;
                        }
                    }
                    if (u >= 0 && u < kFieldWidth && w >= 0 && w < kFieldHeight) {
                        const size_t at = (size_t)w * kFieldWidth + u;
                        if (g_field[at] != g_space[at]) {
                            pixel = g_field[at];
                        }
                    }
                    row[x] = pixel;
                }
            }
        }
        if (!Is_Default_View() || highlight) {
            Draw_Beams_Beyond_Window_(pixels, stride, v);
            Mark_Far_Flyers_(pixels, stride, v);
        }
        // The floor cursor: the square of cells the active ship would take up if sent to where the mouse points,
        // outlined on the battlefield itself with a cross at its centre, so it turns and tips with the field. The
        // game works out that square (it depends on the ship's size); with no such square it is the single cell
        // under the mouse. It goes down before the ships, which stand on top of it.
        const int32_t pointer_x = g_pointer_field_x.load(), pointer_y = g_pointer_field_y.load();
        if (pointer_x >= 0 && pointer_y >= 0) {
            const bool sized = g_cursor_size > 0;
            const float left = sized ? (float)g_cursor_x : (float)(pointer_x / 20 * 20), top = sized ? (float)g_cursor_y : (float)(pointer_y / 20 * 20);
            const float extent = sized ? (float)g_cursor_size : 20.0f, right = left + extent, bottom = top + extent;
            const float mid_x = left + extent * 0.5f, mid_y = top + extent * 0.5f;
            const float segment[6][4] = {{left, top, right, top}, {right, top, right, bottom}, {right, bottom, left, bottom}, {left, bottom, left, top},
                {mid_x - 4.0f, mid_y, mid_x + 4.0f, mid_y}, {mid_x, mid_y - 4.0f, mid_x, mid_y + 4.0f}};
            const uint8_t color = 31; // the palest of the shared greys
            for (const float* line : segment) {
                float x0, y0, x1, y1, depth;
                Project_(v, line[0], 0.0f, line[1], &x0, &y0, &depth);
                Project_(v, line[2], 0.0f, line[3], &x1, &y1, &depth);
                const int32_t steps = std::max(1, (int32_t)ceilf(std::max(fabsf(x1 - x0), fabsf(y1 - y0))));
                for (int32_t i = 0; i <= steps; ++i) {
                    const int32_t x = (int32_t)floorf(x0 + (x1 - x0) * (float)i / (float)steps);
                    const int32_t y = (int32_t)floorf(y0 + (y1 - y0) * (float)i / (float)steps);
                    if (x >= 0 && x < kFieldWidth && y >= 0 && y < kFieldHeight) {
                        pixels[(size_t)y * stride + x] = color;
                    }
                }
            }
        }
        Draw_Minimap_Outline_(pixels, stride, v);
        if (g_ships.empty()) {
            return;
        }
        // the camera's axes in the world: screen right, screen up and toward the eye
        const float camera[3][3] = {{v.cy, 0.0f, -v.sy}, {-v.sy * v.ct, v.st, -v.cy * v.ct}, {v.sy * v.st, v.ct, v.cy * v.st}};
        struct s_placed {
            float sx, sy, depth;
            const s_ship* ship;
        };
        std::vector<s_placed> placed;
        placed.reserve(g_ships.size());
        for (const s_ship& ship : g_ships) {
            s_placed p;
            p.ship = &ship;
            Project_(v, (float)ship.centre_x, 0.0f, (float)ship.centre_y, &p.sx, &p.sy, &p.depth);
            placed.push_back(p);
        }
        std::sort(placed.begin(), placed.end(), [](const s_placed& a, const s_placed& b) { return a.depth < b.depth; });
        for (const s_placed& p : placed) {
            int32_t size = 0;
            if (!splat_ship::Render_View(p.ship->ship_idx, p.ship->engine_step, p.ship->heading, p.ship->roll, 0.0f, camera, v.zoom, &g_ship_pixels, &size)) {
                continue;
            }
            const int32_t left = (int32_t)lroundf(p.sx) - size / 2, top = (int32_t)lroundf(p.sy) - size / 2;
            for (int32_t y = 0; y < size; ++y) {
                const int32_t dy = top + y;
                if (dy < 0 || dy >= kFieldHeight) {
                    continue;
                }
                for (int32_t x = 0; x < size; ++x) {
                    const int32_t dx = left + x;
                    const uint8_t pixel = g_ship_pixels[(size_t)y * size + x];
                    if (pixel != 0 && dx >= 0 && dx < kFieldWidth) {
                        pixels[(size_t)dy * stride + dx] = pixel;
                    }
                }
            }
        }
        // Zoomed far out, a ship is only a few pixels. Give each one a marker in its side's colour, bigger for
        // bigger hulls, with the ship whose turn it is ringed in pale grey.
        if (v.zoom < kIconZoom) {
            for (const s_placed& p : placed) {
                const s_combat_data& ship = MOX::combat_data[p.ship->ship_idx];
                const int32_t reach = 2 + std::clamp(ship.picture_num / 8, 0, 4); // a diamond 5 to 13 pixels across
                const uint8_t color = ship.owner == MOX::combat_attacker_player ? uint8_t{42} : uint8_t{106};
                const bool active = p.ship->ship_idx == MOX::cur_ship;
                const int32_t cx = (int32_t)lroundf(p.sx), cy = (int32_t)lroundf(p.sy);
                const int32_t outer = reach + (active ? 2 : 0);
                for (int32_t y = -outer; y <= outer; ++y) {
                    for (int32_t x = -outer; x <= outer; ++x) {
                        const int32_t distance = abs(x) + abs(y);
                        if (distance > outer || (distance > reach && distance < outer)) {
                            continue; // outside, or the gap between the marker and its ring
                        }
                        const int32_t px = cx + x, py = cy + y;
                        if (px >= 0 && px < kFieldWidth && py >= 0 && py < kFieldHeight) {
                            pixels[(size_t)py * stride + px] = distance > reach ? uint8_t{31} : color;
                        }
                    }
                }
            }
        }
    }

    void Screen_To_Field(float screen_x, float screen_y, float* field_x, float* field_y) {
        Unproject_(View_(), screen_x, screen_y, field_x, field_y);
    }

    bool Remaps_Pointer(int16_t field_y) {
        return Active() && g_popups.load() == 0 && !Is_Default_View() && field_y < kFieldHeight;
    }

    void Field_To_Screen(int16_t* x, int16_t* y) {
        if (!Remaps_Pointer(*y)) {
            return;
        }
        float sx, sy, depth;
        Project_(View_(), *x + 0.5f, 0.0f, *y + 0.5f, &sx, &sy, &depth);
        *x = (int16_t)std::clamp((int32_t)floorf(sx), 0, kFieldWidth - 1);
        *y = (int16_t)std::clamp((int32_t)floorf(sy), 0, kFieldHeight - 1);
    }

    void Map_Pointer(int16_t* x, int16_t* y) {
        if (!Active() || g_popups.load() > 0 || *y >= kFieldHeight) {
            g_pointer_field_x = -1; // off the battlefield, or a window is in front of it
            return;
        }
        if (!Is_Default_View()) {
            float u, w;
            Unproject_(View_(), *x + 0.5f, *y + 0.5f, &u, &w);
            const bool on_field = u >= 0.0f && u < (float)kFieldWidth && w >= 0.0f && w < (float)kFieldHeight;
            *x = (int16_t)std::clamp((int32_t)floorf(u), 0, kFieldWidth - 1);
            *y = (int16_t)std::clamp((int32_t)floorf(w), 0, kFieldHeight - 1);
            if (!on_field) {
                g_pointer_field_x = -1; // pointing past the part of the field the game can act on
                return;
            }
        }
        g_pointer_field_x = *x;
        g_pointer_field_y = *y;
    }

    void Orbit(float delta_x, float delta_y) {
        g_target.yaw = g_target.yaw.load() - delta_x * 0.008f * g_settings.turn_x;
        g_target.tilt = std::clamp(g_target.tilt.load() - delta_y * 0.009f * g_settings.turn_y, 0.0f, kMaxTilt);
    }

    void Zoom(float wheel_steps, float screen_x, float screen_y) {
        // keep the point of the field under the mouse where it is: find it, change the zoom, then move the centre so
        // that the same point lands back under the mouse
        s_view view = View_Of_(g_target);
        float before_u, before_w, after_u, after_w;
        Field_Step_(view, screen_x - kCentreX, screen_y - kCentreY, &before_u, &before_w);
        view.zoom = std::clamp(view.zoom * powf(1.12f, wheel_steps * g_settings.zoom), kMinZoom, kMaxZoom);
        Field_Step_(view, screen_x - kCentreX, screen_y - kCentreY, &after_u, &after_w);
        g_target.zoom = view.zoom;
        g_player_zoom = view.zoom;
        // while the camera follows the player's ship, zoom in and out on the ship, so the follow is not broken
        if (screen_y < (float)kFieldHeight && g_following_player.load() == 0) {
            g_target.centre_x = g_target.centre_x.load() + before_u - after_u;
            g_target.centre_y = g_target.centre_y.load() + before_w - after_w;
            Clamp_Target_Centre_();
        }
    }

    void Pan(float delta_x, float delta_y) {
        g_player_moved_camera = 1;
        // grab and slide: the field moves with the mouse, so the centre moves the other way
        float du, dw;
        Field_Step_(View_Of_(g_target), delta_x, delta_y, &du, &dw);
        g_target.centre_x = g_target.centre_x.load() - du;
        g_target.centre_y = g_target.centre_y.load() - dw;
        Clamp_Target_Centre_();
    }

    bool On_Minimap(float screen_x, float screen_y) {
        return Active() && g_popups.load() == 0 && screen_x >= kMapLeft && screen_x <= kMapRight && screen_y >= kMapTop && screen_y <= kMapBottom;
    }

    void Centre_From_Minimap(float screen_x, float screen_y) {
        g_player_moved_camera = 1;
        g_target.centre_x = (std::clamp(screen_x, kMapLeft, kMapRight) - kMapLeft) * (20.0f / 1.5f);
        g_target.centre_y = (std::clamp(screen_y, kMapTop, kMapBottom) - kMapTop) * (g_field_height.load() / 102.0f);
        Clamp_Target_Centre_();
    }

    void Set_Pan_Keys(int32_t right_minus_left, int32_t down_minus_up) {
        if (right_minus_left != 0 || down_minus_up != 0) {
            g_player_moved_camera = 1;
        }
        g_pan_key_x = right_minus_left;
        g_pan_key_y = down_minus_up;
    }

    void Frame_Points(float ax, float ay, float bx, float by) {
        if (!Active() || g_centre_known.load() == 0) {
            return;
        }
        g_framing = true;
        g_shot_ax = ax;
        g_shot_ay = ay;
        g_shot_bx = bx;
        g_shot_by = by;
        g_target.centre_x = (ax + bx) * 0.5f;
        g_target.centre_y = (ay + by) * 0.5f;
        Clamp_Target_Centre_();
        // how far apart the two are on screen at zoom 1, under the angle the camera is heading for
        s_view view = View_Of_(g_target);
        view.zoom = 1.0f;
        float sx_a, sy_a, sx_b, sy_b, depth;
        Project_(view, ax - g_window_x.load(), 0.0f, ay - g_window_y.load(), &sx_a, &sy_a, &depth);
        Project_(view, bx - g_window_x.load(), 0.0f, by - g_window_y.load(), &sx_b, &sy_b, &depth);
        // leave room for the ships themselves and a margin: the pair may span at most this much of the view
        const float fit_x = ((float)kFieldWidth - 180.0f) / std::max(1.0f, fabsf(sx_b - sx_a));
        const float fit_y = ((float)kFieldHeight - 140.0f) / std::max(1.0f, fabsf(sy_b - sy_a));
        // never closer than the player chose; further out only as far as it takes
        g_target.zoom = std::clamp(std::min(g_player_zoom.load(), std::min(fit_x, fit_y)), kMinZoom, kMaxZoom);
        static int32_t logged = 0;
        if (logged < 6) {
            ++logged;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "battle cam: framing a shot, zoom %.2f (player's zoom %.2f)", g_target.zoom.load(), g_player_zoom.load());
        }
    }

    void End_Framing() {
        g_framing = false;
    }

    bool Settled() {
        return fabsf(g_shown.centre_x.load() - g_target.centre_x.load()) < 1.5f && fabsf(g_shown.centre_y.load() - g_target.centre_y.load()) < 1.5f &&
            fabsf(g_shown.zoom.load() - g_target.zoom.load()) < 0.01f;
    }

    void Reset() {
        g_player_zoom = 1.0f;
        g_target.yaw = 0.0f;
        g_target.tilt = 0.0f;
        g_target.zoom = 1.0f;
        g_target.centre_x = g_window_x.load() + kCentreX;
        g_target.centre_y = g_window_y.load() + kCentreY;
        // the shortest way round back to zero, however many turns have been made
        g_shown.yaw = remainderf(g_shown.yaw.load(), 2.0f * kPi);
    }
} // namespace battle_cam
