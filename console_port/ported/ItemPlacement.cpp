#include "ItemPlacement.h"
namespace console {
int placedTileForItem(int itemId){
    switch(itemId){
    case 287:return 132; // string -> tripwire
    case 338:return 83;  // sugar cane -> reeds
    case 354:return 92;  // cake
    case 356:return 93;  // repeater
    case 379:return 117; // brewing stand
    case 380:return 118; // cauldron
    case 390:return 140; // flower pot
    default:return 0;
    }
}
}
