#ifndef EPIC_DAG_SERVICE_H
#define EPIC_DAG_SERVICE_H
#include <unordered_set>

#include "block.h"
#include "utxo.h"

class OnLvsConfirmedInterface {
public:
    virtual void OnLvsConfirmed(std::vector<RecordPtr> blocks,
                                std::unordered_set<UTXOPtr> UTXOs,
                                std::unordered_set<uint256> STXOs) = 0;
};

using OnLvsConfirmedListener = std::shared_ptr<OnLvsConfirmedInterface>;
#endif // EPIC_DAG_SERVICE_H
