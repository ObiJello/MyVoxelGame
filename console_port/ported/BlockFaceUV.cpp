// Generated from TileRenderer full-cube face routines and tesselateTreeInWorld.
#include "BlockFaceUV.h"
#include <stdexcept>
namespace console { namespace {
struct SharedConstants {static constexpr float WORLD_RESOLUTION=16;};
struct TreeTile {enum {MASK_FACING=12,FACING_X=4,FACING_Z=8};};
struct Icon {
 float getU(float x,bool)const{return x/16;}
 float getV(float x,bool)const{return x/16;}
 float getU0(bool)const{return 0;} float getU1(bool)const{return 1;}
 float getV0(bool)const{return 0;} float getV1(bool)const{return 1;}
};
struct Point {float x,y,z,u,v;};
struct Quad {std::array<Point,4> points{};int count=0;
 void vertexUV(float x,float y,float z,float u,float v){points.at(count++)={x,y,z,u,v};}
};
struct FaceBuilder {
 enum {FLIP_CW=1,FLIP_CCW=2,FLIP_180=3};
 float tileShapeX0=0,tileShapeY0=0,tileShapeZ0=0,tileShapeX1=1,tileShapeY1=1,tileShapeZ1=1;
 bool xFlipTexture=false;
 int eastFlip=0,northFlip=0,southFlip=0,westFlip=0,upFlip=0,downFlip=0;
 void tree(int data){
int facing = data & TreeTile::MASK_FACING;

	if (facing == TreeTile::FACING_X)
	{
		northFlip = FLIP_CW;
		southFlip = FLIP_CW;
		upFlip = FLIP_CW;
		downFlip = FLIP_CW;
	}
	else if (facing == TreeTile::FACING_Z)
	{
		eastFlip = FLIP_CW;
		westFlip = FLIP_CW;
	}

	}
Quad renderFaceUp(){ constexpr double x=0,y=0,z=0;

	Quad quad; Quad* t=&quad; Icon icon;Icon* tex=&icon;

	
	float u00 = tex->getU(tileShapeX0 * 16.0f, true);
	float u11 = tex->getU(tileShapeX1 * 16.0f, true);
	float v00 = tex->getV(tileShapeZ0 * 16.0f, true);
	float v11 = tex->getV(tileShapeZ1 * 16.0f, true);

	if ( tileShapeX0 < 0 || tileShapeX1 > 1 )
	{
		u00 = tex->getU0(true);
		u11 = tex->getU1(true);
	}
	if ( tileShapeZ0 < 0 || tileShapeZ1 > 1 )
	{
		v00 = tex->getV0(true);
		v11 = tex->getV1(true);
	}

	float u01 = u11, u10 = u00, v01 = v00, v10 = v11;

	if ( upFlip == FLIP_CW )
	{
		u00 = tex->getU(tileShapeZ0 * 16.0f, true);
		v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeX1 * 16.0f, true);
		u11 = tex->getU(tileShapeZ1 * 16.0f, true);
		v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeX0 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u01 = u00;
		u10 = u11;
		v00 = v11;
		v11 = v01;
	}
	else if ( upFlip == FLIP_CCW )
	{
		// reshape
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeZ1 * 16.0f, true);
		v00 = tex->getV(tileShapeX0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeZ0 * 16.0f, true);
		v11 = tex->getV(tileShapeX1 * 16.0f, true);

		// rotate
		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u00 = u01;
		u11 = u10;
		v01 = v11;
		v10 = v00;
	}
	else if ( upFlip == FLIP_180 )
	{
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeX0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeX1 * 16.0f, true);
		v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeZ0 * 16.0f, true);
		v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeZ1 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
	}


	double x0 = x + tileShapeX0;
	double x1 = x + tileShapeX1;
	double y1 = y + tileShapeY1;
	double z0 = z + tileShapeZ0;
	double z1 = z + tileShapeZ1;

	
		t->vertexUV( ( float )( x1 ), ( float )( y1 ), ( float )( z1 ), ( float )( u11 ), ( float )( v11 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y1 ), ( float )( z0 ), ( float )( u01 ), ( float )( v01 ) );
		t->vertexUV( ( float )( x0 ), ( float )( y1 ), ( float )( z0 ), ( float )( u00 ), ( float )( v00 ) );
		t->vertexUV( ( float )( x0 ), ( float )( y1 ), ( float )( z1 ), ( float )( u10 ), ( float )( v10 ) );
	
