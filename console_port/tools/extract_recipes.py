#!/usr/bin/env python3
"""Extract the console crafting recipes by compiling the original registration code.

The untouched Recipes.cpp constructor, addShapedRecipy/addShapelessRecipy and the
ToolRecipies/WeaponRecipies/FoodRecipies/OreRecipies/StructureRecipies/
ArmorRecipes/ClothDyeRecipes tables are compiled against small generated stubs:
Tile and Item registries whose members carry the IDs registered in Tile.cpp and
Item.cpp, a minimal ItemInstance, and the tile/item constant classes the
tables use. ShapedRecipy::requires and ShapelessRecipy::requires are copied
verbatim. The harness prints Recipes::getRecipeIngredientsArray(), which is
exactly what the console crafting menu (IUIScene_CraftingMenu) works from.

usage: extract_recipes.py OUTPUT.cpp [--cxx COMPILER] [--check]
"""
from pathlib import Path
import argparse, os, re, subprocess, sys, tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'source_full/Minecraft.World'
TABLES = ['ToolRecipies', 'WeaponRecipies', 'FoodRecipies', 'OreRecipies',
          'StructureRecipies', 'ArmorRecipes', 'ClothDyeRecipes']
CONSTANT_CLASSES = ['TreeTile', 'StoneSlabTile', 'SandStoneTile', 'QuartzBlockTile', 'WallTile',
                    'ClothTile', 'DyePowderItem', 'CoalItem']
GROUPS = ['Structure', 'Tool', 'Food', 'Armour', 'Mechanism', 'Transport', 'Decoration']


def read(name):
    return (SRC / name).read_text(encoding='utf-8-sig', errors='replace')


def clean(text):
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)


