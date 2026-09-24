#include "stdafx.h"
#include "ImprovedNoise.h"
#include "PerlinNoise.h"
#include "Mth.h"
#include <iomanip>
#include <iostream>
int main(){
    std::cout<<std::hexfloat;
    for(std::int64_t seed:{0LL,1LL,-1LL,8675309LL,2147483647LL}){
        Random random(seed);
        for(int i=0;i<100;++i)std::cout<<random.nextInt()<<' '<<random.nextLong()<<' '<<random.nextDouble()<<' '<<random.nextGaussian()<<'\n';
        byte bytes[31];random.nextBytes(bytes,31);for(byte b:bytes)std::cout<<int(b)<<'\n';
        for(int i=0;i<31;++i)std::cout<<random.nextFloat()<<' '<<random.nextBoolean()<<'\n';
        for(int bound:{1,7,256,1073741825,2147483647})for(int i=0;i<100;++i)std::cout<<random.nextInt(bound)<<'\n';
        ImprovedNoise noise(&random);
        for(double x:{-16777217.5,-13.25,0.0,.125,27.75})for(double y:{-3.5,0.,1.75})std::cout<<noise.getValue(x,y,3.75)<<'\n';
        PerlinNoise octaves(&random,5);
        for(int ySize:{1,4}){
            doubleArray region=octaves.getRegion({},-16777217,-4,16777219,4,ySize,3,.3,.5,.7);
            for(unsigned i=0;i<region.length;++i)std::cout<<region[i]<<'\n';
        }
        auto grid=noise.create(4,5);for(unsigned i=0;i<grid.length;++i)std::cout<<grid[i]<<'\n';
        for(double x:{-13.25,-.125,0.,.125,27.75})std::cout<<octaves.getValue(x,2.5)<<' '<<Mth::floor(x)<<'\n';
    }
}
