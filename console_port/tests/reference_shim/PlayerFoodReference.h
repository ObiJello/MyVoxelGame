#pragma once
#include "PlayerFoodRules.h"

enum FoodHostOption {
    eGameHostOption_HostCanFly, eGameHostOption_HostCanChangeHunger,
    eGameHostOption_HostCanBeInvisible, eGameHostOption_TrustPlayers
};
// Fixture services for the unmodified original predicate bodies. The reference
// shares FoodData (verified separately) to isolate player permission decisions.
class ReferenceFoodPolicy {
    FoodData& foodData;
public:
    console::PlayerFoodSettings settings;
    enum Privilege {
        ePlayerGamePrivilege_CanFly, ePlayerGamePrivilege_ClassicHunger,
        ePlayerGamePrivilege_Invulnerable, ePlayerGamePrivilege_CannotBuild
    };
    struct App {
        console::PlayerFoodSettings& settings;
        int GetGameHostOption(FoodHostOption option) {
            switch(option) {
                case eGameHostOption_HostCanFly: return settings.hostCanFly;
                case eGameHostOption_HostCanChangeHunger: return settings.hostCanChangeHunger;
                case eGameHostOption_HostCanBeInvisible: return settings.hostCanBeInvisible;
                case eGameHostOption_TrustPlayers: return settings.trustPlayers;
            }
            throw std::runtime_error("Unknown original host option");
        }
    } app{settings};
    struct Abilities { bool& invulnerable; bool& flying; } abilities{settings.invulnerable,settings.flying};
    struct Level { bool& isClientSide; } world{settings.clientSide};
    Level* level=&world;
    explicit ReferenceFoodPolicy(FoodData& food):foodData(food) {}
    int getPlayerGamePrivilege(Privilege privilege) {
        switch(privilege) {
            case ePlayerGamePrivilege_CanFly: return settings.canFly;
            case ePlayerGamePrivilege_ClassicHunger: return settings.classicHunger;
            case ePlayerGamePrivilege_Invulnerable: return settings.invulnerablePrivilege;
            case ePlayerGamePrivilege_CannotBuild: return settings.cannotBuild;
        }
        throw std::runtime_error("Unknown original food privilege");
    }
    bool isAllowedToFly();
    bool isAllowedToIgnoreExhaustion();
    bool hasInvulnerablePrivilege();
    void causeFoodExhaustion(float amount);
    bool canEat(bool magicalItem);
};
