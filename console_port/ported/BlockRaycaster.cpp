#include "BlockRaycaster.h"
#include "Mth.h"
#include "Facing.h"
#include <cmath>
#include <stdexcept>
namespace console {
HitResult *BlockRaycaster::clip(::Vec3 *a, ::Vec3 *b, bool liquid, bool solidOnly)
{
    if(!a || !b)return nullptr;
    for(double coordinate:{a->x,a->y,a->z,b->x,b->y,b->z})
        if(!std::isfinite(coordinate) || coordinate<double(INT32_MIN)+2 || coordinate>double(INT32_MAX)-2)return nullptr;

	if (std::isnan(a->x) || std::isnan(a->y) || std::isnan(a->z)) return NULL;
	if (std::isnan(b->x) || std::isnan(b->y) || std::isnan(b->z)) return NULL;

	int xTile1 = Mth::floor(b->x);
	int yTile1 = Mth::floor(b->y);
	int zTile1 = Mth::floor(b->z);

	int xTile0 = Mth::floor(a->x);
	int yTile0 = Mth::floor(a->y);
	int zTile0 = Mth::floor(a->z);

	{
		int t = getTile(xTile0, yTile0, zTile0);
		int data = getData(xTile0, yTile0, zTile0);
		RaycastTile *tile = tileFor(t);
        if(t>0 && !tile)throw std::logic_error("Unregistered raycast tile");
		if (solidOnly && tile != NULL && tile->getAABB(this, xTile0, yTile0, zTile0) == NULL)
		{
			// No collision

		}
		else if (t > 0 && tile->mayPick(data, liquid))
		{
			HitResult *r = tile->clip(this, xTile0, yTile0, zTile0, a, b);
			if (r != NULL) return r;
		}
	}

	int maxIterations = 200;
	while (maxIterations-- >= 0)
	{
		if (std::isnan(a->x) || std::isnan(a->y) || std::isnan(a->z)) return NULL;
		if (xTile0 == xTile1 && yTile0 == yTile1 && zTile0 == zTile1) return NULL;

		bool xClipped = true;
		bool yClipped = true;
		bool zClipped = true;

		double xClip = 999;
		double yClip = 999;
		double zClip = 999;

		if (xTile1 > xTile0) xClip = xTile0 + 1.000;
		else if (xTile1 < xTile0) xClip = xTile0 + 0.000;
		else xClipped = false;

		if (yTile1 > yTile0) yClip = yTile0 + 1.000;
		else if (yTile1 < yTile0) yClip = yTile0 + 0.000;
		else yClipped = false;

		if (zTile1 > zTile0) zClip = zTile0 + 1.000;
		else if (zTile1 < zTile0) zClip = zTile0 + 0.000;
		else zClipped = false;

		double xDist = 999;
		double yDist = 999;
		double zDist = 999;

		double xd = b->x - a->x;
		double yd = b->y - a->y;
		double zd = b->z - a->z;

		if (xClipped) xDist = (xClip - a->x) / xd;
		if (yClipped) yDist = (yClip - a->y) / yd;
		if (zClipped) zDist = (zClip - a->z) / zd;

		int face = 0;
		if (xDist < yDist && xDist < zDist)
		{
			if (xTile1 > xTile0) face = 4;
			else face = 5;

			a->x = xClip;
			a->y += yd * xDist;
			a->z += zd * xDist;
		}
		else if (yDist < zDist)
		{
			if (yTile1 > yTile0) face = 0;
			else face = 1;

			a->x += xd * yDist;
			a->y = yClip;
			a->z += zd * yDist;
		}
		else
		{
			if (zTile1 > zTile0) face = 2;
			else face = 3;

			a->x += xd * zDist;
			a->y += yd * zDist;
			a->z = zClip;
		}

		::Vec3 *tPos = ::Vec3::newTemp(a->x, a->y, a->z);
		xTile0 = (int) (tPos->x = floor(a->x));
		if (face == 5)
		{
			xTile0--;
			tPos->x++;
		}
		yTile0 = (int) (tPos->y = floor(a->y));
		if (face == 1)
		{
			yTile0--;
			tPos->y++;
		}
		zTile0 = (int) (tPos->z = floor(a->z));
		if (face == 3)
		{
			zTile0--;
			tPos->z++;
		}

		int t = getTile(xTile0, yTile0, zTile0);
		int data = getData(xTile0, yTile0, zTile0);
		RaycastTile *tile = tileFor(t);
        if(t>0 && !tile)throw std::logic_error("Unregistered raycast tile");
		if (solidOnly && tile != NULL && tile->getAABB(this, xTile0, yTile0, zTile0) == NULL)
		{
			// No collision

		}
		else if (t > 0 && tile->mayPick(data, liquid))
		{
			HitResult *r = tile->clip(this, xTile0, yTile0, zTile0, a, b);
			if (r != NULL) return r;
		}
	}
	return NULL;
}
HitResult *BoxRaycastTile::clip(BlockRaycaster *level, int xt, int yt, int zt, ::Vec3 *a, ::Vec3 *b)
{
	AABB* bounds = shape(level, xt, yt, zt);
    if(!bounds)return nullptr;

	a = a->add(-xt, -yt, -zt);
	b = b->add(-xt, -yt, -zt);

	
	::Vec3 *xh0 = a->clipX(b, bounds->x0);
	::Vec3 *xh1 = a->clipX(b, bounds->x1);

	::Vec3 *yh0 = a->clipY(b, bounds->y0);
	::Vec3 *yh1 = a->clipY(b, bounds->y1);

	::Vec3 *zh0 = a->clipZ(b, bounds->z0);
	::Vec3 *zh1 = a->clipZ(b, bounds->z1);

	::Vec3 *closest = NULL;

	if (bounds->containsX(xh0) && (closest == NULL || a->distanceToSqr(xh0) < a->distanceToSqr(closest))) closest = xh0;
	if (bounds->containsX(xh1) && (closest == NULL || a->distanceToSqr(xh1) < a->distanceToSqr(closest))) closest = xh1;
	if (bounds->containsY(yh0) && (closest == NULL || a->distanceToSqr(yh0) < a->distanceToSqr(closest))) closest = yh0;
	if (bounds->containsY(yh1) && (closest == NULL || a->distanceToSqr(yh1) < a->distanceToSqr(closest))) closest = yh1;
	if (bounds->containsZ(zh0) && (closest == NULL || a->distanceToSqr(zh0) < a->distanceToSqr(closest))) closest = zh0;
	if (bounds->containsZ(zh1) && (closest == NULL || a->distanceToSqr(zh1) < a->distanceToSqr(closest))) closest = zh1;

	if (closest == NULL) return NULL;

	int face = -1;

	if (closest == xh0) face = Facing::WEST;
	if (closest == xh1) face = Facing::EAST;
	if (closest == yh0) face = Facing::DOWN;
	if (closest == yh1) face = Facing::UP;
	if (closest == zh0) face = Facing::NORTH;
	if (closest == zh1) face = Facing::SOUTH;

	return new HitResult(xt, yt, zt, face, closest->add(xt, yt, zt));
}
}
