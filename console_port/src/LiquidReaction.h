#pragma once
namespace console {
class LiquidReactionAccess {
public:
    virtual ~LiquidReactionAccess()=default;
    virtual bool lava(int x,int y,int z)const=0;
    virtual bool water(int x,int y,int z)const=0;
    virtual int data(int x,int y,int z)const=0;
};
struct LiquidReaction { int replacement=0; bool fizz=false; };
LiquidReaction consoleLiquidReaction(const LiquidReactionAccess&,int x,int y,int z);
}