return quad;
}
Quad renderFaceDown(){ constexpr double x=0,y=0,z=0;

	Quad quad; Quad* t=&quad; Icon icon;Icon* tex=&icon;

	
	float u00 = tex->getU(tileShapeX0 * 16.0f, true);
	float u11 = tex->getU(tileShapeX1 * 16.0f, true);
	float v00 = tex->getV(tileShapeZ0 * 16.0f, true);
	float v11 = tex->getV(tileShapeZ1 * 16.0f, true);

	if ( tileShapeX0 < 0 || tileShapeX1 > 1 )
	{
		u00 = tex->getU0(true);
		u11 = tex->getU1(true);
	}
	if ( tileShapeZ0 < 0 || tileShapeZ1 > 1 )
	{
		v00 = tex->getV0(true);
		v11 = tex->getV1(true);
	}

	double		u01 = u11, u10 = u00, v01 = v00, v10 = v11;
	if ( downFlip == FLIP_CCW )
	{
		u00 = tex->getU(tileShapeZ0 * 16.0f, true);
		v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeX1 * 16.0f, true);
		u11 = tex->getU(tileShapeZ1 * 16.0f, true);
		v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeX0 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u01 = u00;
		u10 = u11;
		v00 = v11;
		v11 = v01;
	}
	else if ( downFlip == FLIP_CW )
	{
		// reshape
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeZ1 * 16.0f, true);
		v00 = tex->getV(tileShapeX0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeZ0 * 16.0f, true);
		v11 = tex->getV(tileShapeX1 * 16.0f, true);

		// rotate
		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u00 = u01;
		u11 = u10;
		v01 = v11;
		v10 = v00;
	}
	else if ( downFlip == FLIP_180 )
	{
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeX0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeX1 * 16.0f, true);
		v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeZ0 * 16.0f, true);
		v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeZ1 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
	}

	double x0 = x + tileShapeX0;
	double x1 = x + tileShapeX1;
	double y0 = y + tileShapeY0;
	double z0 = z + tileShapeZ0;
	double z1 = z + tileShapeZ1;

	
		t->vertexUV( ( float )( x0 ), ( float )( y0 ), ( float )( z1 ), ( float )( u10 ), ( float )( v10 ) );
		t->vertexUV( ( float )( x0 ), ( float )( y0 ), ( float )( z0 ), ( float )( u00 ), ( float )( v00 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y0 ), ( float )( z0 ), ( float )( u01 ), ( float )( v01 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y0 ), ( float )( z1 ), ( float )( u11 ), ( float )( v11 ) );
	
