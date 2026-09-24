// Generated from StitchedTexture::cycleFrames and imported animation timing files.
#include "TextureAnimation.h"
#include <stdexcept>
namespace console {
TextureAnimation::TextureAnimation(int count,std::vector<std::pair<int,int>> schedule)
    :frameCount_(count),frame(0),schedule_(std::move(schedule)) {
    if(count<1 || count>4096 || schedule_.size()>=600)throw std::invalid_argument("Invalid animation size");
    for(auto [index,duration]:schedule_)if(index<0 || index>=count || duration<1 || duration>1000000)
        throw std::invalid_argument("Invalid animation frame");
}
int TextureAnimation::tick(){

	if (!schedule_.empty())
	{
		std::pair<int, int> current = schedule_.at(frame);
		subFrame++;
		if (subFrame >= current.second)
		{
			int oldFrame = current.first;
			frame = (frame + 1) % schedule_.size();
			subFrame = 0;

			current = schedule_.at(frame);
			int newFrame = current.first;
			if (oldFrame != newFrame && newFrame >= 0 && newFrame < frameCount_)
			{
				return newFrame;
			}
		}

	}
	else
	{
		int oldFrame = frame;
		frame = (frame + 1) % frameCount_;

		if (oldFrame != frame)
		{
			return frame;
		}
	}

return -1;
}
std::vector<LiquidAnimationSpec> liquidAnimationSpecs(){return {
{"water",208,192,16,32,{{0,2},{1,2},{2,2},{3,2},{4,2},{5,2},{6,2},{7,2},{8,2},{9,2},{10,2},{11,2},{12,2},{13,2},{14,2},{15,2},{16,2},{17,2},{18,2},{19,2},{20,2},{21,2},{22,2},{23,2},{24,2},{25,2},{26,2},{27,2},{28,2},{29,2},{30,2},{31,2}}},
{"water_flow",224,192,32,32,{}},
{"lava",208,224,16,20,{{0,2},{1,2},{2,2},{3,2},{4,2},{5,2},{6,2},{7,2},{8,2},{9,2},{10,2},{11,2},{12,2},{13,2},{14,2},{15,2},{16,2},{17,2},{18,2},{19,2},{18,2},{17,2},{16,2},{15,2},{14,2},{13,2},{12,2},{11,2},{10,2},{9,2},{8,2},{7,2},{6,2},{5,2},{4,2},{3,2},{2,2},{1,2}}},
{"lava_flow",224,224,32,16,{{0,3},{1,3},{2,3},{3,3},{4,3},{5,3},{6,3},{7,3},{8,3},{9,3},{10,3},{11,3},{12,3},{13,3},{14,3},{15,3}}},
};}
}
