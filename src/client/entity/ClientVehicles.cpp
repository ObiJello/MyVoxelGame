// File: src/client/entity/ClientVehicles.cpp
#include "client/entity/ClientVehicles.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/entity/Player.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/input/Input.hpp"
#include "client/input/KeyMapping.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/SoundManager.hpp"

#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/PlayerRideable.hpp"
#include "common/entity/PlayerRideableJumping.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/decoration/BlockAttachedEntity.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/vehicle/Boat.hpp"
#include "common/entity/vehicle/Minecart.hpp"
#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/VehiclePackets.hpp"
#include "common/sound/SoundEvents.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

namespace Client::Vehicles {

    namespace {

        // The local player, for the riding sounds' "is the player still on
        // it / under water" (set each client tick; cleared by Reset).
        const Game::ClientPlayer* s_localPlayer = nullptr;
        uint32_t s_localPlayerId = 0;

        // What LocalPlayer.tick last sent (ServerboundPlayerInputPacket).
        bool    s_inputSent = false;
        uint8_t s_lastSentInput = 0;

        // The frame's partial tick (FrameUpdate) — the seat lerps with it.
        float s_partialTick = 1.0f;

        // The connection, once it is in the PLAY phase — the riding packets
        // are gameplay packets (MC's LocalPlayer, which sends them, does not
        // exist before ClientboundLoginPacket).
        ClientConnection* Connection() {
            if (!g_networkClient || !g_networkClient->IsConnected()) return nullptr;
            auto conn = g_networkClient->GetConnection();
            return conn && conn->IsInPlayPhase() ? conn.get() : nullptr;
        }

        Game::VehicleEntity* VehicleOf(const Game::Mob* mob) {
            if (!mob || !Game::IsVehicleEntityType(mob->GetType())) return nullptr;
            return static_cast<Game::VehicleEntity*>(const_cast<Game::Mob*>(mob));
        }

        ClientMob* EntryOf(int32_t id) {
            if (!g_clientMobManager || id == 0) return nullptr;
            return const_cast<ClientMob*>(g_clientMobManager->GetMob(id));
        }

        // The vehicle the local player drives (a boat whose front seat is
        // theirs), or null.
        Game::VehicleEntity* DrivenVehicle(const Game::ClientPlayer& player) {
            if (player.vehicleId == 0) return nullptr;
            ClientMob* entry = EntryOf(player.vehicleId);
            Game::VehicleEntity* vehicle = entry ? VehicleOf(entry->mob.get()) : nullptr;
            return vehicle && vehicle->IsLocalInstanceAuthoritative() ? vehicle : nullptr;
        }

        // The mob the local player steers (a saddled horse, a pig on a
        // carrot, a harnessed happy ghast), or null.
        Game::LivingEntity* SteeredMount(const Game::ClientPlayer& player) {
            if (player.vehicleId == 0) return nullptr;
            ClientMob* entry = EntryOf(player.vehicleId);
            Game::Mob* mob = entry ? entry->mob.get() : nullptr;
            if (!mob || Game::IsVehicleEntityType(mob->GetType())) return nullptr;
            return mob->IsLocallySteered() ? mob : nullptr;
        }

        // The LIVING mob the local player rides (steered or not), or null —
        // not a boat or cart, nor a cushion (plain Entities in MC).
        Game::LivingEntity* RiddenMount(const Game::ClientPlayer& player) {
            if (player.vehicleId == 0) return nullptr;
            ClientMob* entry = EntryOf(player.vehicleId);
            Game::Mob* mob = entry ? entry->mob.get() : nullptr;
            if (!mob || Game::IsVehicleEntityType(mob->GetType())) return nullptr;
            if (dynamic_cast<Game::BlockAttachedEntity*>(mob)) return nullptr;
            return mob;
        }

        // MC LocalPlayer.jumpableVehicle: the controlled vehicle, when it is
        // PlayerRideableJumping and canJump.
        Game::PlayerRideableJumping* JumpableVehicle(const Game::ClientPlayer& player) {
            Game::LivingEntity* mount = SteeredMount(player);
            auto* jumping = mount ? dynamic_cast<Game::PlayerRideableJumping*>(mount) : nullptr;
            return jumping && jumping->CanJump() ? jumping : nullptr;
        }

        // A boat just boarded: the view's yaw to snap to on the next frame.
        bool  s_pendingYawSnap = false;
        float s_pendingYaw = 0.0f;

