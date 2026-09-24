#pragma once
#include "ArrayWithLength.h"

class DataLayer
{
public:
    byteArray data;
    DataLayer(const DataLayer&) = delete;
    DataLayer& operator=(const DataLayer&) = delete;

private:
	const int depthBits;
	const int depthBitsPlusFour;

public:
    DataLayer(int length, int depthBits);
    DataLayer(byteArray data, int depthBits);
	~DataLayer();

    int get(int x, int y, int z);

    void set(int x, int y, int z, int val);
    bool isValid();
    void setAll(int br);
};
