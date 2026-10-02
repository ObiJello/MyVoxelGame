// File: src/common/entity/PlayerCapes.hpp
//
// Every cape Minecraft: Java Edition has released, with its texture hash on
// textures.minecraft.net (http://textures.minecraft.net/texture/<hash>, a
// 64x32 PNG). The launcher's cape grid lists these, downloads the picked one
// on demand into the obeycraft directory (capes/<slug>.png) and hands it to
// the game as --cape. The game itself never looks a cape up by slug: what
// travels is the PNG (PlayerAppearance::capePng), so a server needs no table.
//
// Sources (2026-10): the Minecraft Wiki "Cape" page and per-cape pages for
// names and how each was obtained; the hashes cross-checked against the
// capes.me API and every one fetched from textures.minecraft.net. Capes with
// no Java texture (Bedrock-only Progress Pride, the Xbox 360 capes, the 2010
// holiday capes that were never on accounts, the 23w13a "caep" joke capes)
// are not in the table; neither are Mojang's API test capes.
#pragma once

#include <cstddef>
#include <string_view>

namespace Game {

    enum class CapeCategory : unsigned char {
        Event,      // given to everyone who did something (migrated, claimed, watched…)
        Minecon,    // MINECON attendees
        Mojang,     // Mojang staff
        Community,  // programs: translators, moderators, map makers, champions
        Personal,   // one-off capes given to individual players
    };

    struct CapeInfo {
        std::string_view slug;      // stable id: launcher config, cache file name
        std::string_view name;      // display name
        std::string_view hash;      // textures.minecraft.net texture id
        std::string_view note;      // when / how it was given
        CapeCategory     category;
    };