return quad;
}
Quad renderWest(){ constexpr double x=0,y=0,z=0;

	Quad quad; Quad* t=&quad; Icon icon;Icon* tex=&icon;

	
	double u00 = tex->getU(tileShapeZ0 * 16.0f, true);
	double u11 = tex->getU(tileShapeZ1 * 16.0f, true);
	double v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeY1 * 16.0f, true);
	double v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeY0 * 16.0f, true);
	if ( xFlipTexture )
	{
		double tmp = u00;
		u00 = u11;
		u11 = tmp;
	}

	if ( tileShapeZ0 < 0 || tileShapeZ1 > 1 )
	{
		u00 = tex->getU0(true);
		u11 = tex->getU1(true);
	}
	if ( tileShapeY0 < 0 || tileShapeY1 > 1 )
	{
		v00 = tex->getV0(true);
		v11 = tex->getV1(true);
	}

	double		u01 = u11, u10 = u00, v01 = v00, v10 = v11;

	if ( westFlip == FLIP_CW )
	{
		u00 = tex->getU(tileShapeY0 * 16.0f, true);
		v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeZ1 * 16.0f, true);
		u11 = tex->getU(tileShapeY1 * 16.0f, true);
		v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeZ0 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u01 = u00;
		u10 = u11;
		v00 = v11;
		v11 = v01;
	}
	else if ( westFlip == FLIP_CCW )
	{
		// reshape
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeY1 * 16.0f, true);
		v00 = tex->getV(tileShapeZ0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeY0 * 16.0f, true);
		v11 = tex->getV(tileShapeZ1 * 16.0f, true);

		// rotate
		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u00 = u01;
		u11 = u10;
		v01 = v11;
		v10 = v00;
	}
	else if ( westFlip == FLIP_180 )
	{
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeZ0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeZ1 * 16.0f, true);
		v00 = tex->getV(tileShapeY1 * 16.0f, true);
		v11 = tex->getV(tileShapeY0 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
	}

	double x0 = x + tileShapeX0;
	double y0 = y + tileShapeY0;
	double y1 = y + tileShapeY1;
	double z0 = z + tileShapeZ0;
	double z1 = z + tileShapeZ1;

	
		t->vertexUV( ( float )( x0 ), ( float )( y1 ), ( float )( z1 ), ( float )( u01 ), ( float )( v01 ) );
		t->vertexUV( ( float )( x0 ), ( float )( y1 ), ( float )( z0 ), ( float )( u00 ), ( float )( v00 ) );
		t->vertexUV( ( float )( x0 ), ( float )( y0 ), ( float )( z0 ), ( float )( u10 ), ( float )( v10 ) );
		t->vertexUV( ( float )( x0 ), ( float )( y0 ), ( float )( z1 ), ( float )( u11 ), ( float )( v11 ) );
	
return quad;
}
Quad renderEast(){ constexpr double x=0,y=0,z=0;

	Quad quad; Quad* t=&quad; Icon icon;Icon* tex=&icon;

	
	double u00 = tex->getU(tileShapeZ0 * 16.0f, true);
	double u11 = tex->getU(tileShapeZ1 * 16.0f, true);
	double v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeY1 * 16.0f, true);
	double v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeY0 * 16.0f, true);
	if ( xFlipTexture )
	{
		double tmp = u00;
		u00 = u11;
		u11 = tmp;
	}

	if ( tileShapeZ0 < 0 || tileShapeZ1 > 1 )
	{
		u00 = tex->getU0(true);
		u11 = tex->getU1(true);
	}
	if ( tileShapeY0 < 0 || tileShapeY1 > 1 )
	{
		v00 = tex->getV0(true);
		v11 = tex->getV1(true);
	}

	double		u01 = u11, u10 = u00, v01 = v00, v10 = v11;

	if ( eastFlip == FLIP_CCW )
	{
		u00 = tex->getU(tileShapeY0 * 16.0f, true);
		v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeZ0 * 16.0f, true);
		u11 = tex->getU(tileShapeY1 * 16.0f, true);
		v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeZ1 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u01 = u00;
		u10 = u11;
		v00 = v11;
		v11 = v01;
	}
	else if ( eastFlip == FLIP_CW )
	{
		// reshape
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeY1 * 16.0f, true);
		v00 = tex->getV(tileShapeZ1 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeY0 * 16.0f, true);
		v11 = tex->getV(tileShapeZ0 * 16.0f, true);

		// rotate
		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u00 = u01;
		u11 = u10;
		v01 = v11;
		v10 = v00;
	}
	else if ( eastFlip == FLIP_180 )
	{
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeZ0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeZ1 * 16.0f, true);
		v00 = tex->getV(tileShapeY1 * 16.0f, true);
		v11 = tex->getV(tileShapeY0 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
	}

	double x1 = x + tileShapeX1;
	double y0 = y + tileShapeY0;
	double y1 = y + tileShapeY1;
	double z0 = z + tileShapeZ0;
	double z1 = z + tileShapeZ1;

	
		t->vertexUV( ( float )( x1 ), ( float )( y0 ), ( float )( z1 ), ( float )( u10 ), ( float )( v10 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y0 ), ( float )( z0 ), ( float )( u11 ), ( float )( v11 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y1 ), ( float )( z0 ), ( float )( u01 ), ( float )( v01 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y1 ), ( float )( z1 ), ( float )( u00 ), ( float )( v00 ) );
	
