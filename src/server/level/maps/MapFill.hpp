// File: src/server/level/maps/MapFill.hpp
//
// The debug map fill — `/mapfill`, which the debug chord (Debug Modifier + M)
// sends while a filled map is held. Not a vanilla command: it draws the
// held map's whole area at once, as if a carrier had walked every pixel.
//
// A true map needs the real terrain, so every chunk the map's area (and the
// row of pixels north of it, which the first row's shading reads) touches is
// loaded — or generated — through the ordinary chunk pipeline
// (IntegratedServer::RequestChunkLoad, off the tick thread), at most
// kMaxInFlight at a time, nearest the map's centre first. Each chunk is
// sampled the tick it arrives with exactly MapItem.update's column logic
// (MapItems::SamplePixel) and let go again; nobody watches it, so the
// server's normal unload rule and the generator's request sweep reclaim it.
// A pixel is written (MapItemSavedData.updateColor) as soon as it and the
// pixel north of it are both sampled, so the map fills in visibly and every
// carrier is sent the patches by the usual map tick.
//
// `/mapfill` again (the chord pressed again) while the held map — or any
// fill the player started — is running cancels it, as does `/mapfill
// cancel`: the chunk loads still outstanding are let go
// (IntegratedServer::CancelLoadIfUnwanted), the pixels drawn so far stay.
//
// A fill also stops by itself when the player who started it leaves, and
// when no live item stack carries the map's id any more (the census rule in
// MapFill.cpp: maps are shared by id, so a surviving clone keeps it going).
// A framed map gets the filled pixels through the framed-map pass of the map
// tick (every 10 ticks, to every player in the frame's level), as in MC.
//
// Gated like any command: cheats on and the player allowed to use them.
#pragma once

#include "server/commands/CommandDispatcher.hpp"

#include <cstdint>

namespace Server {

    class IntegratedServer;

    namespace MapFill {

        void Register(CommandDispatcher& dispatcher);

        // Advance every running fill; once per server tick (MapItems::Tick).
        void Tick(IntegratedServer& server, int64_t serverTick);

        // Forget every running fill (world closed).
        void Clear();

    } // namespace MapFill

} // namespace Server
