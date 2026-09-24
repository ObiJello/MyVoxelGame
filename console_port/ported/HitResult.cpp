#include "stdafx.h"
#include "HitResult.h"
HitResult::HitResult(int x, int y, int z, int f, Vec3 *pos)
{
	this->type = TILE;
	this->x = x;
	this->y = y;
	this->z = z;
	this->f = f;
	this->pos = Vec3::newTemp(pos->x, pos->y, pos->z);

	this->entity = nullptr;
}
