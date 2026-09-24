#pragma once
// World-generation fields ported from Biome::staticCtor and biome constructors.
// Entity spawning/decorators remain outside this boundary until their port lands.
class Biome {
public:
    int id;
    float depth,scale,temperature,downfall;
    unsigned char topMaterial,material;
    float getTemperature() const { return temperature; }
    int getTemperatureInt() const { return static_cast<int>(temperature*65536); }
    int getDownfallInt() const { return static_cast<int>(downfall*65536); }
    static Biome* biomes[256];
    static Biome* ocean;
    static Biome* plains;
    static Biome* desert;
    static Biome* extremeHills;
    static Biome* forest;
    static Biome* taiga;
    static Biome* swampland;
    static Biome* river;
    static Biome* hell;
    static Biome* sky;
    static Biome* frozenOcean;
    static Biome* frozenRiver;
    static Biome* iceFlats;
    static Biome* iceMountains;
    static Biome* mushroomIsland;
    static Biome* mushroomIslandShore;
    static Biome* beaches;
    static Biome* desertHills;
    static Biome* forestHills;
    static Biome* taigaHills;
    static Biome* smallerExtremeHills;
    static Biome* jungle;
    static Biome* jungleHills;
};
