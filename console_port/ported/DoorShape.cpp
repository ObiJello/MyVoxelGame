// Generated from original DoorTile.cpp by tools/extract_door_shape.py.
#include "DoorShape.h"
#include <stdexcept>
namespace console {namespace {
constexpr int UPPER_BIT=8,C_DIR_MASK=3,C_OPEN_MASK=4,C_LOWER_DATA_MASK=7,C_IS_UPPER_MASK=8,C_RIGHT_HINGE_MASK=16;
using LevelSource=const BlockShapeAccess;
struct Facing {enum {UP=1,DOWN=0};};
}
int consoleDoorData(const BlockShapeAccess& access,int x,int y,int z){const auto* level=&access;

	int data = level->getData(x, y, z);
	bool isUpper = (data & UPPER_BIT) != 0;
	int lowerData;
	int upperData;
	if (isUpper)
	{
		lowerData = level->getData(x, y - 1, z);
		upperData = data;
	}
	else
	{
		lowerData = data;
		upperData = level->getData(x, y + 1, z);
	}

	// bits: dir, dir, open/closed, isUpper, isRightHinge
	bool isRightHinge = (upperData & 1) != 0;
	return (lowerData & C_LOWER_DATA_MASK) | (isUpper ? 8 : 0) | (isRightHinge ? 16 : 0);
}
BlockShape consoleDoorShape(const BlockShapeAccess& access,int x,int y,int z){
 int compositeData=consoleDoorData(access,x,y,z);BlockShape result;result.count=1;
 auto setShape=[&](double x0,double y0,double z0,double x1,double y1,double z1){result.boxes[0]={x0,y0,z0,x1,y1,z1};};

	float r = 3 / 16.0f;
	setShape(0, 0, 0, 1, 2, 1);
	int dir = compositeData & C_DIR_MASK;
	bool open = (compositeData & C_OPEN_MASK) != 0;
	bool hasRightHinge = (compositeData & C_RIGHT_HINGE_MASK) != 0;
	if (dir == 0)
	{
		if (open)
		{
			if (!hasRightHinge) setShape(0, 0, 0, 1, 1, r);
			else setShape(0, 0, 1 - r, 1, 1, 1);
		}
		else setShape(0, 0, 0, r, 1, 1);
	}
	else if (dir == 1)
	{
		if (open)
		{
			if (!hasRightHinge) setShape(1 - r, 0, 0, 1, 1, 1);
			else setShape(0, 0, 0, r, 1, 1);
		}
		else setShape(0, 0, 0, 1, 1, r);
	}
	else if (dir == 2)
	{
		if (open)
		{
			if (!hasRightHinge) setShape(0, 0, 1 - r, 1, 1, 1);
			else setShape(0, 0, 0, 1, 1, r);
		}
		else setShape(1 - r, 0, 0, 1, 1, 1);
	}
	else if (dir == 3)
	{
		if (open)
		{
			if (!hasRightHinge) setShape(0, 0, 0, r, 1, 1);
			else setShape(1 - r, 0, 0, 1, 1, 1);
		}
		else setShape(0, 0, 1 - r, 1, 1, 1);
	}

 return result;
}
DoorTexture consoleDoorTexture(const BlockShapeAccess& access,int x,int y,int z,int nativeFace){
 if(nativeFace<0 || nativeFace>5)throw std::invalid_argument("Invalid door face");
 constexpr int faces[]={1,0,4,5,2,3};int face=faces[nativeFace];
 const auto* level=&access;constexpr int DOOR_TILE_TEXTURE_COUNT=4;
 const DoorTexture icons[]={{97,false},{81,false},{98,false},{82,false},{97,true},{81,true},{98,true},{82,true}};
 int texBase=access.getTile(x,y,z)==71?2:0;

	if (face == Facing::UP || face == Facing::DOWN) return icons[texBase];

	int compositeData = consoleDoorData(access, x, y, z);
	int dir = compositeData & C_DIR_MASK;
	bool isOpen = (compositeData & C_OPEN_MASK) != 0;
	bool flip = false;
	bool upper = (compositeData & C_IS_UPPER_MASK) != 0;

	if (isOpen)
	{
		if (dir == 0 && face == 2) flip = !flip;
		else if (dir == 1 && face == 5) flip = !flip;
		else if (dir == 2 && face == 3) flip = !flip;
		else if (dir == 3 && face == 4) flip = !flip;
	}
	else
	{
		if (dir == 0 && face == 5) flip = !flip;
		else if (dir == 1 && face == 3) flip = !flip;
		else if (dir == 2 && face == 4) flip = !flip;
		else if (dir == 3 && face == 2) flip = !flip;
		if ((compositeData & C_RIGHT_HINGE_MASK) != 0) flip = !flip;
	}

	return icons[texBase + (flip ? DOOR_TILE_TEXTURE_COUNT : 0) + (upper ? 1 : 0)];
}
}
