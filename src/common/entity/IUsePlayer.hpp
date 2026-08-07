// File: src/common/entity/IUsePlayer.hpp
//
// The slice of "player" that item-use behaviours actually touch. Exists for
// the same reason as ILevelWrite: the behaviours are common code but took a
// Server::ServerPlayer*, so they could only ever run on the server and a
// remote client had to wait a round trip to see the result.
//
// Implementations: Server::ServerPlayer (authority) and
// Client::ClientUsePlayer (prediction).
//
// Kept deliberately narrow — every method here is one an item behaviour
// genuinely calls. Adding to it means widening what the client must be able
// to simulate, so prefer gating a behaviour out of prediction over growing
// this interface.
#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace Game {

    struct ItemStack;

    class IUsePlayer {
    public:
        virtual ~IUsePlayer() = default;

        // Eye-ray inputs — the bucket runs its own POV clip (BucketItem's
        // ClipContext) rather than reusing the click's hit result.
        // Returned by const-ref to match ServerPlayer's existing signature so
        // it satisfies this interface without a shim.
        virtual const glm::dvec3& getPosition() const = 0;
        virtual float             getYaw()      const = 0;
        virtual float             getPitch()    const = 0;

        // Creative skips stack consumption / durability.
        // (A bool rather than the Server::GameMode enum so common code needn't
        // depend on the server's player header.)
        virtual bool isCreative() const = 0;

        virtual bool IsSneaking() const = 0;

        // hand: 0 = main, 1 = off.
        virtual ItemStack& getItemInHand(uint32_t hand) = 0;
        virtual int        handSlotIndex(uint32_t hand) const = 0;

        // Queues a slot for re-broadcast. No-op on the client, whose slot
        // state is already local and will be corrected by the server's
        // InventorySetSlotS2C if it disagrees.
        virtual void markSlotDirty(int slotIndex) = 0;
    };

} // namespace Game
