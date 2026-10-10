// File: src/client/input/VeinMineClient.hpp
//
// The client's half of vein mining (Game::VeinMine has the shapes):
//
//   • the chosen shape and size, kept in options.txt;
//   • the shape key (Input::Binds::VeinMineShape): tapped, it cycles the
//     shape; held — or while Sneak + Vein Mine are held with a sized shape —
//     the size keys change height (taller / shorter) and width (wider /
//     narrower) and the wheel the length — the dug block stays the middle,
//     an even size growing up / right;
//   • the highlight: while Sneak and the Vein Mine key are held with a block
//     under the crosshair, the blocks the dig would take are outlined as
//     one shape (the creases of their union, in light blue) — the same
//     Game::VeinMine::CollectTargets the server runs;
//   • the release edge: letting go of the keys tells the server to stop any
//     vein mine still spreading (BlockActionType::VEIN_MINE_RELEASE).
//
// Main thread only.
#pragma once

#include "common/physics/RayCast.hpp"
#include "common/world/level/VeinMine.hpp"

#include <optional>
#include <string>

namespace Network { struct BlockActionC2SPacket; }
namespace Game { struct ItemStack; }

namespace Client::VeinMineClient {

    // The shape and size, loaded from options.txt on first use.
    const Game::VeinMine::Settings& Current();

    // Sneak and the Vein Mine key both held (no screen open).
    bool KeysHeld();

    // Per frame, in gameplay: the shape key and the size keys. True when the
    // shape or size changed — StatusText() is then worth showing.
    bool HandleKeys();
    // The size keys and the wheel resize: the shape key is held, or Sneak +
    // Vein Mine are with a sized shape. The wheel is then the length
    // (OnScroll), not the hotbar, and the arrows are not the orbit camera's.
    bool ResizeKeysActive();
    // A wheel step while ResizeKeysActive; true when the length changed.
    bool OnScroll(int wheel);
    // "Vein Mine: Tunnel 3x3, 5 long" — for the action bar.
    std::string StatusText();

    // Per frame: true once when the keys go from held to released.
    bool TakeReleaseEdge();

    // The dig's facing for the stairs: the player's horizontal facing along
    // the ray that found the block (portal-aware), North when it has none.
    Game::Direction FacingFor(const Game::RaycastHit& hit);
    // The vein-mine fields of a finishing dig's packet.
    void FillPacket(Network::BlockActionC2SPacket& packet, Game::Direction facing);

    // Queues the highlight (Render::Gizmos) for `hit` when the keys are held.
    // `harvestTool` is the held stack in survival / adventure — only blocks
    // it can harvest are taken (Game::VeinMine::CollectTargets) — and null
    // in creative. The caller flushes the gizmos.
    void QueueHighlight(const Game::RaycastHit& hit, const Game::ItemStack* harvestTool);

} // namespace Client::VeinMineClient
