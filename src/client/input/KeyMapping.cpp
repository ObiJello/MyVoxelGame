// File: src/client/input/KeyMapping.cpp
#include "KeyMapping.hpp"
#include "common/core/Log.hpp"
#include "platform/GameDirectory.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <deque>
#include <unordered_map>

namespace Input {

    namespace {

        // Mappings live in a deque so the Binds:: pointers stay valid as the
        // registry is populated (a vector would rehome them on growth).
        std::deque<KeyMapping>    s_storage;
        std::vector<KeyMapping*>  s_ordered;

        // GLFW code -> MC-style suffix, for EVERY key MC names.
        //
        // The printable half of this table used to be absent, and Name() fell
        // through to glfwGetKeyName for those — which returns the glyph the key
        // produces under the CURRENT KEYBOARD LAYOUT. That made the saved form
        // of a binding layout-dependent ("key.keyboard./"), so a config written
        // on one machine or layout could fail to resolve on another and silently
        // come back Unbound. That is what stopped `/` opening the command line
        // on Windows while `T` — a letter, and equally affected in principle —
        // happened to survive.
        //
        // MC never does this: InputConstants builds a static table
        // (`addKey(KEYSYM, "key.keyboard.slash", 47)`) and uses glfwGetKeyName
        // ONLY for the human-facing label. Same split here — `preferGlfwName`
        // marks the entries whose DISPLAY should follow the layout, while the
        // persisted name always comes from this table.
        struct NamedKey {
            int code; const char* name; const char* display;
            bool preferGlfwName = false;   // display only; never affects Name()
        };
        const NamedKey kNamedKeys[] = {
            { GLFW_KEY_SPACE,         "space",         "Space"        },
            { GLFW_KEY_ESCAPE,        "escape",        "Esc"          },
            { GLFW_KEY_ENTER,         "enter",         "Enter"        },
            { GLFW_KEY_TAB,           "tab",           "Tab"          },
            { GLFW_KEY_BACKSPACE,     "backspace",     "Backspace"    },
            { GLFW_KEY_INSERT,        "insert",        "Insert"       },
            { GLFW_KEY_DELETE,        "delete",        "Delete"       },
            { GLFW_KEY_RIGHT,         "right",         "Right"        },
            { GLFW_KEY_LEFT,          "left",          "Left"         },
            { GLFW_KEY_DOWN,          "down",          "Down"         },
            { GLFW_KEY_UP,            "up",            "Up"           },
            { GLFW_KEY_PAGE_UP,       "page.up",       "Page Up"      },
            { GLFW_KEY_PAGE_DOWN,     "page.down",     "Page Down"    },
            { GLFW_KEY_HOME,          "home",          "Home"         },
            { GLFW_KEY_END,           "end",           "End"          },
            { GLFW_KEY_CAPS_LOCK,     "caps.lock",     "Caps Lock"    },
            { GLFW_KEY_LEFT_SHIFT,    "left.shift",    "Left Shift"   },
            { GLFW_KEY_LEFT_CONTROL,  "left.control",  "Left Control" },
            { GLFW_KEY_LEFT_ALT,      "left.alt",      "Left Alt"     },
            { GLFW_KEY_LEFT_SUPER,    "left.win",      "Left Win"     },
            { GLFW_KEY_RIGHT_SHIFT,   "right.shift",   "Right Shift"  },
            { GLFW_KEY_RIGHT_CONTROL, "right.control", "Right Control"},
            { GLFW_KEY_RIGHT_ALT,     "right.alt",     "Right Alt"    },
            { GLFW_KEY_RIGHT_SUPER,   "right.win",     "Right Win"    },
            { GLFW_KEY_GRAVE_ACCENT,  "grave.accent",  "`"            },
            { GLFW_KEY_F1,  "f1",  "F1"  }, { GLFW_KEY_F2,  "f2",  "F2"  },
            { GLFW_KEY_F3,  "f3",  "F3"  }, { GLFW_KEY_F4,  "f4",  "F4"  },
            { GLFW_KEY_F5,  "f5",  "F5"  }, { GLFW_KEY_F6,  "f6",  "F6"  },
            { GLFW_KEY_F7,  "f7",  "F7"  }, { GLFW_KEY_F8,  "f8",  "F8"  },
            { GLFW_KEY_F9,  "f9",  "F9"  }, { GLFW_KEY_F10, "f10", "F10" },
            { GLFW_KEY_F11, "f11", "F11" }, { GLFW_KEY_F12, "f12", "F12" },

            // ── Printable + keypad + remaining function keys ───────────────
            // Generated from MC InputConstants.addKey(KEYSYM, ...) so the
            // persisted names match vanilla's exactly.
            { GLFW_KEY_0,                "0",               "0", true },
            { GLFW_KEY_1,                "1",               "1", true },
            { GLFW_KEY_2,                "2",               "2", true },
            { GLFW_KEY_3,                "3",               "3", true },
            { GLFW_KEY_4,                "4",               "4", true },
            { GLFW_KEY_5,                "5",               "5", true },
            { GLFW_KEY_6,                "6",               "6", true },
            { GLFW_KEY_7,                "7",               "7", true },
            { GLFW_KEY_8,                "8",               "8", true },
            { GLFW_KEY_9,                "9",               "9", true },
            { GLFW_KEY_A,                "a",               "A", true },
            { GLFW_KEY_B,                "b",               "B", true },
            { GLFW_KEY_C,                "c",               "C", true },
            { GLFW_KEY_D,                "d",               "D", true },
            { GLFW_KEY_E,                "e",               "E", true },
            { GLFW_KEY_F,                "f",               "F", true },
            { GLFW_KEY_G,                "g",               "G", true },
            { GLFW_KEY_H,                "h",               "H", true },
            { GLFW_KEY_I,                "i",               "I", true },
            { GLFW_KEY_J,                "j",               "J", true },
            { GLFW_KEY_K,                "k",               "K", true },
            { GLFW_KEY_L,                "l",               "L", true },
            { GLFW_KEY_M,                "m",               "M", true },
            { GLFW_KEY_N,                "n",               "N", true },
            { GLFW_KEY_O,                "o",               "O", true },
            { GLFW_KEY_P,                "p",               "P", true },
            { GLFW_KEY_Q,                "q",               "Q", true },
            { GLFW_KEY_R,                "r",               "R", true },
            { GLFW_KEY_S,                "s",               "S", true },
            { GLFW_KEY_T,                "t",               "T", true },
            { GLFW_KEY_U,                "u",               "U", true },
            { GLFW_KEY_V,                "v",               "V", true },
            { GLFW_KEY_W,                "w",               "W", true },
            { GLFW_KEY_X,                "x",               "X", true },
            { GLFW_KEY_Y,                "y",               "Y", true },
            { GLFW_KEY_Z,                "z",               "Z", true },
            { GLFW_KEY_F13,              "f13",             "F13", true },
            { GLFW_KEY_F14,              "f14",             "F14", true },
            { GLFW_KEY_F15,              "f15",             "F15", true },
            { GLFW_KEY_F16,              "f16",             "F16", true },
            { GLFW_KEY_F17,              "f17",             "F17", true },
            { GLFW_KEY_F18,              "f18",             "F18", true },
            { GLFW_KEY_F19,              "f19",             "F19", true },
            { GLFW_KEY_F20,              "f20",             "F20", true },
            { GLFW_KEY_F21,              "f21",             "F21", true },
            { GLFW_KEY_F22,              "f22",             "F22", true },
            { GLFW_KEY_F23,              "f23",             "F23", true },
            { GLFW_KEY_F24,              "f24",             "F24", true },
            { GLFW_KEY_F25,              "f25",             "F25", true },
            { GLFW_KEY_NUM_LOCK,         "num.lock",        "Num Lock", true },
            { GLFW_KEY_KP_0,             "keypad.0",        "Keypad 0", true },
            { GLFW_KEY_KP_1,             "keypad.1",        "Keypad 1", true },
            { GLFW_KEY_KP_2,             "keypad.2",        "Keypad 2", true },
            { GLFW_KEY_KP_3,             "keypad.3",        "Keypad 3", true },
            { GLFW_KEY_KP_4,             "keypad.4",        "Keypad 4", true },
            { GLFW_KEY_KP_5,             "keypad.5",        "Keypad 5", true },
            { GLFW_KEY_KP_6,             "keypad.6",        "Keypad 6", true },
            { GLFW_KEY_KP_7,             "keypad.7",        "Keypad 7", true },
            { GLFW_KEY_KP_8,             "keypad.8",        "Keypad 8", true },
            { GLFW_KEY_KP_9,             "keypad.9",        "Keypad 9", true },
            { GLFW_KEY_KP_ADD,           "keypad.add",      "Keypad +", true },
            { GLFW_KEY_KP_DECIMAL,       "keypad.decimal",  "Keypad .", true },
            { GLFW_KEY_KP_ENTER,         "keypad.enter",    "Keypad Enter", true },
            { GLFW_KEY_KP_EQUAL,         "keypad.equal",    "Keypad =", true },
            { GLFW_KEY_KP_MULTIPLY,      "keypad.multiply", "Keypad *", true },
            { GLFW_KEY_KP_DIVIDE,        "keypad.divide",   "Keypad /", true },
            { GLFW_KEY_KP_SUBTRACT,      "keypad.subtract", "Keypad -", true },
            { GLFW_KEY_APOSTROPHE,       "apostrophe",      "'", true },
            { GLFW_KEY_BACKSLASH,        "backslash",       "\\", true },
            { GLFW_KEY_COMMA,            "comma",           ",", true },
            { GLFW_KEY_EQUAL,            "equal",           "=", true },
            { GLFW_KEY_LEFT_BRACKET,     "left.bracket",    "[", true },
            { GLFW_KEY_MINUS,            "minus",           "-", true },
            { GLFW_KEY_PERIOD,           "period",          ".", true },
            { GLFW_KEY_RIGHT_BRACKET,    "right.bracket",   "]", true },
            { GLFW_KEY_SEMICOLON,        "semicolon",       ";", true },
            { GLFW_KEY_SLASH,            "slash",           "/", true },
            { GLFW_KEY_PAUSE,            "pause",           "Pause", true },
            { GLFW_KEY_SCROLL_LOCK,      "scroll.lock",     "Scroll Lock", true },
            { GLFW_KEY_MENU,             "menu",            "Menu", true },
            { GLFW_KEY_PRINT_SCREEN,     "print.screen",    "Print Screen", true },
            { GLFW_KEY_WORLD_1,          "world.1",         "World 1", true },
            { GLFW_KEY_WORLD_2,          "world.2",         "World 2", true },
        };