return quad;
}
Quad renderNorth(){ constexpr double x=0,y=0,z=0;

	Quad quad; Quad* t=&quad; Icon icon;Icon* tex=&icon;

	
	double u00 = tex->getU(tileShapeX0 * 16.0f, true);
	double u11 = tex->getU(tileShapeX1 * 16.0f, true);
	double v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeY1 * 16.0f, true);
	double v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeY0 * 16.0f, true);
	if ( xFlipTexture )
	{
		double tmp = u00;
		u00 = u11;
		u11 = tmp;
	}

	if ( tileShapeX0 < 0 || tileShapeX1 > 1 )
	{
		u00 = tex->getU0(true);
		u11 = tex->getU1(true);
	}
	if ( tileShapeY0 < 0 || tileShapeY1 > 1 )
	{
		v00 = tex->getV0(true);
		v11 = tex->getV1(true);
	}

	double		u01 = u11, u10 = u00, v01 = v00, v10 = v11;

	if ( northFlip == FLIP_CCW )
	{
		u00 = tex->getU(tileShapeY0 * 16.0f, true);
		v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeX0 * 16.0f, true);
		u11 = tex->getU(tileShapeY1 * 16.0f, true);
		v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeX1 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u01 = u00;
		u10 = u11;
		v00 = v11;
		v11 = v01;
	}
	else if ( northFlip == FLIP_CW )
	{
		// reshape
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeY1 * 16.0f, true);
		v00 = tex->getV(tileShapeX1 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeY0 * 16.0f, true);
		v11 = tex->getV(tileShapeX0 * 16.0f, true);

		// rotate
		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u00 = u01;
		u11 = u10;
		v01 = v11;
		v10 = v00;
	}
	else if ( northFlip == FLIP_180 )
	{
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeX0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeX1 * 16.0f, true);
		v00 = tex->getV(tileShapeY1 * 16.0f, true);
		v11 = tex->getV(tileShapeY0 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
	}


	double x0 = x + tileShapeX0;
	double x1 = x + tileShapeX1;
	double y0 = y + tileShapeY0;
	double y1 = y + tileShapeY1;
	double z0 = z + tileShapeZ0;

	
		t->vertexUV( ( float )( x0 ), ( float )( y1 ), ( float )( z0 ), ( float )( u01 ), ( float )( v01 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y1 ), ( float )( z0 ), ( float )( u00 ), ( float )( v00 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y0 ), ( float )( z0 ), ( float )( u10 ), ( float )( v10 ) );
		t->vertexUV( ( float )( x0 ), ( float )( y0 ), ( float )( z0 ), ( float )( u11 ), ( float )( v11 ) );
	
