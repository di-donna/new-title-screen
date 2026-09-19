#include <cstdint>
#include <cstddef>
struct Entry { std::uint32_t registryKey; std::int16_t bubble; std::uint8_t state; bool active; };
struct Update { unsigned char epoch[16]; Entry rosterEntries[128]; unsigned char remaining[0x58500-0x410]; std::uint16_t rosterEntryCount,topLevelRosterCount,groupCount,objectCount; std::uint8_t bubbleBlockCount,paddingBits; bool hasRosterAcknowledgement; };
static_assert(sizeof(Entry)==8);
static_assert(offsetof(Update, rosterEntries) == 0x10);
static_assert(offsetof(Update, rosterEntryCount) == 0x58500);
static_assert(offsetof(Update, topLevelRosterCount) == 0x58502);
static_assert(offsetof(Update, bubbleBlockCount) == 0x58508);
static_assert(offsetof(Update, hasRosterAcknowledgement) == 0x5850a);
struct PatchReader { const unsigned char* data; std::size_t bytes; std::size_t position; bool failed; };
static_assert(sizeof(PatchReader)==32);
__forceinline bool take(PatchReader& r, unsigned width, std::uint64_t& value) noexcept {
    value=0;
    if (r.failed || r.bytes>SIZE_MAX/8 || r.position>r.bytes*8 || width>r.bytes*8-r.position) {
        r.failed=true; return false;
    }
    for (unsigned i=0;i<width;++i) {
        value=(value<<1)|((r.data[r.position/8]>>(7-(r.position&7)))&1);
        ++r.position;
    }
    return true;
}
extern "C" __declspec(dllexport) __declspec(noinline)
bool fixed_roster_ack(PatchReader& r, Update& u) noexcept {
    std::uint64_t present;
    if (!take(r,1,present)) return false;
    if (!present) return true;
    u.hasRosterAcknowledgement=true;
    std::uint64_t blocks=0;
    for (int block=-1;block<static_cast<int>(blocks);++block) {
        const bool top=block==-1;
        std::int16_t bubble=-1;
        if (!top) {
            bubble=-2;
            if (!take(r,1,present)) return false;
            if (present) {
                std::uint64_t key;
                if (!take(r,32,key) || key<0x80000000ULL || key>0x8000003fULL) return false;
                bubble=static_cast<std::int16_t>(key-0x80000000ULL);
            }
        }
        if (!take(r,1,present)) return false;
        if (present) {
            const unsigned width=top?9:7,maximum=top?256:96,words=top?8:3;
            std::uint32_t keys[256],masks[8];
            std::uint8_t states[256];
            std::uint64_t hasKeys,hasMasks,hasStates,keyCount=0,stateCount=0,value;
            if (!take(r,1,hasKeys)) return false;
            if (hasKeys) {
                if (!take(r,width,keyCount) || keyCount>maximum) return false;
                for (unsigned i=0;i<keyCount;++i) {
                    if (!take(r,32,value)) return false;
                    keys[i]=static_cast<std::uint32_t>(value);
                }
            }
            if (!take(r,1,hasMasks)) return false;
            if (hasMasks) for (unsigned i=0;i<words;++i) {
                if (!take(r,32,value)) return false;
                masks[i]=static_cast<std::uint32_t>(value);
            }
            if (!take(r,1,hasStates)) return false;
            if (hasStates) {
                if (!take(r,width,stateCount) || stateCount>maximum || (hasKeys && stateCount!=keyCount)) return false;
                for (unsigned i=0;i<stateCount;++i) {
                    if (!take(r,8,value)) return false;
                    states[i]=static_cast<std::uint8_t>(value);
                }
            }
            if (hasKeys && hasMasks && hasStates && bubble>=-1) {
                if (u.rosterEntryCount>128 || keyCount>128U-u.rosterEntryCount) return false;
                if (top) u.topLevelRosterCount=static_cast<std::uint16_t>(keyCount);
                for (unsigned i=0;i<keyCount;++i) {
                    auto& e=u.rosterEntries[u.rosterEntryCount++];
                    e.registryKey=keys[i];e.bubble=bubble;e.state=states[i];
                    e.active=((masks[i/32]>>(i%32))&1)!=0;
                }
            }
        }
        if (top) {
            if (!take(r,1,present)) return false;
            if (!present) return true;
            if (!take(r,7,blocks) || blocks>64) return false;
            u.bubbleBlockCount=static_cast<std::uint8_t>(blocks);
        }
    }
    return true;
}