        const NamedKey* FindNamed(int code) {
            for (const auto& n : kNamedKeys) if (n.code == code) return &n;
            return nullptr;
        }
        const NamedKey* FindNamedByName(std::string_view name) {
            for (const auto& n : kNamedKeys) if (name == n.name) return &n;
            return nullptr;
        }

        struct NamedButton { int code; const char* name; const char* display; };
        const NamedButton kNamedButtons[] = {
            { GLFW_MOUSE_BUTTON_LEFT,   "left",   "Left Button"   },
            { GLFW_MOUSE_BUTTON_RIGHT,  "right",  "Right Button"  },
            { GLFW_MOUSE_BUTTON_MIDDLE, "middle", "Middle Button" },
        };

        KeyMapping& Register(const char* id, const char* category, const char* label,
                             BoundKey defaultKey) {
            s_storage.push_back(KeyMapping{ id, category, label, defaultKey, defaultKey, false, 0 });
            s_ordered.push_back(&s_storage.back());
            return s_storage.back();
        }

    } // namespace

    // ── BoundKey ───────────────────────────────────────────────────────────

    std::string BoundKey::Name() const {
        switch (type) {
            case Type::Mouse: {
                for (const auto& b : kNamedButtons) {
                    if (b.code == code) return std::string("key.mouse.") + b.name;
                }
                return "key.mouse." + std::to_string(code);
            }
            case Type::Keyboard: {
                // Table ONLY. glfwGetKeyName must never reach the persisted
                // form — it is layout-dependent, which is the whole bug this
                // table exists to prevent. MC's InputConstants is likewise a
                // static table.
                if (const NamedKey* n = FindNamed(code)) {
                    return std::string("key.keyboard.") + n->name;
                }
                // Anything MC does not name (rare/unknown scancodes) falls back
                // to the numeric code, which round-trips exactly.
                return "key.keyboard." + std::to_string(code);
            }
            default:
                return "key.keyboard.unknown";
        }
    }

