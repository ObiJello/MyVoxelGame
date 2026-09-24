#include "stdafx.h"

#include "TickNextTickData.h"
#include <bit>
#include <limits>
#include <stdexcept>

std::atomic<std::uint64_t> TickNextTickData::C{0};

std::uint64_t TickNextTickData::nextSequence()
{
    auto value = C.load(std::memory_order_relaxed);
    do {
        if(value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Scheduled tick insertion sequence exhausted");
    } while(!C.compare_exchange_weak(value,value+1,std::memory_order_relaxed));
    return value;
}

TickNextTickData::TickNextTickData(int x, int y, int z, int tileId)
{
	m_delay = 0;
	c = nextSequence();

	this->x = x;
	this->y = y;
	this->z = z;
	this->tileId = tileId;
}


bool TickNextTickData::equals(const TickNextTickData *o) const
{
    return o && x == o->x && y == o->y && z == o->z && tileId == o->tileId;
}

int TickNextTickData::hashCode() const
{
    const std::uint32_t hash = ((std::uint32_t(x) * 1024u * 1024u +
        std::uint32_t(z) * 1024u + std::uint32_t(y)) * 256u) + std::uint32_t(tileId);
    return std::bit_cast<std::int32_t>(hash);
}

TickNextTickData *TickNextTickData::delay(std::int64_t l)
{
	this->m_delay = l;
	return this;
}

int TickNextTickData::compareTo(const TickNextTickData *tnd) const
{
	if (m_delay < tnd->m_delay) return -1;
	if (m_delay > tnd->m_delay) return 1;
	if (c < tnd->c) return -1;
	if (c > tnd->c) return 1;
	return 0;
}

//A class that takes two arguments of the same type as the container elements and returns a bool.
//The expression comp(a,b), where comp is an object of this comparison class and a and b are elements of the container,
//shall return true if a is to be placed at an earlier position than b in a strict weak ordering operation.
//This can either be a class implementing a function call operator or a pointer to a function (see constructor for an example).
//This defaults to less<Key>, which returns the same as applying the less-than operator (a<b).
bool TickNextTickData::compare_fnct(const TickNextTickData &x, const TickNextTickData &y)
{
	return x.compareTo( &y ) < 0;
}

int TickNextTickData::hash_fnct(const TickNextTickData &k)
{
	return k.hashCode();
}

bool TickNextTickData::eq_test(const TickNextTickData &x, const TickNextTickData &y)
{
	return ( x.x == y.x ) && ( x.y == y.y ) && ( x.z == y.z ) && ( x.tileId == y.tileId );
}