        // The view the camera carries (the rider's yRot / xRot) — kept from
        // the last frame / tick that handed it in.
        float s_viewYaw = 0.0f;
        float s_viewPitch = 0.0f;

        // MC LocalPlayer.jumpRidingTicks / jumpRidingScale and the jump key
        // as last seen (wasJumping).
        int   s_jumpRidingTicks = 0;
        float s_jumpRidingScale = 0.0f;
        bool  s_wasJumping = false;

        // The keys as MC's Input record (the same gate the player's own
        // movement reads them through).
        Game::PlayerInput CurrentInput() {
            Game::PlayerInput in;
            in.forward  = Input::Binds::Forward && Input::IsDown(*Input::Binds::Forward);
            in.backward = Input::Binds::Back && Input::IsDown(*Input::Binds::Back);
            in.left     = Input::Binds::Left && Input::IsDown(*Input::Binds::Left);
            in.right    = Input::Binds::Right && Input::IsDown(*Input::Binds::Right);
            in.jump     = Input::Binds::Jump && Input::IsDown(*Input::Binds::Jump);
            in.shift    = Input::Binds::Sneak && Input::IsDown(*Input::Binds::Sneak);
            in.sprint   = Input::Binds::Sprint && Input::IsDown(*Input::Binds::Sprint);
            return in;
        }

        void SendMoveVehicle(const Game::Entity& vehicle) {
            ClientConnection* conn = Connection();
            if (!conn) return;
            Network::MoveVehicleC2SPacket packet;
            packet.position = vehicle.position;
            packet.yRot = vehicle.yRot;
            packet.xRot = vehicle.xRot;
            packet.onGround = vehicle.onGround;
            conn->SendPacket(static_cast<uint8_t>(Network::PacketId::MoveVehicleC2S),
                             Network::Serialization::Serialize(packet));
        }

        // The vehicle's interpolated pose this frame (what its renderer
        // draws).
        void InterpolatedPose(const ClientMob& entry, float partial, glm::dvec3& pos, float& yRot) {
            const Game::Mob& mob = *entry.mob;
            pos = glm::mix(entry.renderPrevPosition, mob.position, static_cast<double>(partial));
            yRot = Game::Mth::RotLerp(partial, entry.renderPrevYRot, mob.yRot);
        }

        // The client's view of the player in a mount's first seat — only its
        // own: the keys, the camera's view, the held items.
        bool ClientRiderControl(const Game::LivingEntity& mount, Game::LivingEntity::RiderControl& out) {
            const Game::ClientPlayer* player = s_localPlayer;
            if (!player || !mount.LocalPlayerRidesFirst() || player->vehicleId != mount.GetId()) return false;
            const Game::PlayerInput in = CurrentInput();
            float x = (in.left ? 1.0f : 0.0f) - (in.right ? 1.0f : 0.0f);
            float z = (in.forward ? 1.0f : 0.0f) - (in.backward ? 1.0f : 0.0f);
            const float len = std::sqrt(x * x + z * z);
            if (len < 1.0e-4f) { x = 0.0f; z = 0.0f; } else { x /= len; z /= len; }
            out.xxa = x;
            out.zza = z;
            out.jumping = in.jump;
            out.sprinting = in.sprint;
            out.shift = in.shift;
            out.yRot = s_viewYaw;
            out.xRot = s_viewPitch;
            out.mainHandItem = player->inventory.GetSlot(
                Game::Inventory::HotbarToIndex(player->inventory.GetSelectedSlot())).itemId;
            out.offHandItem = player->inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN).itemId;
            out.playerId = static_cast<int32_t>(s_localPlayerId);
            out.local = true;
            return true;
        }

        // ── Sounds ─────────────────────────────────────────────────────────

