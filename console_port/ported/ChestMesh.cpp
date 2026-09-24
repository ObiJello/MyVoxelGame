// ChestModel.cpp dimensions, Cube.cpp face vertices, Polygon.cpp UVs and
// ChestRenderer.cpp orientation, adapted to the native interleaved CPU mesh.
#include "ChestMesh.h"
namespace console {
std::vector<Vertex> buildChestMesh(int x,int y,int z,int facing,int light,bool large){
    const float textureWidth=large?128.f:64.f;
    std::vector<Vertex> result;result.reserve(108);
    auto box=[&](float x0,float y0,float z0,int w,int h,int d,int tx,int ty){
        const float x1=x0+w,y1=y0+h,z1=z0+d;
        const float points[8][3]={{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
            {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}};
        const int faces[6][4]={{5,1,2,6},{0,4,7,3},{5,4,0,1},{2,3,7,6},{1,0,3,2},{4,5,6,7}};
        const int uv[6][4]={{tx+d+w,ty+d,tx+d+w+d,ty+d+h},{tx,ty+d,tx+d,ty+d+h},
            {tx+d,ty,tx+d+w,ty+d},{tx+d+w,ty+d,tx+d+w+w,ty},
            {tx+d,ty+d,tx+d+w,ty+d+h},{tx+d+w+d,ty+d,tx+d+w+d+w,ty+d+h}};
        for(int f=0;f<6;++f){const float us=(uv[f][2]>uv[f][0]?.1f:-.1f)/textureWidth,vs=(uv[f][3]>uv[f][1]?.1f:-.1f)/64;
            const float u0=uv[f][0]/textureWidth+us,u1=uv[f][2]/textureWidth-us,v0=uv[f][1]/64.f+vs,v1=uv[f][3]/64.f-vs;
            const float tex[4][2]={{u1,v0},{u0,v0},{u0,v1},{u1,v1}};
            for(int i:{0,1,2,0,2,3}){const auto& p=points[faces[f][i]];
                float px=p[0]/16-.5f,pz=p[2]/16-.5f;
                float rx=px,rz=pz;
                if(facing==2){rx=-px;rz=-pz;}else if(facing==4){rx=pz;rz=-px;}else if(facing==5){rx=-pz;rz=px;}
                result.push_back({x+.5f+rx+(large&&facing==2?1:0),y+1-p[1]/16,z+.5f-rz+(large&&facing==5?1:0),tex[i][0],tex[i][1],1,1,1,1,
                    (((light>>4)&15)+.5f)/16,(((light>>20)&15)+.5f)/16});
            }
        }
    };
    // ModelPart translation applied to each closed box before chest rotation.
    box(large?15:7,5,0,2,4,1,0,0); // lock
    box(1,6,1,large?30:14,10,14,0,19); // bottom
    box(1,2,1,large?30:14,5,14,0,0); // lid
    return result;
}
}
