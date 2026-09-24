// Generated from LadderTile, TileRenderer and Mob ladder movement.
#include "LadderShape.h"
#include <stdexcept>
namespace console {
BlockShape consoleLadderShape(int data){
 BlockShape result;result.count=1;result.boxes[0]={0,0,0,1,1,1};
 auto setShape=[&](double x0,double y0,double z0,double x1,double y1,double z1){result.boxes[0]={x0,y0,z0,x1,y1,z1};};
{
	int dir = data;
	float r = 2 / 16.0f;

	if (dir == 2) setShape(0, 0, 1 - r, 1, 1, 1);
	if (dir == 3) setShape(0, 0, 0, 1, 1, r);
	if (dir == 4) setShape(1 - r, 0, 0, 1, 1, 1);
	if (dir == 5) setShape(0, 0, 0, r, 1, 1);
}
 return result;
}
std::array<LadderVertex,4> consoleLadderQuad(int face){
 if(face<2 || face>5)throw std::invalid_argument("Invalid ladder facing");
 struct Quad {std::array<LadderVertex,4> vertices{};int count=0;
 void vertexUV(float x,float y,float z,float u,float v){vertices.at(count++)={x,y,z,u,v};}}quad;
 auto* t=&quad;constexpr int x=0,y=0,z=0;constexpr float u0=0,v0=0,u1=1,v1=1;
float		o = 0 / 16.0f;
	float		r = 0.05f;
	if ( face == 5 )
	{
		t->vertexUV( ( float )( x + r ), ( float )( y + 1 + o ), ( float )( z + 1 +
					 o ), ( float )( u0 ), ( float )( v0 ) );
		t->vertexUV( ( float )( x + r ), ( float )( y + 0 - o ), ( float )( z + 1 +
					 o ), ( float )( u0 ), ( float )( v1 ) );
		t->vertexUV( ( float )( x + r ), ( float )( y + 0 - o ), ( float )( z + 0 -
					 o ), ( float )( u1 ), ( float )( v1 ) );
		t->vertexUV( ( float )( x + r ), ( float )( y + 1 + o ), ( float )( z + 0 -
					 o ), ( float )( u1 ), ( float )( v0 ) );
	}
	if ( face == 4 )
	{
		t->vertexUV( ( float )( x + 1 - r ), ( float )( y + 0 - o ), ( float )( z + 1 +
					 o ), ( float )( u1 ), ( float )( v1 ) );
		t->vertexUV( ( float )( x + 1 - r ), ( float )( y + 1 + o ), ( float )( z + 1 +
					 o ), ( float )( u1 ), ( float )( v0 ) );
		t->vertexUV( ( float )( x + 1 - r ), ( float )( y + 1 + o ), ( float )( z + 0 -
					 o ), ( float )( u0 ), ( float )( v0 ) );
		t->vertexUV( ( float )( x + 1 - r ), ( float )( y + 0 - o ), ( float )( z + 0 -
					 o ), ( float )( u0 ), ( float )( v1 ) );
	}
	if ( face == 3 )
	{
		t->vertexUV( ( float )( x + 1 + o ), ( float )( y + 0 - o ), ( float )( z +
					 r ), ( float )( u1 ), ( float )( v1 ) );
		t->vertexUV( ( float )( x + 1 + o ), ( float )( y + 1 + o ), ( float )( z +
					 r ), ( float )( u1 ), ( float )( v0 ) );
		t->vertexUV( ( float )( x + 0 - o ), ( float )( y + 1 + o ), ( float )( z +
					 r ), ( float )( u0 ), ( float )( v0 ) );
		t->vertexUV( ( float )( x + 0 - o ), ( float )( y + 0 - o ), ( float )( z +
					 r ), ( float )( u0 ), ( float )( v1 ) );
	}
	if ( face == 2 )
	{
		t->vertexUV( ( float )( x + 1 + o ), ( float )( y + 1 + o ), ( float )( z + 1 -
					 r ), ( float )( u0 ), ( float )( v0 ) );
		t->vertexUV( ( float )( x + 1 + o ), ( float )( y + 0 - o ), ( float )( z + 1 -
					 r ), ( float )( u0 ), ( float )( v1 ) );
		t->vertexUV( ( float )( x + 0 - o ), ( float )( y + 0 - o ), ( float )( z + 1 -
					 r ), ( float )( u1 ), ( float )( v1 ) );
		t->vertexUV( ( float )( x + 0 - o ), ( float )( y + 1 + o ), ( float )( z + 1 -
					 r ), ( float )( u1 ), ( float )( v0 ) );
	}

	
 return quad.vertices;
}
Vec3 consoleLadderVelocity(Vec3 perSecond,bool sneaking){
 // Original Mob velocities are blocks per 20 Hz tick; native movement uses seconds.
 double xd=perSecond.x/20,yd=perSecond.y/20,zd=perSecond.z/20;
float max = 0.15f;
			if (xd < -max) xd = -max;
			if (xd > max) xd = max;
			if (zd < -max) zd = -max;
			if (zd > max) zd = max;
			
			if (yd < -0.15) yd = -0.15;
			bool playerSneaking = sneaking;
			if (playerSneaking && yd < 0) yd = 0;
 return {xd*20,yd*20,zd*20};
}
}
