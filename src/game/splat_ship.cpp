// LOOK TEST (local patch, not upstream): combat ships drawn live from Gaussian splats.
//
// The ship is authored in ink studio (JavaScript) and exported as a list of splats: a centre, a 3D covariance, a
// surface normal, how shut in the point is, and which colour ramp it is shaded on. Nothing about the view or the
// light is baked in. This file turns that list into the 59 by 60 palette bitmap the combat code already draws,
// for any heading, so ships can be seen from an angle and turn through as many steps as the turn code asks for.
//
// The rasteriser follows ink studio's own software renderer: each splat's covariance is carried to the screen,
// widened a little so sub-pixel splats still land, drawn far to near, and the result is snapped to the palette.
#include "pch.h"

#include <fstream>

#include "game/splat_ship.h"

#if defined(ORION2RE_LOOK_TEST_JS)
#include "quickjs.h"
#endif

namespace splat_ship {
    namespace {
        constexpr int32_t kFloatsPerSplat = 14;
        constexpr float kPi = 3.14159265358979f;
        constexpr float kCameraPitch = 50.0f * kPi / 180.0f; // above the horizon; 90 would be straight down
        constexpr float kLightRight = -0.6f, kLightUp = 0.5f, kLightToEye = 0.3f; // fixed on screen
        constexpr float kSunScale = 0.7f; // the tilted view faces the light more squarely than ink studio's default scene
        // hull length in pixels for frigate, destroyer, cruiser, battleship and titan, measured from the original art
        constexpr float kHullPixels[5] = {15.0f, 22.0f, 30.0f, 44.0f, 55.0f};
        constexpr int32_t kPicturesPerHull = 8;

        struct s_palette_color {
            float r, g, b;
            uint8_t role;       // 0 steel, 1..4 banner tint dark to light, 5 glow, 6 lamp
            uint8_t game_index; // filled in once the combat palette is loaded
        };
        struct s_ramp {
            uint8_t length;
            uint8_t color[7];
        };
        struct s_model {
            bool loaded = false;
            bool tried = false;
            bool palette_mapped = false;
            float hull_length = 4.5f, ambient = 0.2f, sky = 0.22f, sun = 0.72f;
            std::vector<s_palette_color> palette;
            std::vector<s_ramp> ramps;
            std::vector<std::vector<float>> variants; // engine step -> splats
        };
        s_model g_model;

        struct s_orientation {
            float heading; // radians, counter-clockwise on the map from pointing right
            float roll;    // radians about the nose axis, positive dips the port (left) side
            float pitch;   // radians, positive lifts the nose
        };

        struct s_cached {
            std::vector<uint8_t> pixels;
        };
        std::unordered_map<uint32_t, s_cached> g_cache;

        // scratch space reused between draws
        std::vector<uint32_t> g_order;
        std::vector<float> g_depth;
        std::vector<float> g_rgba;

#if defined(ORION2RE_LOOK_TEST_JS)
        // Runs the authoring script (ink studio, the ship scene and the exporter in one file) in QuickJS and
        // takes the packed model it leaves in globalThis.SHIP_MODEL.
        bool Author_In_JavaScript_(std::vector<char>* bytes) {
            std::ifstream file("data/splat/ship-author.js", std::ios::binary);
            if (!file) {
                return false;
            }
            const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const Uint64 started = SDL_GetTicks();
            JSRuntime* runtime = JS_NewRuntime();
            JS_SetMaxStackSize(runtime, 8 * 1024 * 1024);
            JSContext* context = JS_NewContext(runtime);
            bool ok = false;
            JSValue result = JS_Eval(context, source.c_str(), source.size(), "ship-author.js", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(result)) {
                JSValue error = JS_GetException(context);
                const char* text = JS_ToCString(context, error);
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "splat ship: authoring script failed: %s", text != nullptr ? text : "unknown error");
                JS_FreeCString(context, text);
                JS_FreeValue(context, error);
            } else {
                JSValue global = JS_GetGlobalObject(context);
                JSValue model = JS_GetPropertyStr(context, global, "SHIP_MODEL");
                size_t size = 0;
                const uint8_t* data = JS_GetArrayBuffer(context, &size, model);
                if (data != nullptr && size != 0) {
                    bytes->assign((const char*)data, (const char*)data + size);
                    ok = true;
                }
                JS_FreeValue(context, model);
                JS_FreeValue(context, global);
            }
            JS_FreeValue(context, result);
            JS_FreeContext(context);
            JS_FreeRuntime(runtime);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "splat ship: JavaScript authoring %s in %d ms (%d bytes)", ok ? "finished" : "failed",
                (int)(SDL_GetTicks() - started), (int)bytes->size());
            return ok;
        }