    inline constexpr CapeInfo kCapes[] = {
        // ── Everyone's / event capes, newest first ──────────────────────────
        { "twisted", "Twisted", "24aafc451aa2cc34ddc7265211678585c0ef4da4d32edb75ecec1bd8b5408381",
          "2026, Minecraft Dungeons II pre-order", CapeCategory::Event },
        { "hero", "Hero", "bb384a1079b9a6f2811520c7991b6e8150d02e5a96457e44509e30822f72f38e",
          "2026, logging into Dungeons and Dungeons II", CapeCategory::Event },
        { "aurora", "Aurora", "7c418dfbd37412a55e9f1425c9807a591ad10cf71b1edb576a587be9114c277f",
          "2026, Dungeons II stream watch time", CapeCategory::Event },
        { "builder", "Builder", "2c579968c64c1719740fd8c2a451461879b238002574fce48f7d1a7c36a1c7d4",
          "2026, Movie Build Challenge watch time", CapeCategory::Event },
        { "crafter", "Crafter", "479eacefa3cdd7aca94207f36c0dd449653ddf259daf40544a5866baf05eee22",
          "2026, TwitchCon Rotterdam in person", CapeCategory::Event },
        { "moonlight_trail", "Moonlight Trail", "fe8a02dfe9e390e44ff33d69feef9d3943f76d3901015bbd50f0b67722d288bd",
          "2026, Minecraft Experience: Moonlight Trail", CapeCategory::Event },
        { "zombie_horse", "Zombie Horse", "a3f6e4f14801f3ea55e3d95b9b4ef3b5e8802d947f669de93d6ec4b9354a436b",
          "2025, Mounts of Mayhem spear challenge", CapeCategory::Event },
        { "copper", "Copper", "5e6f3193e74cd16cdd6637d9bae5484e3a37ff2a14c2d157c659a07810b1bdca",
          "2025, Copper Cape Quest", CapeCategory::Event },
        { "yearn", "Yearn", "308b32a9e303155a0b4262f9e5483ad4a22e3412e84fe8385a0bdd73dc41fa89",
          "2025, A Minecraft Movie Live Event quests", CapeCategory::Event },
        { "menace", "Menace", "dbc21e222528e30dc88445314f7be6ff12d3aeebc3c192054fba7e3b3f8c77b1",
          "2025, A Minecraft Movie promo (TikTok)", CapeCategory::Event },
        { "home", "Home", "1de21419009db483900da6298a1e6cbf9f1bc1523a0dcdc16263fab150693edd",
          "2025, A Minecraft Movie promo (Twitch)", CapeCategory::Event },
        { "common", "Common", "5ec930cdd2629c8771655c60eebeb867b4b6559b0e6d3bc71c40c96347fa03f0",
          "2025, Bedrock sign-in with a Microsoft account", CapeCategory::Event },
        { "pan", "Pan", "28de4a81688ad18b49e735a273e086c18f1e3966956123ccb574034c06f5d336",
          "2025, given to all players", CapeCategory::Event },
        { "founders", "Founder's", "99aba02ef05ec6aa4d42db8ee43796d6cd50e4b2954ab29f0caeb85f96bf52a1",
          "2025 on Java, MINECON Earth 2019 on Bedrock", CapeCategory::Event },
        { "mojang_office", "Mojang Office", "5c29410057e32abec02d870ecb52ec25fb45ea81e785a7854ae8429d7236ca26",
          "2024, Mojang Office party", CapeCategory::Event },
        { "mcc_15th_year", "MCC 15th Year", "56c35628fe1c4d59dd52561a3d03bfa4e1a76d397c8b9c476c2f77cb6aebb1df",
          "2024, MC Championship anniversary event", CapeCategory::Event },
        { "followers", "Follower's", "569b7f2a1d00d26f30efe3f9ab9ac817b1e6d35f4f3cfb0324ef2d328223d350",
          "2024, TikTok live watch time", CapeCategory::Event },
        { "fifteenth_anniversary", "15th Anniversary", "cd9d82ab17fd92022dbd4a86cde4c382a7540e117fae7b9a2853658505a80625",
          "2024, the 15th anniversary claim", CapeCategory::Event },
        { "purple_heart", "Purple Heart", "cb40a92e32b57fd732a00fc325e7afb00a7ca74936ad50d8e860152e482cfbde",
          "2024, Twitch watch time", CapeCategory::Event },
        { "minecraft_experience", "Minecraft Experience", "7658c5025c77cfac7574aab3af94a46a8886e3b7722a895255fbf22ab8652434",
          "2024, Minecraft Experience: Villager Rescue", CapeCategory::Event },
        { "cherry_blossom", "Cherry Blossom", "afd553b39358a24edfe3b8a9a939fa5fa4faa4d9a9c3d6af8eafb377fa05c2bb",
          "2023, Trails & Tales promotion", CapeCategory::Event },
        { "vanilla", "Vanilla", "f9a76537647989f9a0b6d001e320dac591c359e9e61a31f4ce11c88f207f0ad4",
          "2022, owning Java and Bedrock", CapeCategory::Event },
        { "migrator", "Migrator", "2340c0e03dd24a11b15a8b33c2a7e9e32abb2051b2481d0ba7defd635ca7a933",
          "2021, migrating to a Microsoft account", CapeCategory::Event },

        // ── MINECON ─────────────────────────────────────────────────────────
        { "minecon_2016", "MINECON 2016", "e7dfea16dc83c97df01a12fabbd1216359c0cd0ea42f9999b6e97c584963e980",
          "2016, MINECON attendees", CapeCategory::Minecon },
        { "minecon_2015", "MINECON 2015", "b0cc08840700447322d953a02b965f1d65a13a603bf64b17c803c21446fe1635",
          "2015, MINECON attendees", CapeCategory::Minecon },
        { "minecon_2013", "MINECON 2013", "153b1a0dfcbae953cdeb6f2c2bf6bf79943239b1372780da44bcbb29273131da",
          "2013, MINECON attendees", CapeCategory::Minecon },
        { "minecon_2012", "MINECON 2012", "a2e8d97ec79100e90a75d369d1b3ba81273c4f82bc1b737e934eed4a854be1b6",
          "2012, MINECON attendees", CapeCategory::Minecon },
        { "minecon_2011", "MINECON 2011", "953cac8b779fe41383e675ee2b86071a71658f2180f56fbce8aa315ea70e2ed6",
          "2011, MINECON attendees", CapeCategory::Minecon },

        // ── Mojang staff ────────────────────────────────────────────────────
        { "mojang_studios", "Mojang Studios", "9e507afc56359978a3eb3e32367042b853cddd0995d17d0da995662913fb00f7",
          "2021 on, Mojang Studios staff", CapeCategory::Mojang },
        { "mojang", "Mojang", "5786fe99be377dfb6858859f926c4dbc995751e91cee373468c5fbf4865e7151",
          "2015 to 2021, Mojang staff", CapeCategory::Mojang },
        { "mojang_classic", "Mojang Classic", "8f120319222a9f4a104e2f5cb97b2cda93199a2ee9e1585cb8d09d6f687cb761",
          "2010 to 2015, Mojang staff", CapeCategory::Mojang },

        // ── Community programs ──────────────────────────────────────────────
        { "realms_mapmaker", "Realms Mapmaker", "17912790ff164b93196f08ba71d0e62129304776d0f347334f8a6eae509f8a56",
          "2015, the Realms map-making program", CapeCategory::Community },
        { "cobalt", "Cobalt", "ca35c56efe71ed290385f4ab5346a1826b546a54d519e6a3ff01efa01acce81",
          "2016, Cobalt map challenge winners", CapeCategory::Community },
        { "scrolls_champion", "Scrolls Champion", "3efadf6510961830f9fcc077f19b4daf286d502b5f5aafbd807c7bbffcaca245",
          "2014, Scrolls tournament winners", CapeCategory::Community },
        { "mojira_moderator", "Mojira Moderator", "ae677f7d98ac70a533713518416df4452fe5700365c09cf45d0d156ea9396551",
          "2014, bug-tracker moderators", CapeCategory::Community },
        { "translator", "Translator", "1bf91499701404e21bd46b0191d63239a4ef76ebde88d27e4d430ac211df681e",
          "2012, Crowdin proofreaders", CapeCategory::Community },
        { "translator_chinese", "Translator (Chinese)", "2262fb1d24912209490586ecae98aca8500df3eff91f2a07da37ee524e7e3cb6",
          "2015, Chinese Crowdin proofreaders", CapeCategory::Community },

        // ── Personal capes ──────────────────────────────────────────────────
        { "birthday", "Birthday", "2056f2eebd759cce93460907186ef44e9192954ae12b227d817eb4b55627a7fc",
          "2021, Gr8_Escape", CapeCategory::Personal },
        { "turtle", "Turtle", "5048ea61566353397247d2b7d946034de926b997d5e66c86483dfb1e031aee95",
          "2018, billyK_", CapeCategory::Personal },
        { "prismarine", "Prismarine", "d8f8d13a1adf9636a16c31d47f3ecc9bb8d8533108aa5ad2a01b13b1a0c55eac",
          "2016, Drullkus", CapeCategory::Personal },
        { "spade", "Spade", "2e002d5e1758e79ba51d08d92a0f3a95119f2f435ae7704916507b6c565a7da8",
          "2015, MrMessiah", CapeCategory::Personal },
        { "cheapsh0t", "Cheapsh0t's", "ca29f5dd9e94fb1748203b92e36b66fda80750c87ebc18d6eafdb0e28cc1d05f",
          "2013, cheapsh0t", CapeCategory::Personal },
        { "snowman", "Snowman", "23ec737f18bfe4b547c95935fc297dd767bb84ee55bfd855144d279ac9bfd9fe",
          "2011, JulianClark", CapeCategory::Personal },
        { "db", "dB", "bcfbe84c6542a4a5c213c1cacf8979b5e913dcb4ad783a8b80e3c4a7d5c8bdac",
          "2011, dannyBstyle", CapeCategory::Personal },
        { "millionth_customer", "Millionth Customer", "70efffaf86fe5bc089608d3cb297d3e276b9eb7a8f9f2fe6659c23a2d8b18edf",
          "2011, the millionth sale", CapeCategory::Personal },
        { "bacon", "Bacon", "fd14214cd8073059e93d9c626260f5df85e5a959181537119df56cadaf5002cc",
          "2010, Notch's, later removed", CapeCategory::Personal },
        { "valentine", "Valentine", "e578ef995fabcf0a94768f9651ac3aaba30c59ef85d2438e9b3e0cc1d810652b",
          "briefly given, then removed", CapeCategory::Personal },
        { "oxeye", "Oxeye", "7706b5f5fc90329691e59277dcc66ba20572219fa8e5da472afd5235fad12cc8",
          "given by accident, then removed", CapeCategory::Personal },
        { "blueprint", "Blueprint", "fdcf48f01ec480d1d7cbec27f7ddce48c9da2be6724641109444dae58d4cd013",
          "SupaCookiebrat, later removed", CapeCategory::Personal },
    };

    inline constexpr size_t kCapeCount = sizeof(kCapes) / sizeof(kCapes[0]);

    inline const char* CapeCategoryName(CapeCategory c) {
        switch (c) {
            case CapeCategory::Event:     return "Events";
            case CapeCategory::Minecon:   return "MINECON";
            case CapeCategory::Mojang:    return "Mojang";
            case CapeCategory::Community: return "Community";
            case CapeCategory::Personal:  return "Personal";
        }
        return "";
    }

    // Null for an unknown slug.
    inline const CapeInfo* FindCape(std::string_view slug) {
        for (const CapeInfo& c : kCapes) {
            if (c.slug == slug) return &c;
        }
        return nullptr;
    }

    // The cape a Mojang profile's CAPE texture URL points at (matched on the
    // hash after the last '/'), or null for one this table does not know.
    inline const CapeInfo* FindCapeByUrl(std::string_view url) {
        const size_t slash = url.find_last_of('/');
        const std::string_view hash = slash == std::string_view::npos ? url : url.substr(slash + 1);
        for (const CapeInfo& c : kCapes) {
            if (c.hash == hash) return &c;
        }
        return nullptr;
    }

    inline constexpr std::string_view kTextureServer = "http://textures.minecraft.net/texture/";

} // namespace Game