    BoundKey BoundKey::FromName(std::string_view name) {
        constexpr std::string_view kMouse = "key.mouse.";
        constexpr std::string_view kKeyb  = "key.keyboard.";

        if (name.rfind(kMouse, 0) == 0) {
            const std::string_view suffix = name.substr(kMouse.size());
            for (const auto& b : kNamedButtons) {
                if (suffix == b.name) return Mouse(b.code);
            }
            try { return Mouse(std::stoi(std::string(suffix))); } catch (...) { return Unbound(); }
        }

        if (name.rfind(kKeyb, 0) == 0) {
            const std::string_view suffix = name.substr(kKeyb.size());
            if (suffix == "unknown") return Unbound();
            if (const NamedKey* n = FindNamedByName(suffix)) return Keyboard(n->code);

            // LEGACY: configs written before the table covered printable keys
            // stored the layout glyph ("key.keyboard./"). Resolve those through
            // glfwGetKeyName so nobody loses their bindings on upgrade; the
            // next SaveKeyBindings rewrites them in the stable form.
            //
            // This is exactly the lookup that could fail across layouts, which
            // is why it is now a fallback rather than the primary path.
            for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k) {
                if (const char* printable = glfwGetKeyName(k, 0)) {
                    if (suffix == printable) return Keyboard(k);
                }
            }
            try { return Keyboard(std::stoi(std::string(suffix))); } catch (...) { return Unbound(); }
        }

