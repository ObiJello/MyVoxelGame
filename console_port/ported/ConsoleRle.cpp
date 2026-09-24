// Original CompressRLE run-selection loop, with an empty-input guard and dynamic buffer.
#include "ConsoleCompression.h"
namespace console::compression {
std::vector<unsigned char> encodeRle(std::span<const unsigned char> source){
    if(source.size()>PS3_MAX_SAVE_BYTES)throw IoError("Compression input exceeds save capacity");
    if(source.empty())return {};
    std::vector<unsigned char> buffer(source.size()*2);
    const unsigned char* pucIn=source.data();
    const unsigned char* pucEnd=pucIn+source.size();
    unsigned char* pucOut=buffer.data();
	do
	{
		unsigned char thisOne = *pucIn++;

		unsigned int count = 1;
		while( ( pucIn != pucEnd ) && ( *pucIn == thisOne ) && ( count < 256 ) )
		{
			pucIn++;
			count++;
		}

		if( count <= 3 )
		{
			if( thisOne == 255 )
			{
				*pucOut++ = 255;
				*pucOut++ = count - 1;
			}
			else
			{
				for( unsigned int i = 0; i < count ; i++ )
				{
					*pucOut++ = thisOne;
				}
			}
		}
		else
		{
			*pucOut++ = 255;
			*pucOut++ = count - 1;
			*pucOut++ = thisOne;
		}
	} while (pucIn != pucEnd);
	
    buffer.resize(static_cast<std::size_t>(pucOut-buffer.data()));return buffer;
}
}