def registry(cls, header, source, shift):
    """Static member names of Tile/Item and their registered IDs."""
    h = clean(read(header))
    body = h[h.index('class ' + cls):]
    names = re.findall(r'static\s+(?:const\s+)?[A-Za-z_]\w*\s*\*\s*(\w+)\s*;', body)
    ids = {m[1]: int(m[2]) for m in re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', h)}
    cpp = clean(read(source))
    registered = {}
    for statement in cpp.split(';'):
        m = re.search(cls + r'::(\w+)\s*=.*?new\s+\w+\s*\(\s*(\d+)', statement, flags=re.S)
        if m:
            registered[m[1]] = int(m[2]) + shift
    out = {}
    for name in names:
        if name in registered:
            out[name] = registered[name]
        elif name in ids:
            out[name] = ids[name]
    # ClothTile's constructor hard-codes its ID.
    if cls == 'Tile':
        out.setdefault('cloth', 35)
    return out, ids


def constants(cls):
    h = clean(read(cls + '.h'))
    rows = re.findall(r'static const int (\w+)\s*=\s*(-?\w+)\s*;', h)
    if (SRC / (cls + '.cpp')).exists():
        rows += re.findall(r'const int ' + cls + r'::(\w+)\s*=\s*(-?\w+)\s*;', clean(read(cls + '.cpp')))
    return rows


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += text[i] == '{'
        depth -= text[i] == '}'
        if depth == 0:
            return text[start:i + 1]
    raise ValueError(signature)


def write_stubs(d, tiles, tile_ids, items, item_ids):
    (d / 'stdafx.h').write_text('''#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
using namespace std;
#define XUSER_MAX_COUNT 4
#define ZeroMemory(p,n) memset((p),0,(n))
#define AUTO_VAR(n,v) auto n=(v)
enum eINSTANCEOF { eTYPE_NOTSET, eType_TILE, eType_FIRETILE, eType_ITEM, eType_MAPITEM, eType_ITEMINSTANCE };
struct StubApp { void DebugPrintf(const char*,...){} };
inline StubApp app;
class Level; class CraftingContainer; class FireTile; class MapItem; class Recipy;
typedef std::vector<Recipy *> RecipyList; // Definitions.h
#include "Tile.h"
#include "Item.h"
#include "ItemInstance.h"
''')
    tile = ['#pragma once', 'class Tile { public: int id; explicit Tile(int i):id(i){}']
    tile += [f' static Tile* {n};' for n in tiles]
    tile += [f' static const int {n}_Id={v};' for n, v in tile_ids.items()]
    tile += ['};']
    tile += [f'inline Tile* Tile::{n}=new Tile({v});' for n, v in tiles.items()]
    (d / 'Tile.h').write_text('\n'.join(tile) + '\n')
    item = ['#pragma once', 'class Item { public: int id; explicit Item(int i):id(i){}',
            ' static Item* items[32000];']
    item += [f' static Item* {n};' for n in items]
    item += [f' static const int {n}_Id={v};' for n, v in item_ids.items()]
    item += ['};', 'inline Item* Item::items[32000]={};']
    item += [f'inline Item* Item::{n}=new Item({v});' for n, v in items.items()]
    # Tile items live in Item::items at the tile ID (ClothDyeRecipes uses one).
    item += ['struct TileItemsInit { TileItemsInit(){ for(int i=1;i<256;++i) Item::items[i]=new Item(i); } };',
             'inline TileItemsInit tileItemsInit;']
    (d / 'Item.h').write_text('\n'.join(item) + '\n')
    (d / 'ItemInstance.h').write_text('''#pragma once
class ItemInstance { public:
 int id,count,auxValue;
 ItemInstance(Tile* t,int c=1,int a=0):id(t->id),count(c),auxValue(a){}
 ItemInstance(Item* i,int c=1,int a=0):id(i->id),count(c),auxValue(a){}
 ItemInstance(int i,int c,int a):id(i),count(c),auxValue(a){}
 int getAuxValue()const{return auxValue;}
 ItemInstance* copy_not_shared()const{return new ItemInstance(id,count,auxValue);}
};
''')
    helpers = {'ClothTile': [function(read('ClothTile.cpp'), f'int ClothTile::{name}(')
                             for name in ('getTileDataForItemAuxValue', 'getItemAuxValueForTileData')]}
    for cls in CONSTANT_CLASSES:
        rows = constants(cls)
        methods = ''.join(' static ' + body.replace(cls + '::', '', 1) for body in helpers.get(cls, []))
        (d / (cls + '.h')).write_text('#pragma once\nstruct ' + cls + ' {' +
                                      ''.join(f' static const int {n}={v};' for n, v in rows) + methods + ' };\n')
    for name in ['Container.h', 'AbstractContainerMenu.h', 'CraftingContainer.h',
                 'net.minecraft.world.inventory.h']:
        (d / name).write_text('#pragma once\n')
    constant_includes = ''.join(f'#include "{c}.h"\n' for c in CONSTANT_CLASSES)
    for name in ['net.minecraft.world.item.h', 'net.minecraft.world.Item.h', 'net.minecraft.world.level.tile.h']:
        (d / name).write_text('#pragma once\n' + constant_includes)
    (d / 'net.minecraft.world.item.crafting.h').write_text(
        '#pragma once\n#include "Recipes.h"\n#include "ShapedRecipy.h"\n#include "ShapelessRecipy.h"\n' +
        ''.join(f'#include "{t}.h"\n' for t in TABLES))
    # Real headers used unchanged.
    for name in ['Recipy.h', 'Recipes.h'] + [t + '.h' for t in TABLES]:
        (d / name).write_text(read(name))
    shaped = function(read('ShapedRecipy.cpp'), 'void ShapedRecipy::requires(INGREDIENTS_REQUIRED *pIngReq)')
    shapeless = function(read('ShapelessRecipy.cpp'), 'void ShapelessRecipy::requires(INGREDIENTS_REQUIRED *pIngReq)')
    (d / 'ShapedRecipy.h').write_text('''#pragma once
#include "Recipy.h"
class ShapedRecipy : public Recipy { public:
 int width,height,group; ItemInstance **recipeItems; ItemInstance *result;
 ShapedRecipy(int w,int h,ItemInstance **items,ItemInstance *r,int g=Recipy::eGroupType_Decoration)
  :width(w),height(h),group(g),recipeItems(items),result(r){}
 bool matches(shared_ptr<CraftingContainer>,Level*)override{return false;}
 shared_ptr<ItemInstance> assemble(shared_ptr<CraftingContainer>)override{return nullptr;}
 int size()override{return width*height;}
 const ItemInstance *getResultItem()override{return result;}
 const int getGroup()override{return group;}
 bool requires(int)override{return false;}
 void requires(INGREDIENTS_REQUIRED *pIngReq)override;
 ShapedRecipy *keepTag(){return this;}
 void hideAllHSlots(){}
};
''')
    (d / 'ShapelessRecipy.h').write_text('''#pragma once
#include "Recipy.h"
class ShapelessRecipy : public Recipy { public:
 ItemInstance *result; vector<ItemInstance *> *ingredients; _eGroupType group;
 ShapelessRecipy(ItemInstance *r,vector<ItemInstance *> *i,_eGroupType g=Recipy::eGroupType_Decoration)
  :result(r),ingredients(i),group(g){}
 bool matches(shared_ptr<CraftingContainer>,Level*)override{return false;}
 shared_ptr<ItemInstance> assemble(shared_ptr<CraftingContainer>)override{return nullptr;}
 int size()override{return int(ingredients->size());}
 const ItemInstance *getResultItem()override{return result;}
 const int getGroup()override{return group;}
 bool requires(int)override{return false;}
 void requires(INGREDIENTS_REQUIRED *pIngReq)override;
};
''')
    recipes = read('Recipes.cpp')
    registration = recipes[recipes.index('Recipes *Recipes::instance'):recipes.index('shared_ptr<ItemInstance> Recipes::getItemFor(')]
    # The original reads variadic characters with va_arg(vl,wchar_t), which is
    # undefined: wchar_t is promoted to int through '...'. GCC compiles it to a
    # trap. Read the promoted int instead; the values are unchanged.
    registration = registration.replace('va_arg(vl,wchar_t)', '(wchar_t)va_arg(vl,int)')
    tail = ''.join(function(recipes, sig) for sig in [
        'vector <Recipy *> *Recipes::getRecipies()',
        'void Recipes::buildRecipeIngredientsArray(void)',
        'Recipy::INGREDIENTS_REQUIRED *Recipes::getRecipeIngredientsArray(void)'])
    (d / 'harness.cpp').write_text('#include "stdafx.h"\n#include "net.minecraft.world.level.tile.h"\n'
                                   '#include "net.minecraft.world.item.crafting.h"\n' +
                                   registration + '\n' + tail + '\n' + shaped + '\n' + shapeless + '''
#include <iostream>
int main(){
 Recipes::staticCtor();
 auto* recipes=Recipes::getInstance()->getRecipies();
 auto* required=Recipes::getInstance()->getRecipeIngredientsArray();
 for(size_t i=0;i<recipes->size();++i){
  const auto* result=(*recipes)[i]->getResultItem();
  const auto& r=required[i];
  std::cout<<result->id<<' '<<result->count<<' '<<result->getAuxValue()<<' '<<(*recipes)[i]->getGroup()<<' '<<r.iType<<' '<<r.iIngC;
  for(int j=0;j<r.iIngC;++j)std::cout<<' '<<r.iIngIDA[j]<<' '<<r.iIngAuxValA[j]<<' '<<r.iIngValA[j];
  for(int j=0;j<9;++j)std::cout<<' '<<r.uiGridA[j];
  std::cout<<'\\n';
 }
}
''')
    for t in TABLES:
        (d / (t + '.cpp')).write_text(read(t + '.cpp'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    tiles, tile_ids = registry('Tile', 'Tile.h', 'Tile.cpp', 0)
    items, item_ids = registry('Item', 'Item.h', 'Item.cpp', 256)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        write_stubs(d, tiles, tile_ids, items, item_ids)
        exe = d / 'recipes'
        sources = [str(d / 'harness.cpp')] + [str(d / (t + '.cpp')) for t in TABLES]
        subprocess.run([args.cxx, '-std=c++17', '-w', '-fpermissive' if 'g++' in args.cxx else '-w',
                        '-include', str(d / 'stdafx.h'), '-I', str(d), '-o', str(exe)] + sources, check=True)
        rows = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()
    out = ['// Generated by tools/extract_recipes.py from the original Recipes.cpp and its',
           '// recipe tables (compiled unchanged against ID stubs). Do not edit by hand.',
           '// Order is registration order, which the console crafting menu displays.',
           '#include "CraftingRecipes.h"', 'namespace console {', 'namespace {',
           'std::vector<CraftingRecipe> build(){', ' std::vector<CraftingRecipe> r;']
    for row in rows:
        v = [int(x) for x in row.split()]
        rid, count, aux, group, kind, n = v[:6]
        ingredients = v[6:6 + 3 * n]
        grid = v[6 + 3 * n:]
        ing = ','.join(f'{{{ingredients[3*i]},{ingredients[3*i+1]},{ingredients[3*i+2]}}}' for i in range(n))
        cells = ','.join(str(g) for g in grid)
        out.append(f' r.push_back({{{rid},{count},{aux},{{{ing}}},{"true" if kind == 1 else "false"},'
                   f'"{GROUPS[group]}",{{{cells}}}}});')
    out += [' return r;', '}', '}', 'const std::vector<CraftingRecipe>& consoleCraftingRecipes(){',
            ' static const std::vector<CraftingRecipe> recipes=build();', ' return recipes;', '}', '}', '']
    text = '\n'.join(out)
    target = Path(args.output)
    if args.check:
        if not target.read_text().startswith(text):
            sys.exit(f'{target} differs from the original recipe tables; rerun tools/extract_recipes.py')
        print(f'{len(rows)} recipes match the original tables')
        return
    target.write_text(text)
    print(f'{len(rows)} recipes')


if __name__ == '__main__':
    main()