        // MC MinecartSoundInstance: the rolling loop every cart carries,
        // louder with its horizontal speed. (The class's own `pitch` field
        // shadows the sound's, so the loop plays at its base pitch.)
        class MinecartSoundInstance final : public AbstractTickableSoundInstance {
        public:
            MinecartSoundInstance(int32_t cartId, const glm::dvec3& at)
                : AbstractTickableSoundInstance(Game::SoundEvents::MINECART_RIDING, Game::SoundSource::Neutral,
                                                SoundInstance::UnseededSeed()),
                  m_cartId(cartId) {
                m_looping = true;
                m_delay = 0;
                m_volume = 0.0f;
                m_x = static_cast<double>(static_cast<float>(at.x));
                m_y = static_cast<double>(static_cast<float>(at.y));
                m_z = static_cast<double>(static_cast<float>(at.z));
            }
            bool CanPlaySound() const override {
                const ClientMob* entry = g_clientMobManager ? g_clientMobManager->GetMob(m_cartId) : nullptr;
                return entry && entry->mob && !entry->mob->IsSilent();
            }
            bool CanStartSilent() const override { return true; }
            void Tick() override {
                const ClientMob* entry = g_clientMobManager ? g_clientMobManager->GetMob(m_cartId) : nullptr;
                if (!entry || !entry->mob || entry->mob->IsRemoved()) {
                    Stop();
                    return;
                }
                const Game::Mob& cart = *entry->mob;
                m_x = static_cast<double>(static_cast<float>(cart.position.x));
                m_y = static_cast<double>(static_cast<float>(cart.position.y));
                m_z = static_cast<double>(static_cast<float>(cart.position.z));
                const float speed = static_cast<float>(std::sqrt(cart.velocity.x * cart.velocity.x +
                                                                 cart.velocity.z * cart.velocity.z));
                if (speed >= 0.01f) {
                    m_volume = Game::Mth::Lerp(std::clamp(speed, 0.0f, 0.5f), 0.0f, 0.7f);
                } else {
                    m_volume = 0.0f;
                }
            }
        private:
            int32_t m_cartId;
        };

        // MC RidingEntitySoundInstance: a loop heard only by the rider (no
        // attenuation) while it stays on `entityId`, silent at volumeMin
        // below 0.01 blocks a tick, else amplifier * clampedLerp(speed, min,
        // max) of the mount's whole motion; one instance is for air, one
        // for under water, and each keeps quiet in the other's medium. The
        // happy ghast's harness wind (HAPPY_GHAST_RIDING).
        class RidingEntitySoundInstance final : public AbstractTickableSoundInstance {
        public:
            RidingEntitySoundInstance(int32_t entityId, const glm::dvec3& at, bool underwaterSound,
                                      const char* event, Game::SoundSource source, float volumeMin,
                                      float volumeMax, float amplifier)
                : AbstractTickableSoundInstance(event, source, SoundInstance::UnseededSeed()),
                  m_entityId(entityId), m_underwaterSound(underwaterSound),
                  m_volumeMin(volumeMin), m_volumeMax(volumeMax), m_amplifier(amplifier) {
                m_x = at.x; m_y = at.y; m_z = at.z;
                m_attenuation = Attenuation::None;
                m_looping = true;
                m_delay = 0;
                m_volume = volumeMin;
            }
            bool CanPlaySound() const override {
                const ClientMob* entry = g_clientMobManager ? g_clientMobManager->GetMob(m_entityId) : nullptr;
                return entry && entry->mob && !entry->mob->IsSilent();
            }
            bool CanStartSilent() const override { return true; }
            void Tick() override {
                const ClientMob* entry = g_clientMobManager ? g_clientMobManager->GetMob(m_entityId) : nullptr;
                const Game::ClientPlayer* player = s_localPlayer;
                if (!entry || !entry->mob || entry->mob->IsRemoved() || !player ||
                    player->vehicleId != m_entityId) {
                    Stop();
                    return;
                }
                const Game::Mob& mount = *entry->mob;
                // shouldNotPlayUnderwaterSound: the MOUNT's medium.
                if (m_underwaterSound != mount.IsUnderWater()) {
                    m_volume = m_volumeMin;
                    return;
                }
                const float speed = static_cast<float>(glm::length(mount.velocity));
                if (speed >= 0.01f) {
                    // Mth.clampedLerp(speed, min, max).
                    const float t = std::clamp(speed, 0.0f, 1.0f);
                    m_volume = m_amplifier * (m_volumeMin + t * (m_volumeMax - m_volumeMin));
                } else {
                    m_volume = m_volumeMin;
                }
                m_x = mount.position.x;
                m_y = mount.position.y;
                m_z = mount.position.z;
            }
        private:
            int32_t m_entityId;
            bool    m_underwaterSound;
            float   m_volumeMin, m_volumeMax, m_amplifier;
        };

