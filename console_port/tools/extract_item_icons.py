#!/usr/bin/env python3
from pathlib import Path
import re,sys,json
r=Path(__file__).resolve().parents[1];ref=r/'original/reference-only'
atlas={name:int(x)+16*int(y) for name,x,y in re.findall(r'new SimpleIcon\(L"([^"]+)",slotSize\*(\d+),slotSize\*(\d+),',(ref/'PreStitchedTextureMap.cpp').read_text())}
source=(ref/'Item.cpp').read_text();rows={}
for statement in source.split(';'):
 id=re.search(r'new \w+\((\d+)',statement);icon=re.search(r'setTextureName\(L"([^"]+)"',statement);label=re.search(r'setDescriptionId\(IDS_ITEM_([A-Z0-9_]+)\)',statement)
 if id and icon:
  value=int(id[1])+256;name=icon[1];text=label[1].replace('_',' ').title() if label else re.sub(r'([a-z])([A-Z])',r'\1 \2',name).title()
  if name in atlas:rows[value]=(atlas[name],text)
for number,name in re.findall(r'new RecordingItem\((\d+), L"([^"]+)"',source):
 rows[int(number)+256]=(atlas['record_'+name],'Music Disc - '+name)
out='''// Generated from Item registrations and the original item atlas.
#include "ItemIcons.h"
namespace console {
ItemIcon consoleItemIcon(int id,int damage){
 switch(id){
'''
for id,(tile,name) in sorted(rows.items()):out+=f' case {id}:return {{{tile},{json.dumps(name)}}};\n'
dyes=re.findall(r'L"([^"]+)"',re.search(r'COLOR_TEXTURES\[\]\s*=\s*\{([^}]+)',(ref/'DyePowderItem.cpp').read_text())[1])
out+=' case 351:{static const ItemIcon dyes[]={'+','.join('{'+str(atlas[name])+','+json.dumps(name.removeprefix('dyePowder_').title()+' Dye')+'}' for name in dyes)+'};return dyes[damage<0?0:damage>15?15:damage];}\n'
skulls=re.findall(r'L"([^"]+)"',re.search(r'SkullItem::ICON_NAMES\[SKULL_COUNT\]\s*=\s*\{([^}]+)',(ref/'SkullItem.cpp').read_text())[1])
assert len(skulls)==5
skull_names=['Skeleton Skull','Wither Skeleton Skull','Zombie Head','Player Head','Creeper Head']
out+=' case 397:{static const ItemIcon skulls[]={'+','.join('{'+str(atlas[name])+','+json.dumps(label)+'}' for name,label in zip(skulls,skull_names))+'};return skulls[damage<0?0:damage>4?4:damage];}\n'
out+=''' default:return {-1,"Item"};
 }
}
}
'''
Path(sys.argv[1]).write_text(out)
