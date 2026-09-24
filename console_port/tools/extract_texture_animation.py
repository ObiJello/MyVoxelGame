#!/usr/bin/env python3
"""Adapt original StitchedTexture frame cycling; bake the imported liquid schedules."""
from pathlib import Path
import re,sys
root=Path(__file__).resolve().parents[1]
source=(root/'original/reference-only/StitchedTexture.cpp').read_text()
start=source.index('{',source.index('void StitchedTexture::cycleFrames()'))
i=start+1; depth=1
while depth:
 if source[i]=='{': depth+=1
 if source[i]=='}': depth-=1
 i+=1
body=source[start+1:i-1]
body=body.replace('frameOverride != NULL','!schedule_.empty()').replace('pair<int, int>','std::pair<int, int>').replace('frameOverride->','schedule_.').replace('frames->size()','frameCount_')
body=body.replace('source->blit(x, y, frames->at(newFrame), rotated);','return newFrame;').replace('source->blit(x, y, frames->at(this->frame), rotated);','return frame;')
out='''// Generated from StitchedTexture::cycleFrames and imported animation timing files.
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
'''+body+'\nreturn -1;\n}\nstd::vector<LiquidAnimationSpec> liquidAnimationSpecs(){return {\n'
for name,x,y,size,frames in [('water',208,192,16,32),('water_flow',224,192,32,32),('lava',208,224,16,20),('lava_flow',224,224,32,16)]:
 text=(root/f'assets/animations/{name}.txt').read_text().strip()
 sequence=[]
 for token in re.split(r'[,\s]+',text):
  if not token:continue
  values=token.split('*');sequence.append((int(values[0]),int(values[1]) if len(values)>1 else 1))
 pairs=','.join('{'+str(a)+','+str(b)+'}' for a,b in sequence)
 out+='{"'+name+'",'+','.join(map(str,[x,y,size,frames]))+',{'+pairs+'}},\n'
out+='};}\n}\n'
Path(sys.argv[1]).write_text(out)