        // MC RidingMinecartSoundInstance (over RidingEntitySoundInstance):
        // heard only by the rider, one loop for dry air and one for under
        // water, as the cart's horizontal speed.
        class RidingMinecartSoundInstance final : public AbstractTickableSoundInstance {
        public:
            RidingMinecartSoundInstance(int32_t cartId, const glm::dvec3& at, bool underwaterSound,
                                        const char* event, float volumeMin, float volumeMax, float amplifier)
                : AbstractTickableSoundInstance(event, Game::SoundSource::Neutral, SoundInstance::UnseededSeed()),
                  m_cartId(cartId), m_underwaterSound(underwaterSound),
                  m_volumeMin(volumeMin), m_volumeMax(volumeMax), m_amplifier(amplifier) {
                m_x = at.x; m_y = at.y; m_z = at.z;
                m_attenuation = Attenuation::None;
                m_looping = true;
                m_delay = 0;
                m_volume = volumeMin;
            }
            bool CanPlaySound() const override {
                const ClientMob* entry = g_clientMobManager ? g_clientMobManager->GetMob(m_cartId) : nullptr;
                return entry && entry->mob && !entry->mob->IsSilent();
            }
            bool CanStartSilent() const override { return true; }
            void Tick() override {
                const ClientMob* entry = g_clientMobManager ? g_clientMobManager->GetMob(m_cartId) : nullptr;
                const Game::ClientPlayer* player = s_localPlayer;
                if (!entry || !entry->mob || entry->mob->IsRemoved() || !player ||
                    player->vehicleId != m_cartId) {
                    Stop();
                    return;
                }
                const Game::Mob& cart = *entry->mob;
                // shouldNotPlayUnderwaterSound: the player's eye, not the cart.
                if (m_underwaterSound != player->physics.isEyeInWater) {
                    m_volume = m_volumeMin;
                    return;
                }
                const float speed = static_cast<float>(std::sqrt(cart.velocity.x * cart.velocity.x +
                                                                 cart.velocity.z * cart.velocity.z));
                if (speed >= 0.01f) {
                    // Mth.clampedLerp(speed, min, max).
                    const float t = std::clamp(speed, 0.0f, 1.0f);
                    m_volume = m_amplifier * (m_volumeMin + t * (m_volumeMax - m_volumeMin));
                } else {
                    m_volume = m_volumeMin;
                }
                m_x = cart.position.x;
                m_y = cart.position.y;
                m_z = cart.position.z;
            }
        private:
            int32_t m_cartId;
            bool    m_underwaterSound;
            float   m_volumeMin, m_volumeMax, m_amplifier;
        };

    } // namespace

    // ── Packets ──────────────────────────────────────────────────────────────

    void OnSetPassengers(const Network::SetPassengersS2CPacket& packet, uint32_t localPlayerId) {
        s_localPlayerId = localPlayerId;
        ClientMob* entry = EntryOf(packet.vehicleId);
        if (!entry || !entry->mob) return;
        Game::VehicleEntity* vehicle = VehicleOf(entry->mob.get());
        if (!vehicle) {
            // A mob a player rides: the seat order, and whether this client
            // now steers it (then it simulates the mount — no server
            // interpolation left pending).
            Game::Mob& mount = *entry->mob;
            const bool wasSteered = mount.IsLocallySteered();
            mount.SetSyncedRiders(packet.passengerIds, static_cast<int32_t>(localPlayerId));
            for (int32_t id : packet.passengerIds) {
                if (!(id > 0 && id < Game::kItemEntityIdBase) && g_clientMobManager) {
                    g_clientMobManager->SetVehicle(id, packet.vehicleId);
                }
            }
            if (!wasSteered && mount.IsLocallySteered()) {
                entry->interpSteps = 0;
                entry->targetPosition = mount.position;
                entry->targetYRot = mount.yRot;
            }
            return;
        }
        const bool wasDriven = vehicle->IsLocalInstanceAuthoritative();
        vehicle->SetSyncedPassengers(packet.passengerIds, static_cast<int32_t>(localPlayerId));
        vehicle->SetLocalControlled(!packet.passengerIds.empty() &&
                                    packet.passengerIds.front() == static_cast<int32_t>(localPlayerId));
        // The mob riders ride here too (their own links follow on their
        // entity data; this puts them in the right order at once).
        for (int32_t id : packet.passengerIds) {
            if (!vehicle->IsPlayerPassengerId(id) && g_clientMobManager) {
                g_clientMobManager->SetVehicle(id, packet.vehicleId);
            }
        }
        // Taking the wheel: no server interpolation is left pending on a
        // vehicle this client now simulates; letting go: the server's next
        // move eases it from where the client left it.
        if (!wasDriven && vehicle->IsLocalInstanceAuthoritative()) {
            entry->interpSteps = 0;
            entry->targetPosition = vehicle->position;
            entry->targetYRot = vehicle->yRot;
        }
    }

    void OnMoveVehicle(const Network::MoveVehicleS2CPacket& packet) {
        // MC ClientPacketListener.handleMoveVehicle: the root vehicle the
        // local player drives is put where the server says, and the client
        // answers with where it now is.
        const Game::ClientPlayer* player = s_localPlayer;
        if (!player || player->vehicleId == 0) return;
        ClientMob* entry = EntryOf(player->vehicleId);
        Game::Mob* vehicle = entry ? entry->mob.get() : nullptr;
        if (!vehicle || !IsLocallyDriven(*vehicle)) return;
        // Every correction is a visible snap of the ride: name it (at most
        // once a second) so a rubber-banding vehicle shows its cause.
        {
            static auto s_lastLog = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now - s_lastLog > std::chrono::seconds(1)) {
                s_lastLog = now;
                const glm::dvec3 off = packet.position - vehicle->position;
                Log::Info("[Riding] server corrected %s #%d by %.3f blocks",
                          std::string(vehicle->TypeInfo().slug).c_str(), vehicle->GetId(), glm::length(off));
            }
        }
        vehicle->position = packet.position;
        vehicle->oldPosition = packet.position;
        vehicle->yRot = packet.yRot;
        vehicle->xRot = packet.xRot;
        entry->renderPrevPosition = packet.position;
        entry->renderPrevYRot = packet.yRot;
        entry->targetPosition = packet.position;
        entry->interpSteps = 0;
        SendMoveVehicle(*vehicle);
    }

    void OnVehicleData(const Network::VehicleDataS2CPacket& packet) {
        ClientMob* entry = EntryOf(packet.entityId);
        Game::VehicleEntity* vehicle = entry ? VehicleOf(entry->mob.get()) : nullptr;
        if (!vehicle) return;
        Game::VehicleSyncedData data;
        data.hurtTime = packet.hurtTime;
        data.hurtDir = packet.hurtDir;
        data.damage = packet.damage;
        data.paddleLeft = packet.paddleLeft;
        data.paddleRight = packet.paddleRight;
        data.paddleReverse = packet.paddleReverse;
        data.bubbleTime = packet.bubbleTime;
        data.hasCustomDisplay = packet.hasCustomDisplay;
        data.customDisplay = packet.customDisplay;
        data.displayOffset = packet.displayOffset;
        data.hasFuel = packet.hasFuel;
        vehicle->ApplySyncedData(data);
    }

    // ── Entity lifecycle ────────────────────────────────────────────────────

    void OnVehicleSpawned(Game::Mob& mob) {
        if (!Game::IsMinecartEntityType(mob.GetType())) return;
        GetSoundManager().Play(std::make_shared<MinecartSoundInstance>(mob.GetId(), mob.position));
    }

    void OnLocalMounted(int32_t vehicleId) {
        const ClientMob* entry = g_clientMobManager ? g_clientMobManager->GetMob(vehicleId) : nullptr;
        // MC handleSetEntityPassengersPacket: boarding a boat turns the view
        // to the boat's heading (applied on the next frame, FrameUpdate).
        if (entry && entry->mob && Game::IsBoatEntityType(entry->mob->GetType())) {
            s_pendingYawSnap = true;
            s_pendingYaw = entry->mob->yRot;
        }
        if (!entry || !entry->mob) return;
        // MC LocalPlayer.startRiding: the happy ghast's riding loop.
        if (entry->mob->GetType() == Game::EntityTypeId::HappyGhast) {
            GetSoundManager().Play(std::make_shared<RidingEntitySoundInstance>(
                vehicleId, entry->mob->position, false, Game::SoundEvents::HAPPY_GHAST_RIDING,
                entry->mob->GetSoundSource(), 0.0f, 1.0f, 5.0f));
            return;
        }
        if (!Game::IsMinecartEntityType(entry->mob->GetType())) return;
        const glm::dvec3 at = entry->mob->position;
        GetSoundManager().Play(std::make_shared<RidingMinecartSoundInstance>(
            vehicleId, at, true, Game::SoundEvents::MINECART_INSIDE_UNDERWATER, 0.0f, 0.75f, 1.0f));
        GetSoundManager().Play(std::make_shared<RidingMinecartSoundInstance>(
            vehicleId, at, false, Game::SoundEvents::MINECART_INSIDE, 0.0f, 0.75f, 1.0f));
    }

    bool IsLocallyDriven(const Game::Mob& mob) {
        if (const Game::VehicleEntity* vehicle = VehicleOf(&mob)) return vehicle->IsLocalInstanceAuthoritative();
        return !mob.SyncedRiders().empty() && mob.IsLocallySteered();
    }

    // ── The client tick ─────────────────────────────────────────────────────

    void BeforeEntityTick(Game::ClientPlayer& player, float viewYaw, float viewPitch) {
        s_localPlayer = &player;
        s_viewYaw = viewYaw;
        s_viewPitch = viewPitch;
        // MC LocalPlayer.aiStep: a boat passenger's keys become the boat's.
        if (auto* boat = dynamic_cast<Game::Boat*>(DrivenVehicle(player))) {
            const Game::PlayerInput in = CurrentInput();
            boat->SetInput(in.left, in.right, in.forward, in.backward);
        }

        // MC LocalPlayer.aiStep's riding jump: hold the jump key to charge
        // the bar, release to jump (the mount leaps here — this client
        // simulates it — and the server hears START_RIDING_JUMP).
        const bool jumpDown = CurrentInput().jump;
        const bool wasJumping = s_wasJumping;
        s_wasJumping = jumpDown;
        Game::PlayerRideableJumping* jumpable = JumpableVehicle(player);
        if (jumpable && jumpable->GetJumpCooldown() == 0) {
            if (s_jumpRidingTicks < 0) {
                ++s_jumpRidingTicks;
                if (s_jumpRidingTicks == 0) s_jumpRidingScale = 0.0f;
            }
            if (wasJumping && !jumpDown) {
                s_jumpRidingTicks = -10;
                const int amount = static_cast<int>(std::floor(s_jumpRidingScale * 100.0f));
                jumpable->OnPlayerJump(amount);
                if (ClientConnection* conn = Connection()) {
                    Network::RidingCommandC2SPacket command;
                    command.action = Network::RidingCommand::StartRidingJump;
                    command.data = amount;
                    conn->SendPacket(static_cast<uint8_t>(Network::PacketId::RidingCommandC2S),
                                     Network::Serialization::Serialize(command));
                }
            } else if (!wasJumping && jumpDown) {
                s_jumpRidingTicks = 0;
                s_jumpRidingScale = 0.0f;
            } else if (wasJumping) {
                ++s_jumpRidingTicks;
                if (s_jumpRidingTicks < 10) {
                    s_jumpRidingScale = static_cast<float>(s_jumpRidingTicks) * 0.1f;
                } else {
                    s_jumpRidingScale = 0.8f + 2.0f / static_cast<float>(s_jumpRidingTicks - 9) * 0.1f;
                }
            }
        } else {
            s_jumpRidingScale = 0.0f;
        }
    }

    bool GetHudState(const Game::ClientPlayer& player, int& vehicleHearts, int& vehicleHealth, bool& jumpable,
                     float& jumpScale, bool& jumpCooldown) {
        vehicleHearts = 0;
        vehicleHealth = 0;
        jumpable = false;
        jumpScale = 0.0f;
        jumpCooldown = false;
        // MC Hud.getPlayerVehicleWithHealth / getVehicleMaxHearts: a living
        // vehicle's max health in hearts, 30 at most.
        if (const Game::LivingEntity* mount = RiddenMount(player)) {
            const float maxHealth = mount->GetMaxHealth();
            vehicleHearts = std::min(30, static_cast<int>(maxHealth + 0.5f) / 2);
            vehicleHealth = static_cast<int>(std::ceil(mount->GetHealth()));
        }
        if (Game::PlayerRideableJumping* jumping = JumpableVehicle(player)) {
            jumpable = true;
            jumpScale = s_jumpRidingScale;
            jumpCooldown = jumping->GetJumpCooldown() > 0;
        }
        return vehicleHearts > 0 || jumpable;
    }

    namespace {
        // MC Entity.push(Entity) as it lands on the local player: the
        // larger horizontal axis as the distance, sqrt fall-off, capped, ×0.05
        // — away from `other`. Player velocity is blocks per second.
        void PushLocalPlayerAwayFrom(Game::ClientPlayer& player, const glm::dvec3& other) {
            double xa = player.physics.position.x - other.x;
            double za = player.physics.position.z - other.z;
            double dd = std::max(std::abs(xa), std::abs(za));
            if (dd < 0.009999999776482582) return;   // MC (double)0.01F
            dd = std::sqrt(dd);
            xa /= dd;
            za /= dd;
            const double pow = std::min(1.0, 1.0 / dd);
            xa *= pow * 0.05;
            za *= pow * 0.05;
            constexpr double kTicksPerSecond = 20.0;
            // Into the push's own deltaMovement (PlayerPhysics::
            // pushVelocity) — never the portal momentum in `velocity`.
            player.physics.pushVelocity.x += static_cast<float>(xa * kTicksPerSecond);
            player.physics.pushVelocity.z += static_cast<float>(za * kTicksPerSecond);
        }

        // MC's client half of entity pushing: on the client,
        // EntitySelector.pushableBy admits only the local player, so every
        // client mob's aiStep pushEntities shoves just it (doPush →
        // player.push(mob)), and a boat's tick does the same through
        // AbstractBoat.push (only a body at or below the hull's bottom).
        // A minecart's push is server-only (AbstractMinecart.push). The
        // server never moves the player for a push — its client does.
        void PushLocalPlayer(Game::ClientPlayer& player) {
            if (!g_clientMobManager || player.vehicleId != 0) return;
            // LivingEntity.isPushable: alive, not a spectator, not climbing.
            if (player.health <= 0 || player.IsSpectator() || player.physics.onClimbable || player.physics.noclip) return;
            const double half = player.physics.GetWidth() * 0.5;
            const Game::AABBd playerBox = Game::AABBd::FromMinMax(
                player.physics.position - glm::dvec3(half, 0.0, half),
                player.physics.position + glm::dvec3(half, player.physics.GetCurrentHeight(), half));
            for (const ClientMob* entry : g_clientMobManager->MobList()) {
                if (!entry || !entry->mob || entry->mob->IsRemoved()) continue;
                const Game::Mob& mob = *entry->mob;
                const Game::AABBd box = mob.GetAABBd();
                if (Game::IsBoatEntityType(mob.GetType())) {
                    // AbstractBoat.tick: the hull inflated (0.2, -0.01, 0.2);
                    // AbstractBoat.push: a non-boat only when its bottom is
                    // at or below the hull's.
                    const Game::AABBd reach = Game::AABBd::FromMinMax(box.min - glm::dvec3(0.2, -0.01, 0.2),
                                                          box.max + glm::dvec3(0.2, -0.01, 0.2));
                    if (!reach.Intersects(playerBox) || playerBox.min.y > box.min.y) continue;
                    PushLocalPlayerAwayFrom(player, mob.position);
                    continue;
                }
                if (Game::IsVehicleEntityType(mob.GetType())) continue;
                // A living body that pushes (armour stands, crystals, TNT and
                // the like report unpushable and have no pushEntities).
                if (!static_cast<const Game::Entity&>(mob).IsPushable()) continue;
                if (!box.Intersects(playerBox)) continue;
                PushLocalPlayerAwayFrom(player, mob.position);
            }
        }
    } // namespace

    void AfterEntityTick(Game::ClientPlayer& player) {
        s_localPlayer = &player;
        ClientConnection* conn = Connection();
        if (conn) s_localPlayerId = conn->GetPlayerId();

        // MC LocalPlayer.tick: the keys, whenever they change.
        const uint8_t keys = CurrentInput().Pack();
        if (conn && (!s_inputSent || keys != s_lastSentInput)) {
            Network::PlayerInputC2SPacket packet;
            packet.keys = keys;
            conn->SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerInputC2S),
                             Network::Serialization::Serialize(packet));
            s_inputSent = true;
            s_lastSentInput = keys;
        }

        // MC LocalPlayer.sendPosition for a passenger: the root vehicle it
        // drives (ServerboundMoveVehiclePacket), and a boat's paddles
        // (AbstractBoat.tick's ServerboundPaddleBoatPacket).
        if (Game::LivingEntity* mount = SteeredMount(player)) {
            SendMoveVehicle(*mount);
        } else if (Game::VehicleEntity* driven = DrivenVehicle(player)) {
            SendMoveVehicle(*driven);
            if (const auto* boat = dynamic_cast<const Game::Boat*>(driven); boat && conn) {
                Network::PaddleBoatC2SPacket paddles;
                paddles.left = boat->GetPaddleState(0);
                paddles.right = boat->GetPaddleState(1);
                paddles.reverse = boat->IsPaddlingInReverse();
                conn->SendPacket(static_cast<uint8_t>(Network::PacketId::PaddleBoatC2S),
                                 Network::Serialization::Serialize(paddles));
            }
        }

        // The other players riding something here sit in their seats (MC
        // positionRider on the client's copy of each).
        if (g_remotePlayerManager && g_clientMobManager) {
            for (const auto& [id, rp] : g_remotePlayerManager->GetPlayers()) {
                if (rp.vehicleId == 0) continue;
                const ClientMob* entry = g_clientMobManager->GetMob(rp.vehicleId);
                if (!entry || !entry->mob) continue;
                RemotePlayer* mutableRp = g_remotePlayerManager->GetMutable(id);
                if (!mutableRp) continue;
                const glm::dvec3 seat = Game::PlayerSeatFeetOn(*entry->mob, nullptr, static_cast<int32_t>(id),
                                                               std::max(0.05f, rp.scale));
                mutableRp->position = seat;
                mutableRp->targetPosition = seat;
                mutableRp->lerpSteps = 0;
            }
        }

        PushLocalPlayer(player);
    }

    // ── The frame ───────────────────────────────────────────────────────────

    void FrameUpdate(Game::ClientPlayer& player, float& cameraYaw, float dtSeconds, float partialTick) {
        s_localPlayer = &player;
        s_partialTick = std::clamp(partialTick, 0.0f, 1.0f);
        if (player.vehicleId == 0) { s_pendingYawSnap = false; return; }
        if (s_pendingYawSnap) {
            cameraYaw = s_pendingYaw;
            s_pendingYawSnap = false;
        }
        ClientMob* entry = EntryOf(player.vehicleId);
        auto* boat = entry ? dynamic_cast<Game::Boat*>(entry->mob.get()) : nullptr;
        if (!boat) return;
        // AbstractBoat.positionRider: the driver turns with the boat
        // (deltaRotation a tick, which the view shows spread across the
        // tick's frames as MC's yRotO→yRot lerp does).
        if (boat->IsLocalInstanceAuthoritative()) {
            constexpr float kTickSeconds = 0.05f;
            cameraYaw += boat->DeltaRotation() * std::clamp(dtSeconds / kTickSeconds, 0.0f, 1.0f);
        }
        // clampRotation (every positionRider and onPassengerTurned): the view
        // stays within 105° of the boat's heading.
        glm::dvec3 pos;
        float boatYaw = 0.0f;
        InterpolatedPose(*entry, s_partialTick, pos, boatYaw);
        cameraYaw += Game::Boat::ClampPassengerYaw(cameraYaw, boatYaw);
    }

    bool LocalSeatPosition(const Game::ClientPlayer& player, glm::dvec3& out) {
        if (player.vehicleId == 0 || !g_clientMobManager) return false;
        const ClientMob* entry = g_clientMobManager->GetMob(player.vehicleId);
        if (!entry || !entry->mob) return false;
        const Game::Mob& vehicle = *entry->mob;
        glm::dvec3 pos;
        float yaw = 0.0f;
        InterpolatedPose(*entry, s_partialTick, pos, yaw);
        const float scale = std::max(0.05f, player.physics.scale);
        if (const Game::VehicleEntity* v = VehicleOf(&vehicle)) {
            const int32_t localId = static_cast<int32_t>(s_localPlayerId);
            out = v->PlayerSeatFeetAt(pos, yaw, v->PassengerSlotOfId(localId), std::max(1, v->PassengerTotal()),
                                      scale);
            return true;
        }
        // Any other seat (an equine, a cushion): its offset from the mob's
        // tick position, carried onto the interpolated one.
        const glm::dvec3 seat = Game::PlayerSeatFeetOn(vehicle, nullptr, static_cast<int32_t>(s_localPlayerId), scale);
        out = pos + (seat - vehicle.position);
        return true;
    }

    bool RidesServerControlledInventory(const Game::ClientPlayer& player) {
        if (player.vehicleId == 0 || !g_clientMobManager) return false;
        const ClientMob* entry = g_clientMobManager->GetMob(player.vehicleId);
        if (!entry || !entry->mob) return false;
        // MC MultiPlayerGameMode.isServerControlledInventory: the vehicle is
        // a HasCustomInventoryScreen — a chest boat, or a mount with an
        // inventory screen (the horse family, the llamas, the camels, the
        // nautili; the server's openCustomInventoryScreen decides whether it
        // actually opens, e.g. not for an untamed horse).
        if (entry->mob->HasCustomInventoryScreen()) return true;
        const auto* boat = dynamic_cast<const Game::Boat*>(entry->mob.get());
        return boat && boat->IsChest();
    }

    void Install() {
        Game::LivingEntity::SetRiderControlResolver(true, &ClientRiderControl);
    }

    void Reset() {
        s_localPlayer = nullptr;
        s_jumpRidingTicks = 0;
        s_jumpRidingScale = 0.0f;
        s_wasJumping = false;
        s_inputSent = false;
        s_lastSentInput = 0;
        s_partialTick = 1.0f;
    }

} // namespace Client::Vehicles
