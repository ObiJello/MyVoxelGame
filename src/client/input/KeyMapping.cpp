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

        // GLFW code -> MC-style suffix, for the non-printable keys. Printable
        // ones round-trip through glfwGetKeyName, matching how MC derives most
        // of its key.keyboard.* names from the platform.
        struct NamedKey { int code; const char* name; const char* display; };
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
                if (const NamedKey* n = FindNamed(code)) {
                    return std::string("key.keyboard.") + n->name;
                }
                if (const char* printable = glfwGetKeyName(code, 0)) {
                    return std::string("key.keyboard.") + printable;
                }
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
            // Printable keys: ask GLFW for each candidate's name and match.
            // Cheaper than it looks — it runs once per binding at load.
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
                if (const NamedKey* n = FindNamed(code)) return n->display;
                if (const char* printable = glfwGetKeyName(code, 0)) {
                    std::string s(printable);
                    // GLFW hands back lowercase; MC shows key caps uppercased.
                    for (char& c : s) c = static_cast<char>(::toupper((unsigned char)c));
                    return s;
                }
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
