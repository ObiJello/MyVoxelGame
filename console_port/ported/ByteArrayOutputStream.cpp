#include "stdafx.h"

#include "ByteArrayOutputStream.h"

// Creates a new ::byte array output stream. The buffer capacity is initially 32 bytes, though its size increases if necessary.
ByteArrayOutputStream::ByteArrayOutputStream()
{
	count = 0;
	buf = byteArray( 32 );
}

//Creates a new ::byte array output stream, with a buffer capacity of the specified size, in bytes.
//Parameters:
//size - the initial size.
ByteArrayOutputStream::ByteArrayOutputStream(unsigned int size)
{
	count = 0;
	buf = byteArray( size );
}

ByteArrayOutputStream::~ByteArrayOutputStream()
{
	if (buf.data != NULL)
		delete[] buf.data;
}

//Writes the specified ::byte to this ::byte array output stream.
//Parameters:
//b - the ::byte to be written.
void ByteArrayOutputStream::write(unsigned int b)
{
	ensureCapacity(1);

	buf[count] = (::byte) b;
	count++;
}

// Writes b.length bytes from the specified ::byte array to this output stream.
//The general contract for write(b) is that it should have exactly the same effect as the call write(b, 0, b.length).
void ByteArrayOutputStream::write(byteArray b)
{
	write(b, 0, b.length);
}

//Writes len bytes from the specified ::byte array starting at offset off to this ::byte array output stream.
//Parameters:
//b - the data.
//off - the start offset in the data.
//len - the number of bytes to write.
void ByteArrayOutputStream::write(byteArray b, unsigned int offset, unsigned int length)
{
	validateByteRange(b,offset,length);
	if(length==0)return;
	// The exposed buffer can be supplied for self-append; preserve it across growth.
	std::vector<unsigned char> copy;
	if(length>std::numeric_limits<unsigned>::max()-count)throw std::length_error("Byte output stream exceeds its addressable length");
	if(length>buf.length-count){copy.assign(b.data+offset,b.data+offset+length);b=byteArray(copy.data(),length);offset=0;}
	ensureCapacity(length);
	std::memmove(buf.data+count,b.data+offset,length);
	//std::copy( b->data+offset, b->data+offset+length, buf->data + count ); // Or this instead?

	count += length;
}

void ByteArrayOutputStream::ensureCapacity(unsigned int extra)
{
	const auto limit=std::numeric_limits<unsigned int>::max();
	if(extra>limit-count)throw std::length_error("Byte output stream exceeds its addressable length");
	unsigned int required=count+extra;
	if(required<=buf.length)return;
	std::uint64_t growth=max<std::uint64_t>(required,max<std::uint64_t>(1,std::uint64_t(buf.length)*2));
	buf.resize(static_cast<unsigned>(min<std::uint64_t>(growth,limit)));
}

//Closing a ByteArrayOutputStream has no effect.
//The methods in this class can be called after the stream has been closed without generating an IOException.
void ByteArrayOutputStream::close()
{
}

//Creates a newly allocated ::byte array. Its size is the current size of this output stream and the valid contents of the buffer have been copied into it.
//Returns:
//the current contents of this output stream, as a ::byte array.
byteArray ByteArrayOutputStream::toByteArray()
{
	byteArray out(count);
	memcpy(out.data,buf.data,count);
	return out;
}
