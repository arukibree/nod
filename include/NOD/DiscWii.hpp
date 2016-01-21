#ifndef __NOD_DISC_WII__
#define __NOD_DISC_WII__

#include "DiscBase.hpp"

namespace NOD
{

class DiscWii : public DiscBase
{
public:
    DiscWii(std::unique_ptr<IDiscIO>&& dio);
    DiscWii(const SystemChar* dataPath, const SystemChar* updatePath,
            const SystemChar* outPath, const char gameID[6], const char* gameTitle,
            bool korean=false);
};

}

#endif // __NOD_DISC_WII__
