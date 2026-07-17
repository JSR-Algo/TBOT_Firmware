#ifndef SSID_MANAGER_H
#define SSID_MANAGER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct SsidItem {
    std::string ssid;
    std::string password;
};

class SsidManager {
public:
    static SsidManager& GetInstance() {
        static SsidManager instance;
        return instance;
    }

    void AddSsid(const std::string& ssid, const std::string& password);
    uint32_t BeginSsidTransaction(const std::string& ssid, const std::string& password);
    bool CommitSsidTransaction(uint32_t transaction_id);
    bool RollbackSsidTransaction(uint32_t transaction_id);
    void RemoveSsid(int index);
    void SetDefaultSsid(int index);
    void Clear();
    std::vector<SsidItem> GetSsidList() const;

private:
    SsidManager();
    ~SsidManager();

    void LoadFromNvs();
    bool SaveToNvs();
    void UpsertSsid(const std::string& ssid, const std::string& password);
    void RestoreActiveTransaction();
    void ClearTransactionBackup();
    static void SecureClearString(std::string& value);

    std::vector<SsidItem> ssid_list_;
    mutable std::mutex transaction_mutex_;
    std::atomic<uint32_t> next_transaction_id_{0};
    uint32_t active_transaction_id_ = 0;
    bool transaction_matched_existing_ = false;
    size_t transaction_match_index_ = 0;
    std::string transaction_old_password_;
    bool transaction_inserted_ = false;
    bool transaction_evicted_tail_ = false;
    SsidItem transaction_evicted_item_;
};

#endif // SSID_MANAGER_H
