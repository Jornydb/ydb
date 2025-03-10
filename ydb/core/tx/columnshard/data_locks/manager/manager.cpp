#include "manager.h"
#include <ydb/library/actors/core/log.h>

namespace NKikimr::NOlap::NDataLocks {

void TManager::RegisterLock(const std::shared_ptr<ILock>& lock) {
    AFL_VERIFY(lock);
    AFL_VERIFY(ProcessLocks.emplace(lock->GetLockName(), lock).second)("process_id", lock->GetLockName());
}

void TManager::UnregisterLock(const TString& processId) {
    AFL_VERIFY(ProcessLocks.erase(processId))("process_id", processId);
}

std::optional<TString> TManager::IsLocked(const TPortionInfo& portion) const {
    for (auto&& i : ProcessLocks) {
        if (auto lockName = i.second->IsLocked(portion)) {
            return lockName;
        }
    }
    return {};
}

std::optional<TString> TManager::IsLocked(const TGranuleMeta& granule) const {
    for (auto&& i : ProcessLocks) {
        if (auto lockName = i.second->IsLocked(granule)) {
            return lockName;
        }
    }
    return {};
}

}