// File: src/client/input/KeyMapping.hpp
//
// Port of MC's KeyMapping + InputConstants.Key: named, rebindable actions with
// a click queue.
//
// Two things this buys over reading physical keys directly:
//   • Rebinding. Game code asks "is the JUMP action down", never "is SPACE
//     down", so the Controls screen can point JUMP at any key.
//   • Correct press semantics. A press is an EVENT that increments clickCount
//     (MC KeyMapping.click); gameplay drains discrete presses with
//     ConsumeClick and reads held state with IsDown. Nothing infers a press by
//     diffing a polled level, which is what let a click that dismissed a
//     screen carry into the world.
//
// Several mappings may share one physical key — that is how MC represents a
// conflict, and why events dispatch to ALL mappings bound to a key
// (KeyMapping.forAllKeyMappings).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Input {

    // MC InputConstants.Key — one physical input, tagged with its device.
    struct BoundKey {
        enum class Type : uint8_t { Unbound, Keyboard, Mouse };

        Type type = Type::Unbound;
        int  code = -1;          // GLFW key code, or GLFW mouse button

        static BoundKey Keyboard(int glfwKey) { return { Type::Keyboard, glfwKey }; }
        static BoundKey Mouse(int glfwButton) { return { Type::Mouse, glfwButton }; }
        static BoundKey Unbound()             { return {}; }

        bool IsBound() const { return type != Type::Unbound; }
        bool operator==(const BoundKey& o) const { return type == o.type && code == o.code; }
        bool operator!=(const BoundKey& o) const { return !(*this == o); }

        // Stable identifier for options.txt, in MC's format:
        // "key.keyboard.w", "key.mouse.left", "key.keyboard.unknown".
        std::string Name() const;
        static BoundKey FromName(std::string_view name);

        // Human label for the Controls screen ("W", "Left Button", "Space").
        std::string DisplayName() const;
    };

    // MC KeyMapping. `id` doubles as the options.txt key (prefixed with "key_")
    // and as the lookup name.
    struct KeyMapping {
        std::string id;        // "key.attack"
        std::string category;  // "Gameplay", "Movement", …
        std::string label;     // "Attack/Destroy"
        BoundKey    defaultKey;
        BoundKey    key;

        // Live state, written only by the input callbacks and only while no UI
        // is active — see Input::SetUiActive.
        bool down = false;
        int  clickCount = 0;

        bool IsDefault() const { return key == defaultKey; }
    };

    // Registers every mapping with its vanilla default. Call once, before
    // bindings are loaded from disk.
    void InitKeyMappings();

    // All mappings in display order (grouped by category, as MC lists them).
    const std::vector<KeyMapping*>& AllKeyMappings();

    KeyMapping* FindKeyMapping(std::string_view id);

    // Rebinding. Passing an unbound key clears the binding (MC binds ESC to
    // "unset" in the Controls screen).
    void SetKeyBinding(KeyMapping& mapping, BoundKey key);
    void ResetAllKeyBindings();

    // True when another mapping is bound to the same physical key. MC paints
    // conflicting rows red and shows a tooltip listing the clashes.
    bool HasBindingConflict(const KeyMapping& mapping);

    // options.txt persistence, in vanilla's shape: one `key_<id>` entry per
    // mapping whose value is the MC key name, e.g. `key_key.attack:key.mouse.left`.
    // Absent entries keep the default, so a fresh options.txt needs nothing.
    void LoadKeyBindings();
    void SaveKeyBindings();

    // Per-action state, mirroring KeyMapping.isDown / consumeClick.
    bool IsDown(const KeyMapping& mapping);
    bool ConsumeClick(KeyMapping& mapping);

    // Named handles for the actions the game consumes, mirroring MC's
    // Options.keyUp / keyAttack / … fields. Valid after InitKeyMappings().
    namespace Binds {
        extern KeyMapping* Forward;
        extern KeyMapping* Back;
        extern KeyMapping* Left;
        extern KeyMapping* Right;
        extern KeyMapping* Jump;
        extern KeyMapping* Sneak;
        extern KeyMapping* Sprint;

        extern KeyMapping* Attack;
        extern KeyMapping* Use;
        extern KeyMapping* PickItem;
        extern KeyMapping* Drop;
        extern KeyMapping* SwapOffhand;

        extern KeyMapping* Inventory;
        extern KeyMapping* Chat;
        extern KeyMapping* Command;
        extern KeyMapping* Hotbar[9];

        extern KeyMapping* TogglePerspective;
        extern KeyMapping* Fullscreen;
        extern KeyMapping* ToggleCursor;
        extern KeyMapping* Noclip;
        extern KeyMapping* LogConsole;
    }

} // namespace Input