#endif

        bool Load_() {
            if (g_model.tried) {
                return g_model.loaded;
            }
            g_model.tried = true;
            std::vector<char> bytes;
            bool authored = false;
#if defined(ORION2RE_LOOK_TEST_JS)
            authored = Author_In_JavaScript_(&bytes);
#endif
            if (!authored) {
                // no script, or it failed: fall back to a model exported ahead of time
                std::ifstream file("data/splat/ship.mspl", std::ios::binary);
                if (!file) {
                    return false;
                }
                bytes.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            }
            const auto u32 = [&](size_t at) {
                uint32_t v;
                memcpy(&v, bytes.data() + at, 4);
                return v;
            };
            const auto f32 = [&](size_t at) {
                float v;
                memcpy(&v, bytes.data() + at, 4);
                return v;
            };
            if (bytes.size() < 36 || memcmp(bytes.data(), "MSPL", 4) != 0 || u32(4) != 1) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "splat ship: the model data is not a version 1 model");
                return false;
            }
            const uint32_t variant_count = u32(8), palette_count = u32(12), ramp_count = u32(16);
            g_model.hull_length = f32(20);
            g_model.ambient = f32(24);
            g_model.sky = f32(28);
            g_model.sun = f32(32);
            size_t at = 36;
            if (bytes.size() < at + palette_count * 4 + ramp_count * 8) {
                return false;
            }
            for (uint32_t i = 0; i < palette_count; ++i, at += 4) {
                g_model.palette.push_back({(uint8_t)bytes[at] / 255.0f, (uint8_t)bytes[at + 1] / 255.0f, (uint8_t)bytes[at + 2] / 255.0f, (uint8_t)bytes[at + 3], 0});
            }
            for (uint32_t i = 0; i < ramp_count; ++i, at += 8) {
                s_ramp ramp = {};
                ramp.length = (uint8_t)bytes[at];
                memcpy(ramp.color, bytes.data() + at + 1, 7);
                if (ramp.length == 0 || ramp.length > 7) {
                    return false;
                }
                g_model.ramps.push_back(ramp);
            }
            for (uint32_t v = 0; v < variant_count; ++v) {
                if (bytes.size() < at + 4) {
                    return false;
                }
                const uint32_t count = u32(at);
                at += 4;
                const size_t size = (size_t)count * kFloatsPerSplat * 4;
                if (bytes.size() < at + size) {
                    return false;
                }
                std::vector<float> splats((size_t)count * kFloatsPerSplat);
                memcpy(splats.data(), bytes.data() + at, size);
                at += size;
                g_model.variants.push_back(std::move(splats));
            }
            g_model.loaded = !g_model.variants.empty();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "splat ship: loaded %u engine steps, %u splats in the first", (unsigned)variant_count,
                (unsigned)(g_model.variants.empty() ? 0 : g_model.variants[0].size() / kFloatsPerSplat));
            return g_model.loaded;
        }

        // Steel takes the nearest of the shared greys (indices 2 to 31). Banner tints take the banner's own ramp
        // (32 to 47), which the engine recolours per player. Glow takes the shared violet ramp the original ship
        // art uses for its drives. These are the same choices the sprite converter made.
        void Map_Palette_() {
            if (g_model.palette_mapped) {
                return;
            }
            g_model.palette_mapped = true;
            std::vector<size_t> glows;
            for (size_t i = 0; i < g_model.palette.size(); ++i) {
                s_palette_color& color = g_model.palette[i];
                if (color.role >= 1 && color.role <= 4) {
                    color.game_index = (uint8_t)(30 + color.role * 3 + (color.role == 4 ? 1 : 0)); // 33, 36, 39, 43
                } else if (color.role == 5) {
                    glows.push_back(i);
                } else if (color.role == 6) {
                    color.game_index = 47;
                } else {
                    float best = 1e9f;
                    for (int32_t g = 2; g <= 31; ++g) {
                        const s_palette_entry& entry = video::current_palette[g];
                        const float dr = entry.r / 63.0f - color.r, dg = entry.g / 63.0f - color.g, db = entry.b / 63.0f - color.b;
                        const float d = dr * dr + dg * dg + db * db;
                        if (d < best) {
                            best = d;
                            color.game_index = (uint8_t)g;
                        }
                    }
                }
            }
            const uint8_t glow_index[4] = {65, 67, 69, 31};
            for (size_t i = 0; i < glows.size(); ++i) {
                g_model.palette[glows[i]].game_index = glow_index[std::min<size_t>(i * 4 / std::max<size_t>(glows.size(), 1), 3)];
            }
        }

        inline float Smooth_(float a, float b, float x) {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Fills `out` (width * height palette indices, 0 for nothing) with the ship at `turn` parts of a full turn,
        // counter-clockwise from pointing right, `hull_pixels` long.
        void Rasterise_(const std::vector<float>& splats, const s_orientation& pose, const float camera[3][3], float hull_pixels, int32_t width, int32_t height, float cx, float cy, uint8_t* out) {
            // The ship's own rotation, model to world: pitch about its wing axis, then roll about its nose axis,
            // then heading about the vertical. Columns of M are where the model's X, Y and Z axes end up.
            const float ch = cosf(pose.heading), sh = sinf(pose.heading), cr = cosf(pose.roll), sr = sinf(pose.roll), ct = cosf(pose.pitch), st = sinf(pose.pitch);
            const float pitch_m[3][3] = {{ct, -st, 0.0f}, {st, ct, 0.0f}, {0.0f, 0.0f, 1.0f}};
            const float roll_m[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, cr, -sr}, {0.0f, sr, cr}};
            const float heading_m[3][3] = {{ch, 0.0f, sh}, {0.0f, 1.0f, 0.0f}, {-sh, 0.0f, ch}};
            float rp[3][3], M[3][3];
            for (int32_t i = 0; i < 3; ++i) {
                for (int32_t j = 0; j < 3; ++j) {
                    rp[i][j] = roll_m[i][0] * pitch_m[0][j] + roll_m[i][1] * pitch_m[1][j] + roll_m[i][2] * pitch_m[2][j];
                }
            }
            for (int32_t i = 0; i < 3; ++i) {
                for (int32_t j = 0; j < 3; ++j) {
                    M[i][j] = heading_m[i][0] * rp[0][j] + heading_m[i][1] * rp[1][j] + heading_m[i][2] * rp[2][j];
                }
            }
            // `camera` holds the camera's axes in the world: screen right, screen up and toward the eye.
            // The same three axes as directions in the model's own space (camera * M).
            float R[3], U[3], D[3];
            for (int32_t j = 0; j < 3; ++j) {
                R[j] = camera[0][0] * M[0][j] + camera[0][1] * M[1][j] + camera[0][2] * M[2][j];
                U[j] = camera[1][0] * M[0][j] + camera[1][1] * M[1][j] + camera[1][2] * M[2][j];
                D[j] = camera[2][0] * M[0][j] + camera[2][1] * M[1][j] + camera[2][2] * M[2][j];
            }
            // which way is up in the world, in model space, for the light from above
            const float up[3] = {M[1][0], M[1][1], M[1][2]};
            float L[3];
            {
                const float ll = sqrtf(kLightRight * kLightRight + kLightUp * kLightUp + kLightToEye * kLightToEye);
                for (int32_t k = 0; k < 3; ++k) {
                    L[k] = (R[k] * kLightRight + U[k] * kLightUp + D[k] * kLightToEye) / ll;
                }
            }
            const float scale = hull_pixels / g_model.hull_length, scale2 = scale * scale;
            const uint32_t count = (uint32_t)(splats.size() / kFloatsPerSplat);

            g_order.resize(count);
            g_depth.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                const float* s = &splats[(size_t)i * kFloatsPerSplat];
                g_order[i] = i;
                g_depth[i] = s[0] * D[0] + s[1] * D[1] + s[2] * D[2];
            }
            std::sort(g_order.begin(), g_order.end(), [](uint32_t a, uint32_t b) { return g_depth[a] < g_depth[b]; });

            g_rgba.assign((size_t)width * height * 4, 0.0f);
            for (const uint32_t i : g_order) {
                const float* s = &splats[(size_t)i * kFloatsPerSplat];
                const float sx = cx + (s[0] * R[0] + s[1] * R[1] + s[2] * R[2]) * scale;
                const float sy = cy - (s[0] * U[0] + s[1] * U[1] + s[2] * U[2]) * scale;
                // covariance carried to the screen: J * C * J^T with J's rows R and -U, in pixels
                const float c00 = s[3] * scale2, c01 = s[4] * scale2, c02 = s[5] * scale2, c11 = s[6] * scale2, c12 = s[7] * scale2, c22 = s[8] * scale2;
                const float u0 = c00 * R[0] + c01 * R[1] + c02 * R[2], u1 = c01 * R[0] + c11 * R[1] + c12 * R[2], u2 = c02 * R[0] + c12 * R[1] + c22 * R[2];
                const float w0 = c00 * U[0] + c01 * U[1] + c02 * U[2], w1 = c01 * U[0] + c11 * U[1] + c12 * U[2], w2 = c02 * U[0] + c12 * U[1] + c22 * U[2];
                const float A = R[0] * u0 + R[1] * u1 + R[2] * u2 + 0.15f;
                const float B = -(U[0] * u0 + U[1] * u1 + U[2] * u2);
                const float C = U[0] * w0 + U[1] * w1 + U[2] * w2 + 0.15f;
                const float det = A * C - B * B;
                if (det <= 0.0f) {
                    continue;
                }
                const float k0 = C / det, k1 = -B / det, k2 = A / det;
                const float ex = std::min(3.0f * sqrtf(A), 64.0f), ey = std::min(3.0f * sqrtf(C), 64.0f);
                const int32_t x0 = std::max(0, (int32_t)floorf(sx - ex)), x1 = std::min(width - 1, (int32_t)ceilf(sx + ex));
                const int32_t y0 = std::max(0, (int32_t)floorf(sy - ey)), y1 = std::min(height - 1, (int32_t)ceilf(sy + ey));
                if (x0 > x1 || y0 > y1) {
                    continue;
                }
                // shade: how open the point is, lit from above the ship and by the fixed on-screen light
                const int32_t ramp_word = (int32_t)s[13];
                const s_ramp& ramp = g_model.ramps[std::min<size_t>((size_t)(ramp_word & 255), g_model.ramps.size() - 1)];
                float shade = 1.0f;
                if (ramp_word < 256) {
                    const float facing_light = std::max(0.0f, s[9] * L[0] + s[10] * L[1] + s[11] * L[2]);
                    shade = s[12] * (g_model.ambient + g_model.sky * (0.5f + 0.5f * (s[9] * up[0] + s[10] * up[1] + s[11] * up[2]))) + g_model.sun * kSunScale * facing_light;
                }
                const int32_t step = std::clamp((int32_t)floorf(shade * ramp.length), 0, (int32_t)ramp.length - 1);
                const s_palette_color& color = g_model.palette[std::min<size_t>(ramp.color[step], g_model.palette.size() - 1)];
                for (int32_t y = y0; y <= y1; ++y) {
                    const float dy = y + 0.5f - sy;
                    for (int32_t x = x0; x <= x1; ++x) {
                        const float dx = x + 0.5f - sx;
                        const float power = -0.5f * (k0 * dx * dx + k2 * dy * dy) - k1 * dx * dy;
                        if (power > 0.0f || power < -4.5f) {
                            continue;
                        }
                        const float a = 0.99f * Smooth_(0.38f, 0.62f, 3.0f * expf(power));
                        if (a < 0.004f) {
                            continue;
                        }
                        float* p = &g_rgba[((size_t)y * width + x) * 4];
                        p[0] += (color.r - p[0]) * a;
                        p[1] += (color.g - p[1]) * a;
                        p[2] += (color.b - p[2]) * a;
                        p[3] += (1.0f - p[3]) * a;
                    }
                }
            }
            // snap to the palette: a pixel is either drawn or not, and takes the nearest of the model's colours
            for (int32_t i = 0; i < width * height; ++i) {
                const float* p = &g_rgba[(size_t)i * 4];
                uint8_t index = 0;
                if (p[3] >= 0.5f) {
                    float best = 1e9f;
                    for (size_t c = 1; c < g_model.palette.size(); ++c) {
                        const s_palette_color& color = g_model.palette[c];
                        const float dr = color.r - p[0], dg = color.g - p[1], db = color.b - p[2];
                        const float d = dr * dr + dg * dg + db * db;
                        if (d < best) {
                            best = d;
                            index = color.game_index;
                        }
                    }
                }
                out[i] = index;
            }
        }
    } // namespace

    bool Covers(int16_t ship_idx) {
        if (ship_idx <= 0 || !Load_()) {
            return false;
        }
        const s_combat_data& ship = MOX::combat_data[ship_idx];
        return ship.previous_owner < MAX_PLAYERS && ship.picture_num >= 0 && ship.picture_num < 5 * kPicturesPerHull;
    }

    bool Draw(int16_t ship_idx, int16_t engine_step, float heading, float roll, float pitch, s_animation_header* dest) {
        if (!Covers(ship_idx)) {
            return false;
        }
        Map_Palette_();
        const s_combat_data& ship = MOX::combat_data[ship_idx];
        const s_animation_header* picture = ship.ship_image_seg;
        const int32_t width = picture != nullptr ? picture->width : 59, height = picture != nullptr ? picture->height : 60;
        const int32_t hull = ship.picture_num / kPicturesPerHull;
        const int32_t variant = std::clamp<int32_t>(engine_step, 0, (int32_t)g_model.variants.size() - 1);
        const bool defender = ship.owner != MOX::combat_attacker_player;
        const s_orientation pose = {heading, roll, pitch};

        // A ship at rest sits on one of the 16 facings with no roll or pitch, and is drawn again every frame, so
        // those views are kept. Anything else (a ship mid-turn) is drawn fresh at its exact orientation.
        const float facing_steps = heading * (16.0f / (2.0f * kPi));
        const int32_t facing = (int32_t)lroundf(facing_steps);
        const bool at_rest = fabsf(facing_steps - (float)facing) < 1e-4f && roll == 0.0f && pitch == 0.0f;
        static std::vector<uint8_t> scratch;
        const std::vector<uint8_t>* pixels = nullptr;
        const auto finish = [&](std::vector<uint8_t>& target) {
            target.resize((size_t)width * height);
            // the fixed view used for ship pictures in the panels: looking down from kCameraPitch above the horizon
            const float cp = cosf(kCameraPitch), sp = sinf(kCameraPitch);
            const float camera[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, cp, -sp}, {0.0f, sp, cp}};
            // the originals sit about a pixel left of the canvas centre
            Rasterise_(g_model.variants[variant], pose, camera, kHullPixels[hull], width, height, width * 0.5f - 0.5f, height * 0.5f, target.data());
            if (defender) {
                // the defending side's banner colours live 64 entries up, as the engine arranges for sprite art
                for (uint8_t& pixel : target) {
                    if (pixel >= 32 && pixel < 64) {
                        pixel = (uint8_t)(pixel + 64);
                    }
                }
            }
        };
        if (at_rest) {
            const uint32_t key = (uint32_t)(((facing % 16) + 16) % 16) | ((uint32_t)hull << 4) | ((uint32_t)variant << 8) | (defender ? 1u << 12 : 0u);
            auto found = g_cache.find(key);
            if (found == g_cache.end()) {
                s_cached entry;
                finish(entry.pixels);
                found = g_cache.emplace(key, std::move(entry)).first;
            }
            pixels = &found->second.pixels;
        } else {
            finish(scratch);
            pixels = &scratch;
            static int32_t logged = 0;
            if (logged < 12) {
                ++logged;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "splat ship: ship %d drawn at heading %.2f deg, roll %.2f deg", (int)ship_idx,
                    heading * 180.0f / kPi, roll * 180.0f / kPi);
            }
        }
        bitmap::Create_Blank_Bitmap((int16_t)width, (int16_t)height, dest);
        memcpy((uint8_t*)dest + sizeof(s_animation_header) + sizeof(uint32_t), pixels->data(), pixels->size());
        return true;
    }

    bool Render_View(int16_t ship_idx, int16_t engine_step, float heading, float roll, float pitch, const float camera[3][3], float zoom, std::vector<uint8_t>* pixels, int32_t* size) {
        if (!Covers(ship_idx)) {
            return false;
        }
        Map_Palette_();
        const s_combat_data& ship = MOX::combat_data[ship_idx];
        const int32_t hull = ship.picture_num / kPicturesPerHull;
        const int32_t variant = std::clamp<int32_t>(engine_step, 0, (int32_t)g_model.variants.size() - 1);
        const float hull_pixels = kHullPixels[hull] * zoom;
        *size = ((int32_t)ceilf(hull_pixels * 1.25f) + 6) | 1; // odd, so the ship's centre is a pixel centre
        pixels->resize((size_t)*size * *size);
        const s_orientation pose = {heading, roll, pitch};
        Rasterise_(g_model.variants[variant], pose, camera, hull_pixels, *size, *size, *size * 0.5f, *size * 0.5f, pixels->data());
        if (ship.owner != MOX::combat_attacker_player) {
            for (uint8_t& pixel : *pixels) {
                if (pixel >= 32 && pixel < 64) {
                    pixel = (uint8_t)(pixel + 64);
                }
            }
        }
        return true;
    }
} // namespace splat_ship