return quad;
}
Quad renderSouth(){ constexpr double x=0,y=0,z=0;

	Quad quad; Quad* t=&quad; Icon icon;Icon* tex=&icon;

	
	double u00 = tex->getU(tileShapeX0 * 16.0f, true);
	double u11 = tex->getU(tileShapeX1 * 16.0f, true);
	double v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeY1 * 16.0f, true);
	double v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeY0 * 16.0f, true);
	if ( xFlipTexture )
	{
		double tmp = u00;
		u00 = u11;
		u11 = tmp;
	}

	if ( tileShapeX0 < 0 || tileShapeX1 > 1 )
	{
		u00 = tex->getU0(true);
		u11 = tex->getU1(true);
	}
	if ( tileShapeY0 < 0 || tileShapeY1 > 1 )
	{
		v00 = tex->getV0(true);
		v11 = tex->getV1(true);
	}

	double		u01 = u11, u10 = u00, v01 = v00, v10 = v11;

	if ( southFlip == FLIP_CW )
	{
		u00 = tex->getU(tileShapeY0 * 16.0f, true);
		v11 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeX0 * 16.0f, true);
		u11 = tex->getU(tileShapeY1 * 16.0f, true);
		v00 = tex->getV(SharedConstants::WORLD_RESOLUTION - tileShapeX1 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u01 = u00;
		u10 = u11;
		v00 = v11;
		v11 = v01;
	}
	else if ( southFlip == FLIP_CCW )
	{
		// reshape
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeY1 * 16.0f, true);
		v00 = tex->getV(tileShapeX0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeY0 * 16.0f, true);
		v11 = tex->getV(tileShapeX1 * 16.0f, true);

		// rotate
		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
		u00 = u01;
		u11 = u10;
		v01 = v11;
		v10 = v00;
	}
	else if ( southFlip == FLIP_180 )
	{
		u00 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeX0 * 16.0f, true);
		u11 = tex->getU(SharedConstants::WORLD_RESOLUTION - tileShapeX1 * 16.0f, true);
		v00 = tex->getV(tileShapeY1 * 16.0f, true);
		v11 = tex->getV(tileShapeY0 * 16.0f, true);

		u01 = u11;
		u10 = u00;
		v01 = v00;
		v10 = v11;
	}


	double x0 = x + tileShapeX0;
	double x1 = x + tileShapeX1;
	double y0 = y + tileShapeY0;
	double y1 = y + tileShapeY1;
	double z1 = z + tileShapeZ1;

	
		t->vertexUV( ( float )( x0 ), ( float )( y1 ), ( float )( z1 ), ( float )( u00 ), ( float )( v00 ) );
		t->vertexUV( ( float )( x0 ), ( float )( y0 ), ( float )( z1 ), ( float )( u10 ), ( float )( v10 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y0 ), ( float )( z1 ), ( float )( u11 ), ( float )( v11 ) );
		t->vertexUV( ( float )( x1 ), ( float )( y1 ), ( float )( z1 ), ( float )( u01 ), ( float )( v01 ) );
	
return quad;
}
};
using Faces=std::array<std::array<BlockUV,4>,6>;
Faces makeFaces(int data,BlockBox box={0,0,0,1,1,1}){FaceBuilder builder;builder.tree(data);
 builder.tileShapeX0=box.x0;builder.tileShapeY0=box.y0;builder.tileShapeZ0=box.z0;
 builder.tileShapeX1=box.x1;builder.tileShapeY1=box.y1;builder.tileShapeZ1=box.z1;
 const Quad quads[]={builder.renderFaceUp(),builder.renderFaceDown(),builder.renderWest(),builder.renderEast(),builder.renderNorth(),builder.renderSouth()};
 // Match original vertices by position to the native mesh corner ordering.
 constexpr int corners[6][4][3]={
 {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},{{0,0,1},{0,0,0},{1,0,0},{1,0,1}},
 {{0,1,1},{0,1,0},{0,0,0},{0,0,1}},{{1,1,0},{1,1,1},{1,0,1},{1,0,0}},
 {{0,1,0},{1,1,0},{1,0,0},{0,0,0}},{{1,1,1},{0,1,1},{0,0,1},{1,0,1}}};
 Faces result{};
 for(int f=0;f<6;++f)for(int k=0;k<4;++k){bool found=false;
  for(const auto& p:quads[f].points)if(p.x==float(corners[f][k][0]?box.x1:box.x0) && p.y==float(corners[f][k][1]?box.y1:box.y0) && p.z==float(corners[f][k][2]?box.z1:box.z0)){
   result[f][k]={p.u,p.v};found=true;break;}
  if(!found)throw std::logic_error("Original face vertex missing");
 }
 return result;
}
}
std::array<BlockUV,4> consoleBoxFaceUV(Block block,int face,int data,BlockBox box){
 if(face<0 || face>=6 || data<0 || data>15)throw std::invalid_argument("Invalid block face UV input");
 return makeFaces(block==Log?data:0,box)[face];
}
const std::array<BlockUV,4>& consoleBlockFaceUV(Block block,int face,int data){
 if(face<0 || face>=6 || data<0 || data>15)throw std::invalid_argument("Invalid block face UV input");
 static const std::array<Faces,4> faces={makeFaces(0),makeFaces(4),makeFaces(8),makeFaces(12)};
 return faces[block==Log?data/4:0][face];
}
}
