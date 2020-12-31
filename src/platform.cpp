#include "platform.h"
#include "../dep/tl/include/tl/debug.h"

#include <unordered_set>

struct MemoryBlock {
	Span<u8> data;
	char const *file;
	char const *message;
	u32 line;
	bool operator==(MemoryBlock const &that) const {
		return data.data() == that.data.data();
	}
};

namespace std {
template <>
struct hash<MemoryBlock> {
	umm operator()(MemoryBlock const &v) const {
		return (umm)v.data.data();
	}
};
}

static std::unordered_set<MemoryBlock> usedBlocks;
static RecursiveMutex mutex;

void allocationTracker(void *data, umm size, char const *file, u32 line, char const *message) {
	MemoryBlock block;
	block.data = {(u8 *)data, size};
	block.file = file;
	block.line = line;
	block.message = message;

	SCOPED_LOCK(mutex);
	usedBlocks.insert(block);
}
void deallocationTracker(void *data) {
	SCOPED_LOCK(mutex);
	MemoryBlock b;
	b.data = {(u8 *)data, (u8 *)data};
	usedBlocks.erase(*usedBlocks.find(b));
}

void logUnfreedMemory() {
	SCOPED_LOCK(mutex);
	LOG("Unfreed memory:");
	for (auto &b : usedBlocks) {
		LOG("% (% bytes) %(%) \"%\"", b.data.data(), b.data.size(), b.file, b.line, b.message);
	}
};