        return Unbound();
    }

    std::string BoundKey::DisplayName() const {
        switch (type) {
            case Type::Mouse: {
                for (const auto& b : kNamedButtons) if (b.code == code) return b.display;
                return "Button " + std::to_string(code + 1);
            }
            case Type::Keyboard: {
                // DISPLAY is where the layout SHOULD win — MC's
                // InputConstants.Key display name is glfwGetKeyName when it
                // returns something, and the translated fallback otherwise. So
                // a French layout shows the glyph actually printed on the key.
                const NamedKey* n = FindNamed(code);
                if (!n || n->preferGlfwName) {
                    if (const char* printable = glfwGetKeyName(code, 0)) {
                        std::string s(printable);
                        // GLFW hands back lowercase; MC shows key caps uppercased.
                        for (char& c : s) c = static_cast<char>(::toupper((unsigned char)c));
                        return s;
                    }
                }
                if (n) return n->display;
                return "Key " + std::to_string(code);
            }
            default:
                return "Not Bound";
        }
    }

    // ── Registry ───────────────────────────────────────────────────────────

    namespace Binds {
        KeyMapping* Forward = nullptr;
        KeyMapping* Back = nullptr;
        KeyMapping* Left = nullptr;
        KeyMapping* Right = nullptr;
        KeyMapping* Jump = nullptr;
        KeyMapping* Sneak = nullptr;
        KeyMapping* Sprint = nullptr;
        KeyMapping* Attack = nullptr;
        KeyMapping* Use = nullptr;
        KeyMapping* PickItem = nullptr;
        KeyMapping* Drop = nullptr;
        KeyMapping* SwapOffhand = nullptr;
        KeyMapping* Inventory = nullptr;
        KeyMapping* Chat = nullptr;
        KeyMapping* Command = nullptr;
        KeyMapping* Hotbar[9] = {};
        KeyMapping* TogglePerspective = nullptr;
        KeyMapping* Fullscreen = nullptr;
        KeyMapping* ToggleCursor = nullptr;
        KeyMapping* Noclip = nullptr;
        KeyMapping* LogConsole = nullptr;
    }

    void InitKeyMappings() {
        if (!s_ordered.empty()) return;   // already registered

        // Ids and defaults match vanilla so an options.txt written by either
        // is readable by the other. Categories drive the Controls screen's
        // grouping, in MC's order.
        Binds::Forward = &Register("key.forward", "Movement", "Walk Forwards",
                                   BoundKey::Keyboard(GLFW_KEY_W));
        Binds::Left    = &Register("key.left",    "Movement", "Strafe Left",
                                   BoundKey::Keyboard(GLFW_KEY_A));
        Binds::Back    = &Register("key.back",    "Movement", "Walk Backwards",
                                   BoundKey::Keyboard(GLFW_KEY_S));
        Binds::Right   = &Register("key.right",   "Movement", "Strafe Right",
                                   BoundKey::Keyboard(GLFW_KEY_D));
        Binds::Jump    = &Register("key.jump",    "Movement", "Jump",
                                   BoundKey::Keyboard(GLFW_KEY_SPACE));
        Binds::Sneak   = &Register("key.sneak",   "Movement", "Sneak",
                                   BoundKey::Keyboard(GLFW_KEY_LEFT_SHIFT));
        Binds::Sprint  = &Register("key.sprint",  "Movement", "Sprint",
                                   BoundKey::Keyboard(GLFW_KEY_LEFT_CONTROL));

        Binds::Attack      = &Register("key.attack",      "Gameplay", "Attack/Destroy",
                                       BoundKey::Mouse(GLFW_MOUSE_BUTTON_LEFT));
        Binds::Use         = &Register("key.use",         "Gameplay", "Use Item/Place Block",
                                       BoundKey::Mouse(GLFW_MOUSE_BUTTON_RIGHT));
        Binds::PickItem    = &Register("key.pickItem",    "Gameplay", "Pick Block",
                                       BoundKey::Keyboard(GLFW_KEY_P));
        Binds::Drop        = &Register("key.drop",        "Gameplay", "Drop Selected Item",
                                       BoundKey::Keyboard(GLFW_KEY_Q));
        Binds::SwapOffhand = &Register("key.swapOffhand", "Gameplay", "Swap Item With Offhand",
                                       BoundKey::Keyboard(GLFW_KEY_F));

        Binds::Inventory = &Register("key.inventory", "Inventory", "Open/Close Inventory",
                                     BoundKey::Keyboard(GLFW_KEY_E));
        static const int kHotbarKeys[9] = {
            GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4, GLFW_KEY_5,
            GLFW_KEY_6, GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9
        };
        for (int i = 0; i < 9; ++i) {
            static std::string ids[9], labels[9];
            ids[i]    = "key.hotbar." + std::to_string(i + 1);
            labels[i] = "Hotbar Slot " + std::to_string(i + 1);
            Binds::Hotbar[i] = &Register(ids[i].c_str(), "Inventory", labels[i].c_str(),
                                         BoundKey::Keyboard(kHotbarKeys[i]));
        }

        Binds::Chat    = &Register("key.chat",    "Multiplayer", "Open Chat",
                                   BoundKey::Keyboard(GLFW_KEY_T));
        Binds::Command = &Register("key.command", "Multiplayer", "Open Command",
                                   BoundKey::Keyboard(GLFW_KEY_SLASH));

        Binds::TogglePerspective = &Register("key.togglePerspective", "Miscellaneous",
                                             "Toggle Perspective",
                                             BoundKey::Keyboard(GLFW_KEY_F5));
        Binds::Fullscreen        = &Register("key.fullscreen", "Miscellaneous", "Toggle Fullscreen",
                                             BoundKey::Keyboard(GLFW_KEY_F11));
        // Engine-specific actions with no vanilla counterpart; still rebindable.
        Binds::ToggleCursor = &Register("key.toggleCursor", "Miscellaneous", "Toggle Cursor",
                                        BoundKey::Keyboard(GLFW_KEY_TAB));
        Binds::Noclip       = &Register("key.noclip",       "Miscellaneous", "Toggle Noclip",
                                        BoundKey::Keyboard(GLFW_KEY_N));
        Binds::LogConsole   = &Register("key.logConsole",   "Miscellaneous", "Toggle Log Console",
                                        BoundKey::Keyboard(GLFW_KEY_GRAVE_ACCENT));

        Log::Info("Key mappings registered: %zu bindings", s_ordered.size());
    }

    const std::vector<KeyMapping*>& AllKeyMappings() { return s_ordered; }

    KeyMapping* FindKeyMapping(std::string_view id) {
        for (KeyMapping* m : s_ordered) if (m->id == id) return m;
        return nullptr;
    }

    void SetKeyBinding(KeyMapping& mapping, BoundKey key) {
        mapping.key = key;
        // Rebinding mid-press would otherwise strand the old key's down state
        // on the mapping forever (nothing will ever deliver its release).
        mapping.down = false;
        mapping.clickCount = 0;
    }

    void ResetAllKeyBindings() {
        for (KeyMapping* m : s_ordered) SetKeyBinding(*m, m->defaultKey);
    }

    bool HasBindingConflict(const KeyMapping& mapping) {
        if (!mapping.key.IsBound()) return false;
        for (const KeyMapping* other : s_ordered) {
            if (other == &mapping) continue;
            if (other->key == mapping.key) return true;
        }
        return false;
    }

    void LoadKeyBindings() {
        auto& settings = Platform::g_gameSettings;
        for (KeyMapping* m : s_ordered) {
            const std::string stored = settings.GetString("key_" + m->id, "");
            if (stored.empty()) continue;          // never rebound; keep the default
            m->key = BoundKey::FromName(stored);
        }
    }

    void SaveKeyBindings() {
        auto& settings = Platform::g_gameSettings;
        for (const KeyMapping* m : s_ordered) {
            settings.SetString("key_" + m->id, m->key.Name());
        }
    }

    bool IsDown(const KeyMapping& mapping) { return mapping.down; }

    bool ConsumeClick(KeyMapping& mapping) {
        if (mapping.clickCount == 0) return false;
        --mapping.clickCount;
        return true;
    }

} // namespace Input
