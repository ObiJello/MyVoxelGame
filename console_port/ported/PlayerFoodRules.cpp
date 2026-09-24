#include "PlayerFoodRules.h"
namespace console {
void PlayerFoodRules::causeFoodExhaustion(float amount)
{
	if( isAllowedToIgnoreExhaustion() || ( isAllowedToFly() && settings.flying) ) return;
	if (settings.invulnerable || hasInvulnerablePrivilege() ) return;

	// 4J Stu - Added 1.8.2 bug fix (TU6) - If players cannot eat, then their food bar should not decrease due to exhaustion
	if(settings.trustPlayers == 0 && settings.cannotBuild != 0) return;

	if (!settings.clientSide)
	{
		foodData.addExhaustion(amount);
	}
}

bool PlayerFoodRules::canEat(bool magicalItem)
{
	return (magicalItem || foodData.needsFood()) && !settings.invulnerable && !hasInvulnerablePrivilege();
}

bool PlayerFoodRules::isAllowedToFly()
{
	bool allowed = false;
	if(settings.hostCanFly != 0 && settings.canFly != 0)
	{
		allowed = true;
	}
	return allowed;
}

bool PlayerFoodRules::isAllowedToIgnoreExhaustion()
{
	bool allowed = false;
	if( (settings.hostCanChangeHunger != 0 && settings.classicHunger != 0) ||
		(isAllowedToFly() && settings.flying) )
	{
		allowed = true;
	}
	return allowed;
}

bool PlayerFoodRules::hasInvulnerablePrivilege()
{
	bool enabled = false;
	if(settings.hostCanBeInvisible != 0 && settings.invulnerablePrivilege != 0)
	{
		enabled = true;
	}
	return enabled;
}
}
