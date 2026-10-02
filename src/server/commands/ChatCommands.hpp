// File: src/server/commands/ChatCommands.hpp
//
// The vanilla commands that put text in front of players:
//
//   /say <message>                      MC SayCommand      "[<source>] <message>" to everyone
//   /msg <targets> <message>            MC MsgCommand      whispers; /tell and /w are its aliases
//   /me <action>                        MC EmoteCommands   "* <source> <action>" to everyone
//   /tellraw <targets> <message>        MC TellRawCommand  a text component, resolved per target
//   /title <targets> (clear | reset | (title|subtitle|actionbar) <title>
//                    | times <fadeIn> <stay> <fadeOut>)      MC TitleCommand
//
// A <message> is MC's MessageArgument: free text whose `@` selectors become
// names (CommandText::ResolveMessage — only for a source allowed
// selectors). A component is MC's ComponentArgument (SNBT or JSON —
// CommandText::ParseComponent), resolved against the source with the target
// as `@s`. Titles go out as TitlesS2C (TitlesS2CPacket.hpp); the action bar
// is a ChatMessageS2C at position 2 with '§' codes for its styling.
//
// /msg, /tell, /w and /me are permission level 0 in MC — every player may
// use them, cheats or not (ServerConnection's command gate exempts them);
// a selector in their targets or message still needs the command
// permission, as MC's entity-selector permission does.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class ChatCommands {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void ExecuteSay(const CommandSourceStack& source, const std::vector<std::string>& args,
                               ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteMsg(const CommandSourceStack& source, const std::vector<std::string>& args,
                               ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteMe(const CommandSourceStack& source, const std::vector<std::string>& args,
                              ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteTellRaw(const CommandSourceStack& source, const std::vector<std::string>& args,
                                   ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteTitle(const CommandSourceStack& source, const std::vector<std::string>& args,
                                 ServerConnection& connection, PlayerSessionManager& sessionManager);
    };

} // namespace Server
