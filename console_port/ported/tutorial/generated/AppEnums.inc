enum eGameSetting
{
	eGameSetting_MusicVolume=0,
	eGameSetting_SoundFXVolume,
	eGameSetting_Gamma,
	eGameSetting_Difficulty,
	eGameSetting_Sensitivity_InGame,
	eGameSetting_Sensitivity_InMenu,
	eGameSetting_ViewBob,
	eGameSetting_ControlScheme,
	eGameSetting_ControlInvertLook,
	eGameSetting_ControlSouthPaw,
	eGameSetting_SplitScreenVertical,
	eGameSetting_GamertagsVisible,
	// Interim TU 1.6.6
	eGameSetting_Autosave,
	eGameSetting_DisplaySplitscreenGamertags,
	eGameSetting_Hints,
	eGameSetting_InterfaceOpacity,
	eGameSetting_Tooltips,
	// TU5
	eGameSetting_Clouds,
	eGameSetting_Online,
	eGameSetting_InviteOnly,
	eGameSetting_FriendsOfFriends,
	eGameSetting_DisplayUpdateMessage,

	// TU6
	eGameSetting_BedrockFog,
	eGameSetting_DisplayHUD,
	eGameSetting_DisplayHand,

	// TU7
	eGameSetting_CustomSkinAnim,

	// TU9
	eGameSetting_DeathMessages,
	eGameSetting_UISize,
	eGameSetting_UISizeSplitscreen,
	eGameSetting_AnimatedCharacter,

	// PS3
	eGameSetting_PS3_EULA_Read,

	// PSVita
	eGameSetting_PSVita_NetworkModeAdhoc,


};
enum EControllerActions
{
	ACTION_MENU_A,
	ACTION_MENU_B,
	ACTION_MENU_X,
	ACTION_MENU_Y,
	ACTION_MENU_UP,
	ACTION_MENU_DOWN,
	ACTION_MENU_RIGHT,
	ACTION_MENU_LEFT,
	ACTION_MENU_PAGEUP,
	ACTION_MENU_PAGEDOWN,
	ACTION_MENU_RIGHT_SCROLL,
	ACTION_MENU_LEFT_SCROLL,
	ACTION_MENU_STICK_PRESS,
	ACTION_MENU_OTHER_STICK_PRESS,
	ACTION_MENU_OTHER_STICK_UP,
	ACTION_MENU_OTHER_STICK_DOWN,
	ACTION_MENU_OTHER_STICK_LEFT,
	ACTION_MENU_OTHER_STICK_RIGHT,
	ACTION_MENU_PAUSEMENU,

#ifdef _DURANGO
	ACTION_MENU_GTC_PAUSE,
	ACTION_MENU_GTC_RESUME,
#endif

#ifdef __ORBIS__
	ACTION_MENU_TOUCHPAD_PRESS,
#endif

	ACTION_MENU_OK,
	ACTION_MENU_CANCEL,
	ACTION_MAX_MENU = ACTION_MENU_CANCEL,

	MINECRAFT_ACTION_JUMP,
	MINECRAFT_ACTION_FORWARD,
	MINECRAFT_ACTION_BACKWARD,
	MINECRAFT_ACTION_LEFT,
	MINECRAFT_ACTION_RIGHT,
	MINECRAFT_ACTION_LOOK_LEFT,
	MINECRAFT_ACTION_LOOK_RIGHT,
	MINECRAFT_ACTION_LOOK_UP,
	MINECRAFT_ACTION_LOOK_DOWN,
	MINECRAFT_ACTION_USE,
	MINECRAFT_ACTION_ACTION,
	MINECRAFT_ACTION_LEFT_SCROLL,
	MINECRAFT_ACTION_RIGHT_SCROLL,
	MINECRAFT_ACTION_INVENTORY,
	MINECRAFT_ACTION_PAUSEMENU,
	MINECRAFT_ACTION_DROP,
	MINECRAFT_ACTION_SNEAK_TOGGLE,
	MINECRAFT_ACTION_CRAFTING,
	MINECRAFT_ACTION_RENDER_THIRD_PERSON,
	MINECRAFT_ACTION_GAME_INFO,
	MINECRAFT_ACTION_DPAD_LEFT,
	MINECRAFT_ACTION_DPAD_RIGHT,
	MINECRAFT_ACTION_DPAD_UP,
	MINECRAFT_ACTION_DPAD_DOWN,

	MINECRAFT_ACTION_MAX,

	// These 4 aren't mapped to the input manager directly but are created from the dpad controls if required in Minecraft::run_middle
	// Don't use them with the input manager directly, just through LocalPlayer::ullButtonsPressed
	MINECRAFT_ACTION_SPAWN_CREEPER,
	MINECRAFT_ACTION_CHANGE_SKIN,
	MINECRAFT_ACTION_FLY_TOGGLE,
	MINECRAFT_ACTION_RENDER_DEBUG
};
