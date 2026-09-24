#pragma once
#include "Vec3.h"
class Entity;

class HitResult
{
public:
	enum Type
	{
		TILE, ENTITY
	};

	Type type;
	int x, y, z, f;
	// Borrowed original vector-pool result; copy coordinates before pool reuse.
	Vec3 *pos;
	shared_ptr<Entity> entity;

	HitResult(int x, int y, int z, int f, Vec3 *pos);

	// Entity-dependent operations require the full Entity port.
	HitResult(shared_ptr<Entity> entity) = delete;

	double distanceTo(shared_ptr<Entity> e) = delete;
};