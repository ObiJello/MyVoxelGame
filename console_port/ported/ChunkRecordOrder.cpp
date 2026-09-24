// Original numeric fields and section order; entity construction is a separate boundary.
#include "ChunkRecord.h"
void console::ChunkRecord::writePrefix(DataOutputStream* dos)
{
	dos->writeShort(SAVE_FILE_VERSION_NUMBER);
	dos->writeInt(this->x);
	dos->writeInt(this->z);
	dos->writeLong(lastUpdate);

	this->writeCompressedBlockData(dos);

	this->writeCompressedDataData(dos);

	this->writeCompressedSkyLightData(dos);
	this->writeCompressedBlockLightData(dos);

	dos->write(this->heightmap);
	dos->writeShort(this->terrainPopulated);
	dos->write(this->getBiomes());
}
void console::ChunkRecord::writeCompressedBlockData(DataOutputStream *dos)
{
	lowerBlocks->write(dos);
	upperBlocks->write(dos);
}
void console::ChunkRecord::writeCompressedDataData(DataOutputStream *dos)
{
	lowerData->write(dos);
	upperData->write(dos);
}
void console::ChunkRecord::writeCompressedSkyLightData(DataOutputStream *dos)
{
	lowerSkyLight->write(dos);
	upperSkyLight->write(dos);
}
void console::ChunkRecord::writeCompressedBlockLightData(DataOutputStream *dos)
{
	lowerBlockLight->write(dos);
	upperBlockLight->write(dos);
}
